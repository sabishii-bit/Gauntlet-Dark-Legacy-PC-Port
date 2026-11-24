//! A fresh install's work in its folder, phase by phase: the folder judged again, then made with
//! the launcher's state folder, locked, its staging made and its journal begun; the runtime, the
//! disc's trees and the launcher's copy staged, each file synced and closed before the next is
//! made; the staged game probed; then every root put in place and the receipt written, the point
//! past which the installation stands and nothing is taken back.

use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{self, Read, Seek};
use std::path::Path;

#[cfg(windows)]
use gdl_files::read_bounded;
use gdl_files::{
    Cancelled, FilesError, Job, PhaseId, Progress, RelPath, RunningTotal, Staging, is_absent,
    no_links, refuses_writes,
};
#[cfg(windows)]
use gdl_media::MAX_TPL_LEN;
use gdl_media::{DiscEntryKind, GameCubeImage, MediaError, Sha256Digest};
use gdl_release::Platform;
use serde_json::Map;

use super::disc_files::{CopiedFile, DISC_FILES, DiscFiles};
use super::journal::{InstallJournal, JournalFile};
use super::settings::{FaultPoint, InstallSettings};
use super::{
    CHECK_DISC, CHECK_GAME, CONFIG_FOLDER, COPY_DISC_FILES, COPY_LAUNCHER, Changes, FolderRecord,
    GET_RUNTIME, INSTALL_STAGING_PREFIX, InstallError, InstallNotice, InstallPlan, Installed,
    PREPARE, PUBLISH, SAVES_FOLDER, install_name,
};
#[cfg(windows)]
use super::{PERSONALISE, icon_entry};
use crate::compat::MIN_LAUNCHER;
use crate::destination::{Blocker, judge_destination};
use crate::folder::{
    FolderError, InstallFolder, InstallLock, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE,
    STATE_DIR,
};
#[cfg(windows)]
use crate::icon::{DISC_ICON, Personalised};
use crate::payload::PayloadError;
use crate::probe::GameStartError;
use crate::receipt::{
    DiscIcon, GUNE5D, InstalledLauncher, RECEIPT_SCHEMA, Receipt, installed_launcher_name,
};
use crate::sections::{Sections, SectionsError};

/// The mode of the game's executable and the launcher's copy, which run as programs.
#[cfg(unix)]
const PROGRAM_MODE: u32 = 0o755;

/// The mode of every other file of the runtime.
#[cfg(unix)]
const FILE_MODE: u32 = 0o644;

/// What an install has made of its own so far, which tells what a failure leaves.
#[derive(Clone, Copy, Debug, Default)]
struct Footprint {
    /// Whether the record of installations was told of the destination.
    recorded: bool,
    /// How many folders the install made: the destination and the missing folders above it.
    made_folders: u32,
    /// Whether the install made its state folder.
    made_state: bool,
}

/// How far an install's work has come, in bytes: the sections read, then the runtime, the disc
/// and the launcher's copy written.
#[derive(Clone, Copy, Debug)]
struct Tally {
    /// The bytes done.
    done: u64,
    /// The bytes of every part of the install.
    total: u64,
}

impl Tally {
    /// The part of the install's progress that begins where the work has come to.
    fn part<'p>(&self, job: &Job<'p>) -> RunningTotal<'p> {
        RunningTotal {
            job: job.progress,
            before: self.done,
            total: self.total,
        }
    }
}

/// What the install keeps of the runtime once its files are staged: what the receipt records.
#[derive(Clone, Debug)]
struct RuntimeRecord {
    /// The release's version.
    version: String,
    /// The commit it was built from.
    commit: String,
    /// The platform it was built for.
    platform: Platform,
    /// The game's executable, by its name in the runtime.
    executable: String,
    /// The executable's digest as the release lists it, which the receipt keeps once the disc's
    /// icon is set into the executable.
    #[cfg(windows)]
    executable_sha256: Sha256Digest,
    /// The digest of the release record, `build-info.json`, as the runtime holds it.
    build_info_sha256: Sha256Digest,
}

/// What the install holds once its folder is prepared: the folder, its lock, the staging and the
/// journal that names it.
struct Prepared<'a> {
    /// The installation's folder.
    folder: InstallFolder,
    /// Its lock, held until the job ends.
    _lock: InstallLock,
    /// Where the install makes what it will publish, kept for the journal that names it.
    staging: Staging,
    /// The install's journal.
    journal: JournalFile<'a>,
}

/// A fresh install of a plan, as it runs.
pub(super) struct Install<'a> {
    /// The install planned.
    plan: &'a InstallPlan,
    /// How the job is asked to stop and where its progress goes.
    job: Job<'a>,
    /// What the install asks of the world beyond its folder.
    settings: &'a InstallSettings<'a>,
    /// What it has made so far.
    footprint: Footprint,
}

impl<'a> Install<'a> {
    /// An install of `plan`, not yet begun.
    pub(super) fn new(
        plan: &'a InstallPlan,
        job: Job<'a>,
        settings: &'a InstallSettings<'a>,
    ) -> Install<'a> {
        Install {
            plan,
            job,
            settings,
            footprint: Footprint::default(),
        }
    }

    /// What a failure of the install leaves: nothing, when it failed before the record of
    /// installations was told of the folder, or when nothing was made since, in which case the
    /// record forgets the folder again, a failure to forget it passed over; otherwise what it
    /// made, kept for the next start.
    pub(super) fn left(&self, record: &mut dyn FolderRecord) -> Changes {
        let footprint = self.footprint;
        if !footprint.recorded {
            return Changes::None;
        }
        if footprint.made_folders == 0 && !footprint.made_state {
            let _ = record.forget(&self.plan.destination);
            return Changes::None;
        }
        Changes::Kept
    }

    /// Runs the install, phase by phase, telling each phase first and then looking at the cancel,
    /// but for the publish, which is never cancelled.
    pub(super) fn run<R: Read + Seek>(
        &mut self,
        image: &mut GameCubeImage,
        launcher: &mut Sections<R>,
        record: &mut dyn FolderRecord,
    ) -> Result<Installed, InstallError> {
        self.check_disc()?;
        let prepared = self.prepare(record)?;
        let sections: u64 = launcher.sections().iter().map(|section| section.size).sum();
        let mut tally = Tally {
            done: 0,
            total: sections
                .saturating_add(self.plan.runtime.bytes)
                .saturating_add(self.plan.disc.bytes)
                .saturating_add(launcher.file_len()),
        };
        let runtime = self.get_runtime(launcher, &prepared.staging, sections, &mut tally)?;
        self.copy_disc_files(image, &prepared, &mut tally)?;
        #[cfg(windows)]
        let icon = self.personalise(image, &prepared.staging, &runtime)?;
        // Only Windows shows a program by an icon it carries.
        #[cfg(not(windows))]
        let icon = SetIcon::default();
        let launcher_sha256 =
            self.copy_launcher(launcher, &prepared.staging, runtime.platform, &mut tally)?;
        let mut notices = Vec::new();
        self.check_game(&prepared.staging, &runtime, &mut notices)?;
        let receipt = receipt_of(image, &runtime, launcher_sha256, icon);
        self.publish(prepared, receipt, &runtime, record, notices, tally)
    }

    /// Tells `phase` and looks at the cancel.
    fn begin(&self, phase: PhaseId) -> Result<(), InstallError> {
        self.job.progress.phase(phase);
        self.check_cancel()
    }

    /// Fails with [`InstallError::Cancelled`] once the job has been asked to stop.
    fn check_cancel(&self) -> Result<(), InstallError> {
        self.job
            .cancel
            .check()
            .map_err(|Cancelled| InstallError::Cancelled)
    }

    /// Passes `point`, a failure there reported as one at `at`, what the step before it made.
    fn pass(&self, point: FaultPoint, at: &Path) -> Result<(), InstallError> {
        self.settings
            .faults
            .pass(point)
            .map_err(|source| InstallError::Io {
                path: at.to_path_buf(),
                source,
            })
    }

    /// The install's failure for what a file operation reported: a link on the destination's
    /// path, the destination itself or a folder above it, refuses the folder as the judging
    /// refuses it; anything else, a link inside the install's own folders among it, is the
    /// operation's own failure.
    fn files_error(&self, error: FilesError) -> InstallError {
        match error {
            FilesError::Link { path } if self.plan.destination.starts_with(&path) => {
                InstallError::Blocked(Blocker::Links { path })
            }
            other => InstallError::Files(other),
        }
    }

    /// The install's failure for why its lock was not taken: a link on the destination's path as
    /// [`files_error`](Self::files_error) takes one, anything else as the lock's own failure.
    fn lock_error(&self, error: LockError) -> InstallError {
        match error {
            LockError::Links { path } if self.plan.destination.starts_with(&path) => {
                InstallError::Blocked(Blocker::Links { path })
            }
            other => InstallError::Lock(other),
        }
    }

    /// Judges the destination again, as the plan judged it, for the plan's roots and the state
    /// folder: the folder may have changed since. The host's finding, which cannot have, is taken
    /// from the plan, before the folder's. The first finding refuses the install.
    fn check_disc(&self) -> Result<(), InstallError> {
        self.begin(CHECK_DISC)?;
        let plan = self.plan;
        let mut names = plan.roots.clone();
        names.push(install_name(STATE_DIR));
        let report = judge_destination(&plan.destination, &names, plan.needed_bytes)
            .map_err(InstallError::Destination)?;
        let host = plan
            .blockers
            .iter()
            .filter(|blocker| matches!(blocker, Blocker::Unsupported { .. }));
        match host.chain(&report.blockers).next() {
            Some(blocker) => Err(InstallError::Blocked(blocker.clone())),
            None => Ok(()),
        }
    }

    /// Makes the folder ready to stage into: tells the record of installations of it, makes it
    /// and the missing folders above it, makes the launcher's state folder in it, which no other
    /// job may have made, takes the installation's lock, judges the folder again under the lock,
    /// then makes the journal folder and the staging, and writes the journal, which names the
    /// staging, kept from then on for the journal's sake.
    fn prepare(&mut self, record: &mut dyn FolderRecord) -> Result<Prepared<'a>, InstallError> {
        self.begin(PREPARE)?;
        let plan = self.plan;
        let destination = &plan.destination;
        record
            .pending(destination)
            .map_err(|source| InstallError::Record { source })?;
        self.footprint.recorded = true;
        self.make_destination()?;
        let state = destination.join(STATE_DIR);
        match fs::create_dir(&state) {
            Ok(()) => self.footprint.made_state = true,
            // Another launcher installs into the folder, or did before this job began.
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
                return Err(InstallError::Blocked(Blocker::Collision {
                    names: vec![STATE_DIR.to_owned()],
                }));
            }
            Err(error) if refuses_writes(&error) => {
                return Err(InstallError::Blocked(Blocker::NotWritable {
                    path: destination.clone(),
                }));
            }
            Err(source) => {
                return Err(InstallError::Io {
                    path: state,
                    source,
                });
            }
        }
        self.pass(FaultPoint::Prepare, &state)?;
        let folder = InstallFolder::at(destination).map_err(folder_error)?;
        let lock = folder.lock().map_err(|error| self.lock_error(error))?;
        self.pass(FaultPoint::Prepare, &state.join(LOCK_FILE))?;
        self.judge_again()?;
        let journals = state.join(JOURNAL_DIR);
        fs::create_dir(&journals).map_err(|source| InstallError::Io {
            path: journals.clone(),
            source,
        })?;
        let mut staging = Staging::create_in(&state, INSTALL_STAGING_PREFIX)
            .map_err(|error| self.files_error(error))?;
        let journal = InstallJournal::staging(self.footprint.made_folders, staging.name());
        let journal = JournalFile::begin(&journals, journal, self.settings.faults)
            .map_err(|error| self.files_error(error))?;
        staging.keep();
        self.pass(FaultPoint::Prepare, journal.path())?;
        Ok(Prepared {
            folder,
            _lock: lock,
            staging,
            journal,
        })
    }

    /// Makes the destination and each missing folder above it, outermost first, counting those
    /// made. A folder that another made meanwhile is not the install's, and neither is any above
    /// it, so the count starts again below it.
    fn make_destination(&mut self) -> Result<(), InstallError> {
        let plan = self.plan;
        let mut missing = Vec::new();
        for folder in plan.destination.ancestors() {
            match fs::symlink_metadata(folder) {
                Ok(_) => break,
                Err(error) if is_absent(&error) => missing.push(folder),
                Err(source) => {
                    return Err(InstallError::Io {
                        path: folder.to_path_buf(),
                        source,
                    });
                }
            }
        }
        for folder in missing.into_iter().rev() {
            match fs::create_dir(folder) {
                Ok(()) => self.footprint.made_folders += 1,
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
                    self.footprint.made_folders = 0;
                }
                Err(error) if refuses_writes(&error) => {
                    return Err(InstallError::Blocked(Blocker::NotWritable {
                        path: folder.parent().unwrap_or(folder).to_path_buf(),
                    }));
                }
                Err(source) => {
                    return Err(InstallError::Io {
                        path: folder.to_path_buf(),
                        source,
                    });
                }
            }
        }
        Ok(())
    }

    /// Judges the folder again under the lock, for links on its path and for the roots it holds,
    /// since either could have changed while the lock was not held. The state folder the install
    /// has just made is left out.
    fn judge_again(&self) -> Result<(), InstallError> {
        let plan = self.plan;
        let report = judge_destination(&plan.destination, &plan.roots, 0)
            .map_err(InstallError::Destination)?;
        for blocker in report.blockers {
            match blocker {
                Blocker::Collision { mut names } => {
                    names.retain(|name| name != STATE_DIR);
                    if !names.is_empty() {
                        return Err(InstallError::Blocked(Blocker::Collision { names }));
                    }
                }
                other => return Err(InstallError::Blocked(other)),
            }
        }
        Ok(())
    }

    /// Reads every section of the launcher file against its table, then stages every file of the
    /// runtime it carries, in its archive's order, each checked against the release record as it
    /// is written and synced. On Unix the game's executable is made a program and every other file
    /// readable by all. What the receipt needs of the runtime is kept, and the runtime let go of.
    fn get_runtime<R: Read + Seek>(
        &self,
        launcher: &mut Sections<R>,
        staging: &Staging,
        sections: u64,
        tally: &mut Tally,
    ) -> Result<RuntimeRecord, InstallError> {
        self.begin(GET_RUNTIME)?;
        let told = tally.part(&self.job);
        launcher
            .verify(&Job {
                cancel: self.job.cancel,
                progress: &told,
            })
            .map_err(sections_error)?;
        tally.done = tally.done.saturating_add(sections);
        let mut payload = launcher.runtime().map_err(payload_error)?;
        let info = payload.build_info().clone();
        let entries = payload.entries().to_vec();
        for entry in &entries {
            let mut file = staging
                .create_file(&entry.name)
                .map_err(|error| self.files_error(error))?;
            let told = tally.part(&self.job);
            let copied = payload
                .copy_entry(
                    entry.index,
                    &mut file,
                    &Job {
                        cancel: self.job.cancel,
                        progress: &told,
                    },
                )
                .map_err(payload_error)?;
            if copied == 0 {
                told_empty(&told, &entry.name);
            }
            #[cfg(unix)]
            set_mode(
                &file,
                if entry.name.as_str() == info.executable {
                    PROGRAM_MODE
                } else {
                    FILE_MODE
                },
            )
            .map_err(|source| InstallError::Io {
                path: entry.name.path_under(staging.path()),
                source,
            })?;
            close_synced(file, &entry.name.path_under(staging.path()))?;
            tally.done = tally.done.saturating_add(copied);
        }
        let runtime = RuntimeRecord {
            // A release record always lists its platform's executable.
            #[cfg(windows)]
            executable_sha256: info.files[&info.executable],
            version: info.version,
            commit: info.commit,
            platform: info.platform,
            executable: info.executable,
            build_info_sha256: payload.build_info_sha256(),
        };
        drop(payload);
        self.pass(FaultPoint::Stage, staging.path())?;
        Ok(runtime)
    }

    /// Stages the disc's `Gauntlet` and `carddemo` trees, every entry in the file table's order:
    /// a folder made, its empty ones included, a file copied and synced. The cancel is looked at
    /// before every entry, as the installer looks, and a file of no bytes, which no block tells,
    /// is told once as it is made, so that every file the install makes is named in its progress.
    /// Then the list of what was copied is written into the state folder.
    fn copy_disc_files(
        &self,
        image: &mut GameCubeImage,
        prepared: &Prepared<'_>,
        tally: &mut Tally,
    ) -> Result<(), InstallError> {
        self.begin(COPY_DISC_FILES)?;
        let staging = &prepared.staging;
        let entries = image.entries().to_vec();
        let mut folders = Vec::new();
        let mut files = Vec::new();
        for entry in &entries {
            self.check_cancel()?;
            let DiscEntryKind::File { size, .. } = entry.kind else {
                staging
                    .create_dir(&entry.path)
                    .map_err(|error| self.files_error(error))?;
                folders.push(entry.path.as_str().to_owned());
                continue;
            };
            let mut file = staging
                .create_file(&entry.path)
                .map_err(|error| self.files_error(error))?;
            let told = tally.part(&self.job);
            let sha256 = image
                .copy(
                    entry,
                    &mut file,
                    &Job {
                        cancel: self.job.cancel,
                        progress: &told,
                    },
                )
                .map_err(media_error)?;
            if size == 0 {
                told_empty(&told, &entry.path);
            }
            close_synced(file, &entry.path.path_under(staging.path()))?;
            tally.done = tally.done.saturating_add(size);
            files.push(CopiedFile::new(entry.path.as_str(), size, sha256));
        }
        let state = prepared.folder.state_dir();
        DiscFiles::listing(folders, files)
            .write(&state)
            .map_err(|error| self.files_error(error))?;
        self.pass(FaultPoint::Stage, &state.join(DISC_FILES))
    }

    /// Sets the disc's icon, read from its staged copy no further than a TPL may hold, into the
    /// staged game's executable, and gives what the receipt records of it.
    #[cfg(windows)]
    fn personalise(
        &self,
        image: &GameCubeImage,
        staging: &Staging,
        runtime: &RuntimeRecord,
    ) -> Result<SetIcon, InstallError> {
        self.begin(PERSONALISE)?;
        let icon = match icon_entry(image.entries()) {
            Some(entry) => entry.path.path_under(staging.path()),
            None => staging.path().join(DISC_ICON),
        };
        let tpl = read_bounded(&icon, MAX_TPL_LEN).map_err(|error| self.files_error(error))?;
        let executable = staging.path().join(&runtime.executable);
        let personalised =
            (self.settings.personalise)(&executable, &tpl).map_err(InstallError::Icon)?;
        self.pass(FaultPoint::Stage, &executable)?;
        Ok(SetIcon::of(runtime, &personalised))
    }

    /// Makes the folders the game keeps the player's saves and settings in, then copies the whole
    /// launcher file, through the handle its sections were found on, as the installation's copy of
    /// the launcher, a program on Unix, synced; and gives the copy's digest.
    fn copy_launcher<R: Read + Seek>(
        &self,
        launcher: &mut Sections<R>,
        staging: &Staging,
        platform: Platform,
        tally: &mut Tally,
    ) -> Result<Sha256Digest, InstallError> {
        self.begin(COPY_LAUNCHER)?;
        for folder in [SAVES_FOLDER, CONFIG_FOLDER] {
            staging
                .create_dir(&install_name(folder))
                .map_err(|error| self.files_error(error))?;
        }
        let name = install_name(installed_launcher_name(platform));
        let path = name.path_under(staging.path());
        let mut file = staging
            .create_file(&name)
            .map_err(|error| self.files_error(error))?;
        let told = tally.part(&self.job);
        let sha256 = launcher
            .copy_file(
                &mut file,
                &Job {
                    cancel: self.job.cancel,
                    progress: &told,
                },
            )
            .map_err(sections_error)?;
        #[cfg(unix)]
        set_mode(&file, PROGRAM_MODE).map_err(|source| InstallError::Io {
            path: path.clone(),
            source,
        })?;
        close_synced(file, &path)?;
        tally.done = tally.done.saturating_add(launcher.file_len());
        self.pass(FaultPoint::Stage, &path)?;
        Ok(sha256)
    }

    /// Runs the staged game with `--version`, once its executable is seen to be a regular file
    /// reached through no link, so that nothing planted at its name is run. A game that does not
    /// answer is a notice, and the install goes on; a cancel stops the game and the install.
    fn check_game(
        &self,
        staging: &Staging,
        runtime: &RuntimeRecord,
        notices: &mut Vec<InstallNotice>,
    ) -> Result<(), InstallError> {
        self.begin(CHECK_GAME)?;
        let executable = staging.path().join(&runtime.executable);
        no_links(&executable).map_err(|error| self.files_error(error))?;
        match fs::symlink_metadata(&executable) {
            Ok(metadata) if metadata.is_file() => {}
            Ok(_) => {
                return Err(InstallError::Io {
                    path: executable,
                    source: io::Error::new(io::ErrorKind::InvalidInput, "not a regular file"),
                });
            }
            Err(source) => {
                return Err(InstallError::Io {
                    path: executable,
                    source,
                });
            }
        }
        match (self.settings.probe)(&executable, self.job.cancel) {
            Ok(probe) if probe.succeeded() => {}
            Ok(probe) => notices.push(InstallNotice::ProbeFailed {
                status: probe.status.code(),
                first_line: probe.first_line,
                stderr_tail: probe.stderr_tail,
            }),
            Err(GameStartError::Cancelled) => return Err(InstallError::Cancelled),
            Err(_) => notices.push(InstallNotice::ProbeFailed {
                status: None,
                first_line: None,
                stderr_tail: String::new(),
            }),
        }
        self.pass(FaultPoint::Stage, &executable)
    }

    /// Puts every root in place and writes the receipt, never cancelled. The staged tree is synced
    /// first and the journal moved to publishing with the roots and the receipt, so that the moves
    /// can be finished from it; each root is then recorded as it moves. The receipt on disk is the
    /// commit point: a failure before it is the install's, and from it on the installation stands,
    /// so that a failure to clean up is only a notice, which the next start finishes. The record
    /// of installations is then told, and the shell, and the progress reaches its total.
    fn publish(
        &self,
        prepared: Prepared<'a>,
        receipt: Receipt,
        runtime: &RuntimeRecord,
        record: &mut dyn FolderRecord,
        mut notices: Vec<InstallNotice>,
        tally: Tally,
    ) -> Result<Installed, InstallError> {
        self.job.progress.phase(PUBLISH);
        let plan = self.plan;
        let Prepared {
            folder,
            _lock,
            staging,
            mut journal,
        } = prepared;
        staging
            .sync_folders()
            .map_err(|error| self.files_error(error))?;
        journal
            .publishing(&plan.roots, &receipt)
            .map_err(|error| self.files_error(error))?;
        self.pass(FaultPoint::Publish, journal.path())?;
        staging
            .publish_new(&plan.roots, &plan.destination, &mut journal)
            .map_err(|error| self.files_error(error))?;
        self.pass(FaultPoint::Publish, journal.path())?;
        let written = receipt.write(&folder);
        // Whether the receipt is in place is read from the disc: a write can fail after its
        // rename, in putting the rename on disk, with the receipt there.
        let receipt_path = folder.state_dir().join(RECEIPT_FILE);
        if !fs::symlink_metadata(&receipt_path).is_ok_and(|metadata| metadata.is_file()) {
            return Err(match written {
                Err(error) => self.files_error(error),
                Ok(()) => InstallError::Io {
                    path: receipt_path,
                    source: io::Error::from(io::ErrorKind::NotFound),
                },
            });
        }
        if let Err(error) = self.clean_up(staging, &mut journal) {
            notices.push(InstallNotice::CleanupLeft {
                detail: error.to_string().into(),
            });
        }
        if let Err(error) = record.installed(&plan.destination) {
            notices.push(InstallNotice::NotRecorded {
                detail: error.to_string().into(),
            });
        }
        (self.settings.notify_shell)(&plan.destination.join(&runtime.executable));
        self.job.progress.advance(tally.total, tally.total, None);
        Ok(Installed {
            folder,
            receipt,
            notices,
        })
    }

    /// Cleans up once the receipt is in place: the journal moved to committed, then the staging
    /// removed, then the journal, last, so that whichever step a crash cuts short, the journal is
    /// left for the next start to finish from, or nothing is. A staging or a journal already gone
    /// counts as removed.
    fn clean_up(
        &self,
        staging: Staging,
        journal: &mut JournalFile<'_>,
    ) -> Result<(), InstallError> {
        self.pass(FaultPoint::Publish, journal.path())?;
        journal
            .committed()
            .map_err(|error| self.files_error(error))?;
        self.pass(FaultPoint::Cleanup, staging.path())?;
        let removed = staging.path().to_path_buf();
        match staging.remove() {
            Ok(()) => {}
            Err(FilesError::Io { source, .. }) if is_absent(&source) => {}
            Err(error) => return Err(self.files_error(error)),
        }
        self.pass(FaultPoint::Cleanup, &removed)?;
        journal.remove().map_err(|error| self.files_error(error))
    }
}

/// What the receipt records of the icon set into the game's executable: the executable's digest
/// with the icon in it, in place of the one the release lists, and where the icon came from. An
/// install that sets no icon records neither.
#[derive(Clone, Debug, Default)]
struct SetIcon {
    /// The digest of each installed file whose bytes are not the ones the release lists, by its
    /// name there.
    installed_digests: BTreeMap<String, String>,
    /// Where the icon came from, and what the executable was before.
    disc_icon: Option<DiscIcon>,
}

#[cfg(windows)]
impl SetIcon {
    /// What the receipt records of the executable of `runtime`, `personalised`.
    fn of(runtime: &RuntimeRecord, personalised: &Personalised) -> SetIcon {
        SetIcon {
            installed_digests: BTreeMap::from([(
                runtime.executable.clone(),
                personalised.executable_sha256.hex(),
            )]),
            disc_icon: Some(DiscIcon {
                path: DISC_ICON.to_owned(),
                sha256: personalised.icon_sha256.hex(),
                release_executable_sha256: runtime.executable_sha256.hex(),
                other: Map::new(),
            }),
        }
    }
}

/// The receipt of an install of the disc in `image` and of `runtime`, whose launcher copy has the
/// digest `launcher_sha256`, with what `icon` records of the icon set into the executable.
fn receipt_of(
    image: &GameCubeImage,
    runtime: &RuntimeRecord,
    launcher_sha256: Sha256Digest,
    icon: SetIcon,
) -> Receipt {
    Receipt {
        schema: RECEIPT_SCHEMA,
        min_launcher: MIN_LAUNCHER.to_owned(),
        version: runtime.version.clone(),
        commit: runtime.commit.clone(),
        platform: runtime.platform.as_str().to_owned(),
        disc: GUNE5D.to_owned(),
        disc_revision: image.revision(),
        build_info_sha256: runtime.build_info_sha256.hex(),
        installed_digests: icon.installed_digests,
        disc_icon: icon.disc_icon,
        launcher: InstalledLauncher {
            name: installed_launcher_name(runtime.platform).to_owned(),
            sha256: launcher_sha256.hex(),
            other: Map::new(),
        },
        other: Map::new(),
    }
}

/// Tells `told` that the file `name`, which holds no bytes and so had no block to tell, is made.
fn told_empty(told: &RunningTotal<'_>, name: &RelPath) {
    told.advance(0, 0, Some(name));
}

/// Writes the staged file `file`, at `path`, through to its disk, then closes it, before the next
/// is made.
fn close_synced(file: File, path: &Path) -> Result<(), InstallError> {
    file.sync_all().map_err(|source| InstallError::Io {
        path: path.to_path_buf(),
        source,
    })
}

/// Gives the open file `file` the mode `mode`.
#[cfg(unix)]
fn set_mode(file: &File, mode: u32) -> io::Result<()> {
    use std::os::unix::fs::PermissionsExt;

    file.set_permissions(fs::Permissions::from_mode(mode))
}

/// The install's failure for why the folder it made could not be taken as an installation's: a
/// link or a file on its path refuses the folder as the judging refuses it.
fn folder_error(error: FolderError) -> InstallError {
    match error {
        FolderError::Links { path } => InstallError::Blocked(Blocker::Links { path }),
        FolderError::NotDirectory { path } => InstallError::Blocked(Blocker::NotDirectory { path }),
        FolderError::Absent { path } => InstallError::Io {
            path,
            source: io::Error::from(io::ErrorKind::NotFound),
        },
        FolderError::Io { path, source } => InstallError::Io { path, source },
    }
}

/// The install's failure for what reading the launcher file's sections reported.
fn sections_error(error: SectionsError) -> InstallError {
    match error {
        SectionsError::Cancelled => InstallError::Cancelled,
        other => InstallError::Launcher(other),
    }
}

/// The install's failure for what reading the runtime reported.
fn payload_error(error: PayloadError) -> InstallError {
    match error {
        PayloadError::Cancelled | PayloadError::Sections(SectionsError::Cancelled) => {
            InstallError::Cancelled
        }
        other => InstallError::Runtime(other),
    }
}

/// The install's failure for what reading the disc's image reported.
fn media_error(error: MediaError) -> InstallError {
    match error {
        MediaError::Cancelled => InstallError::Cancelled,
        other => InstallError::Image(other),
    }
}
