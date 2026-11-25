//! An update's checking and staging, under the installation's lock: the source read against its
//! table and its names judged; every installed file and the launcher copy hashed again against what
//! the installation records; the room measured and the game found closed; then, in a staging folder
//! of the state folder's, each new or changed file, the files the game's probe runs beside them,
//! the disc's icon on Windows and the launcher copy, each synced and closed before the next is
//! made, and the journal that lists every move; then the staged game probed. A failure or a cancel
//! takes back what was staged, and the installation is as it was.

use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io;
use std::path::{Path, PathBuf};

#[cfg(windows)]
use gdl_files::read_bounded;
use gdl_files::{
    BLOCK_LEN, FilesError, Job, PAYLOAD_NAMES, PhaseId, Progress, RelPath, RunningTotal, Staging,
    fill_from, is_absent, no_links,
};
#[cfg(windows)]
use gdl_media::MAX_TPL_LEN;
use gdl_media::Sha256Digest;
use gdl_release::Version;
#[cfg(windows)]
use serde_json::Map;
use sha2::{Digest, Sha256};

use super::journal::{
    JournalFile, PLANNED_STAGING, Planned, UPDATE_STAGING_PREFIX, UpdatePhase, check_journal_len,
};
use super::source::verified_runtime;
use super::swap::{discard, folders_to_make};
use super::{
    FileChanges, Probed, STAGE, Update, UpdateError, UpdateFailure, UpdateNotice, VERIFY_DOWNLOAD,
    files_error, judge_names, payload_error, probe_copies, sections_error, stages_executable,
    unchanged,
};
use crate::faults::FaultPoint;
use crate::folder::{JOURNAL_DIR, RECEIPT_FILE};
use crate::hold::{HeldGame, game_closed_holding};
#[cfg(windows)]
use crate::icon::{DISC_ICON, Personalised};
use crate::install::{CHECK_GAME, Changes, SPACE_MARGIN};
use crate::payload::{BUILD_INFO, BuildInfo, Payload};
use crate::probe::GameStartError;
#[cfg(windows)]
use crate::receipt::DiscIcon;
use crate::receipt::{MAX_RECEIPT_LEN, Receipt, installed_launcher_name};
use crate::sections::{SectionView, Sections};

/// The mode of the game's executable and the launcher copy, which run as programs.
#[cfg(unix)]
const PROGRAM_MODE: u32 = 0o755;

/// The mode of every other file of the runtime.
#[cfg(unix)]
const FILE_MODE: u32 = 0o644;

/// How far the staging's work has come, in bytes: the installed files hashed again, then the
/// runtime's files and the launcher copy written.
#[derive(Clone, Copy, Debug)]
pub(super) struct Tally {
    /// The bytes done.
    done: u64,
    /// The bytes of every part of the staging.
    pub(super) total: u64,
}

impl Tally {
    /// The part of the job's progress that begins where the work has come to.
    fn part<'p>(&self, job: &Job<'p>) -> RunningTotal<'p> {
        RunningTotal {
            job: job.progress,
            before: self.done,
            total: self.total,
        }
    }
}

/// An installed file an update hashes again before it changes anything.
struct Owned {
    /// Its name under the installation's top.
    name: RelPath,
    /// Where it is.
    path: PathBuf,
    /// The digest the installation records for it: the receipt's for a file installed with other
    /// bytes than the release's, the release record's otherwise; `None` for a digest the receipt
    /// gives in no form a digest takes.
    expected: Option<Sha256Digest>,
    /// How many bytes it holds.
    len: u64,
}

/// What an update has staged, ready to swap in: the staging and its journal, the names that move,
/// the receipt it writes and what it found.
pub(super) struct Staged {
    /// Where the new files and the launcher copy are staged, kept for the journal that names it.
    pub(super) staging: Staging,
    /// The update's journal, at staged.
    pub(super) journal: JournalFile,
    /// The runtime names the update moves.
    pub(super) changes: FileChanges,
    /// The release the update brings.
    pub(super) to: Version,
    /// The receipt the update writes.
    pub(super) receipt: Receipt,
    /// Whether the executable is staged to be moved into place, rather than left where it is.
    pub(super) moves_executable: bool,
    /// Whether no launcher copy was there to move aside.
    pub(super) launcher_missing: bool,
    /// Whether the update ran the game's probe.
    pub(super) probed: Probed,
    /// What went wrong without stopping the update.
    pub(super) notices: Vec<UpdateNotice>,
    /// How far the staging's progress has come.
    pub(super) tally: Tally,
}

/// What the staging found of the new release before the staging folder was made.
struct Judged {
    /// The runtime names the update moves.
    changes: FileChanges,
    /// The names staged beside them for the game's probe.
    copies: Vec<RelPath>,
    /// The new release's record.
    record: BuildInfo,
    /// The digest of the new release's record, as the runtime holds it.
    record_sha256: Sha256Digest,
}

impl Update<'_> {
    /// Reads the source and judges the names it moves, hashes the installed files again, measures
    /// the room, waits for the game to close, stages what the update puts in place and writes the
    /// journal, then probes the staged game. Whatever stops it takes back what it staged.
    pub(super) fn stage(&self, sections: &mut Sections<File>) -> Result<Staged, UpdateFailure> {
        let file_len = sections.file_len();
        self.begin(VERIFY_DOWNLOAD).map_err(unchanged)?;
        let mut payload = verified_runtime(sections, self.running, &self.job).map_err(unchanged)?;
        let judged = self.judge(&payload).map_err(unchanged)?;
        self.pass(FaultPoint::UpdateStage, self.folder.root())
            .map_err(unchanged)?;
        self.begin(STAGE).map_err(unchanged)?;
        let executable = &judged.record.executable;
        let backup = judged.changes.backup();
        let publish = judged.changes.publish();
        let moves_executable = publish.iter().any(|name| name.as_str() == executable);
        let to_stage: BTreeSet<&RelPath> = publish.iter().chain(&judged.copies).collect();
        let staged_bytes: u64 = payload
            .entries()
            .iter()
            .filter(|entry| to_stage.contains(&entry.name))
            .map(|entry| entry.size)
            .sum();
        let (owned, launcher_missing) = self.owned_files().map_err(unchanged)?;
        let to = judged.record.version();
        let to_text = to.to_string();
        let from = self.receipt.version.clone();
        let planned = |staging, receipt| Planned {
            from: &from,
            to: &to_text,
            staging,
            backup: &backup,
            publish: &publish,
            platform: self.platform,
            launcher_missing,
            receipt,
        };
        let receipt = self.planned_receipt(&judged, moves_executable);
        // The journal is judged as it is at its largest: rolling back, with the folders the new
        // names need, which the swap records.
        let mut largest = planned(PLANNED_STAGING, &receipt).staged();
        largest.phase = UpdatePhase::RollingBack;
        largest.made_folders = folders_to_make(self.folder.root(), &publish).map_err(unchanged)?;
        self.check_receipt_len(&receipt)
            .and_then(|()| check_journal_len(&largest))
            .map_err(unchanged)?;
        let mut tally = Tally {
            done: 0,
            total: owned
                .iter()
                .map(|file| file.len)
                .sum::<u64>()
                .saturating_add(staged_bytes)
                .saturating_add(file_len),
        };
        self.rehash(&owned, &mut tally).map_err(unchanged)?;
        self.pass(FaultPoint::UpdateStage, self.folder.root())
            .map_err(unchanged)?;
        self.check_space(staged_bytes.saturating_add(file_len))
            .map_err(unchanged)?;
        self.game_closed().map_err(unchanged)?;
        // Looked at as the staging begins too, so that a cancel made as the last file was hashed
        // stops the update before it makes anything.
        self.job
            .cancel
            .check()
            .map_err(|cancelled| unchanged(cancelled.into()))?;
        let state = self.folder.state_dir();
        let mut staging = Staging::create_in(&state, UPDATE_STAGING_PREFIX)
            .map_err(|error| unchanged(files_error(error)))?;
        staging.keep();
        let filled = self
            .stage_files(&staging, &mut payload, &publish, &mut tally)
            .and_then(|()| self.stage_files(&staging, &mut payload, &judged.copies, &mut tally));
        drop(payload);
        let receipt = filled.and_then(|()| {
            self.finish_staging(&staging, sections, &judged, moves_executable, &mut tally)
        });
        let receipt = match receipt {
            Ok(receipt) => receipt,
            Err(error) => return Err(self.taken_back(error, staging, None)),
        };
        let journal = planned(staging.name(), &receipt).staged();
        let journal = match JournalFile::begin(&state.join(JOURNAL_DIR), journal) {
            Ok(journal) => journal,
            Err(error) => return Err(self.taken_back(error, staging, None)),
        };
        if let Err(error) = self.pass(FaultPoint::UpdateStage, journal.path()) {
            return Err(self.taken_back(error, staging, Some(journal)));
        }
        let executable_staged = to_stage.iter().any(|name| name.as_str() == executable);
        let mut notices = Vec::new();
        let probed = match self.check_game(&staging, executable_staged, &mut notices) {
            Ok(probed) => probed,
            Err(error) => return Err(self.taken_back(error, staging, Some(journal))),
        };
        Ok(Staged {
            staging,
            journal,
            changes: judged.changes,
            to,
            receipt,
            moves_executable,
            launcher_missing,
            probed,
            notices,
            tally,
        })
    }

    /// Judges the names the new release in `payload` moves against the installed one's, and the
    /// names staged beside them for the game's probe.
    fn judge(&self, payload: &Payload<SectionView<'_, File>>) -> Result<Judged, UpdateError> {
        let record = payload.build_info().clone();
        let changes = judge_names(
            self.folder.root(),
            &self.installed,
            &record,
            stages_executable(&self.installed, &self.receipt),
        )?;
        let copies = probe_copies(&changes, &record);
        Ok(Judged {
            changes,
            copies,
            record,
            record_sha256: payload.build_info_sha256(),
        })
    }

    /// Tells `phase` and looks at the cancel.
    pub(super) fn begin(&self, phase: PhaseId) -> Result<(), UpdateError> {
        self.job.progress.phase(phase);
        Ok(self.job.cancel.check()?)
    }

    /// Passes `point`, a failure there reported as one at `at`, what the step before it made.
    pub(super) fn pass(&self, point: FaultPoint, at: &Path) -> Result<(), UpdateError> {
        self.settings
            .faults
            .pass(point)
            .map_err(|source| UpdateError::Io {
                path: at.to_path_buf(),
                source,
            })
    }

    /// The update's failure for `error` once its staging was made: the staging goes, with the
    /// journal when it was written, and the installation is as it was; when that fails, what is
    /// left stays for the next start.
    pub(super) fn taken_back(
        &self,
        error: UpdateError,
        staging: Staging,
        journal: Option<JournalFile>,
    ) -> UpdateFailure {
        let changes = match discard(staging, journal.as_ref(), self.settings.faults) {
            Ok(()) => Changes::RolledBack,
            Err(_) => Changes::Kept,
        };
        UpdateFailure { error, changes }
    }

    /// Every file the installation records and the launcher copy, each with the digest the
    /// installation records for it and its length; and whether no launcher copy is there. Each is
    /// looked at without following a link: a file gone, or something other than a file in its
    /// place, refuses the update, but for a launcher copy that is not there, which the update puts
    /// in place.
    fn owned_files(&self) -> Result<(Vec<Owned>, bool), UpdateError> {
        let root = self.folder.root();
        let receipt = &self.receipt;
        let mut owned = Vec::new();
        let names = self
            .installed
            .files
            .keys()
            .map(String::as_str)
            .chain([BUILD_INFO]);
        for text in names {
            let expected = if text == BUILD_INFO {
                Sha256Digest::from_hex(&receipt.build_info_sha256)
            } else {
                match receipt.installed_digests.get(text) {
                    Some(digest) => Sha256Digest::from_hex(digest),
                    None => self.installed.files.get(text).copied(),
                }
            };
            let name = RelPath::parse(text, &PAYLOAD_NAMES)
                .map_err(|_| UpdateError::InstalledNames { name: text.into() })?;
            let path = name.path_under(root);
            let Some(len) = file_len(&path, text)? else {
                return Err(changed(text));
            };
            owned.push(Owned {
                name,
                path,
                expected,
                len,
            });
        }
        let launcher = installed_launcher_name(self.platform);
        let path = root.join(launcher);
        no_links(&path).map_err(files_error)?;
        let launcher_missing = match file_len(&path, launcher)? {
            Some(len) => {
                owned.push(Owned {
                    name: RelPath::parse(launcher, &PAYLOAD_NAMES)
                        .expect("the launcher copy's name keeps the payload's rules"),
                    path,
                    expected: Sha256Digest::from_hex(&receipt.launcher.sha256),
                    len,
                });
                false
            }
            None => true,
        };
        owned.sort_by(|one, other| one.name.cmp(&other.name));
        Ok((owned, launcher_missing))
    }

    /// Hashes every one of `owned` again, a block at a time, through a handle opened as std opens
    /// one, which lets others delete the file beside it: the cancel looked at before each block and
    /// the progress told after it. A file whose bytes are not the ones the installation records is
    /// [`UpdateError::OwnedFileChanged`]. On Windows a receipt's record of the disc's icon must
    /// name the icon at its fixed place and the installed release's executable first.
    fn rehash(&self, owned: &[Owned], tally: &mut Tally) -> Result<(), UpdateError> {
        #[cfg(windows)]
        self.check_icon_record()?;
        let mut block = vec![0; BLOCK_LEN];
        for file in owned {
            let told = tally.part(&self.job);
            let io = |source| UpdateError::Io {
                path: file.path.clone(),
                source,
            };
            let mut reader = match File::open(&file.path) {
                Ok(reader) => reader,
                Err(error) if is_absent(&error) => return Err(changed(file.name.as_str())),
                Err(source) => return Err(io(source)),
            };
            let mut hasher = Sha256::new();
            let mut done = 0;
            loop {
                self.job.cancel.check()?;
                let read = fill_from(&mut reader, &mut block).map_err(io)?;
                hasher.update(&block[..read]);
                done += read as u64;
                if read > 0 {
                    told.advance(done, file.len, Some(&file.name));
                }
                if read < block.len() {
                    break;
                }
            }
            if Some(Sha256Digest(hasher.finalize().into())) != file.expected {
                return Err(changed(file.name.as_str()));
            }
            tally.done = tally.done.saturating_add(done);
        }
        Ok(())
    }

    /// Refuses a receipt whose record of the disc's icon names another icon than the one at its
    /// fixed place, or another executable than the installed release's.
    #[cfg(windows)]
    fn check_icon_record(&self) -> Result<(), UpdateError> {
        let Some(icon) = &self.receipt.disc_icon else {
            return Ok(());
        };
        let release = self.installed.files.get(&self.installed.executable);
        if icon.path != DISC_ICON
            || Sha256Digest::from_hex(&icon.release_executable_sha256).as_ref() != release
        {
            return Err(UpdateError::Receipt(
                crate::receipt::ReceiptError::Disagrees { field: "discIcon" },
            ));
        }
        Ok(())
    }

    /// Refuses the update unless the volume of the launcher's state has room for `staged` bytes
    /// and [`SPACE_MARGIN`].
    fn check_space(&self, staged: u64) -> Result<(), UpdateError> {
        let state = self.folder.state_dir();
        let volume = (self.settings.folders.volume)(&state).map_err(|source| UpdateError::Io {
            path: state,
            source,
        })?;
        let needed = staged.saturating_add(SPACE_MARGIN);
        if volume.available < needed {
            return Err(UpdateError::Space {
                needed,
                available: volume.available,
            });
        }
        Ok(())
    }

    /// Waits for the installed game to close, holding the job while it runs.
    pub(super) fn game_closed(&self) -> Result<(), UpdateError> {
        let executable = self.folder.root().join(&self.installed.executable);
        game_closed_holding(
            &[&executable],
            self.settings.game_closed,
            &self.job,
            self.settings.hold_poll,
        )
        .map_err(|held| match held {
            HeldGame::Game(error) => UpdateError::Game(error),
            HeldGame::Cancelled => UpdateError::Cancelled,
        })
    }

    /// Stages each of `names` from the runtime in `payload`, in the archive's order, each checked
    /// against the release record as it is written, then synced and closed, before the next is
    /// made; on Unix the game's executable is made a program and every other file readable by all.
    /// Then passes the step, when it staged anything.
    fn stage_files(
        &self,
        staging: &Staging,
        payload: &mut Payload<SectionView<'_, File>>,
        names: &[RelPath],
        tally: &mut Tally,
    ) -> Result<(), UpdateError> {
        if names.is_empty() {
            return Ok(());
        }
        let wanted: BTreeSet<&RelPath> = names.iter().collect();
        let entries = payload.entries().to_vec();
        for entry in entries.iter().filter(|entry| wanted.contains(&entry.name)) {
            self.job.cancel.check()?;
            let mut file = staging.create_file(&entry.name).map_err(files_error)?;
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
                told.advance(0, 0, Some(&entry.name));
            }
            let path = entry.name.path_under(staging.path());
            #[cfg(unix)]
            set_mode(
                &file,
                if entry.name.as_str() == self.installed.executable {
                    PROGRAM_MODE
                } else {
                    FILE_MODE
                },
            )
            .map_err(|source| UpdateError::Io {
                path: path.clone(),
                source,
            })?;
            close_synced(file, &path)?;
            tally.done = tally.done.saturating_add(copied);
        }
        self.pass(FaultPoint::UpdateStage, staging.path())
    }

    /// Sets the disc's icon, read from its fixed place in the installation no further than a TPL
    /// may hold, into the staged executable `executable`, and gives what the receipt records of it.
    /// An icon that is not there is an icon of no bytes, which no TPL decoder takes.
    #[cfg(windows)]
    fn personalise(
        &self,
        staging: &Staging,
        executable: &str,
    ) -> Result<Personalised, UpdateError> {
        let icon = DISC_ICON
            .split('/')
            .fold(self.folder.root().to_path_buf(), |path, part| {
                path.join(part)
            });
        let tpl = match read_bounded(&icon, MAX_TPL_LEN) {
            Ok(tpl) => tpl,
            Err(FilesError::Io { source, .. })
                if is_absent(&source)
                    || fs::symlink_metadata(&icon).is_ok_and(|entry| !entry.is_file()) =>
            {
                Vec::new()
            }
            Err(error) => return Err(files_error(error)),
        };
        let staged = staging.path().join(executable);
        let personalised = (self.settings.personalise)(&staged, &tpl).map_err(UpdateError::Icon)?;
        self.pass(FaultPoint::UpdateStage, &staged)?;
        Ok(personalised)
    }

    /// Copies the whole launcher file, through the handle its sections were found on, as the new
    /// launcher copy, a program on Unix, synced; and gives the copy's digest.
    fn copy_launcher(
        &self,
        staging: &Staging,
        sections: &mut Sections<File>,
        tally: &mut Tally,
    ) -> Result<Sha256Digest, UpdateError> {
        let name = RelPath::parse(installed_launcher_name(self.platform), &PAYLOAD_NAMES)
            .expect("the launcher copy's name keeps the payload's rules");
        let path = name.path_under(staging.path());
        let mut file = staging.create_file(&name).map_err(files_error)?;
        let told = tally.part(&self.job);
        let sha256 = sections
            .copy_file(
                &mut file,
                &Job {
                    cancel: self.job.cancel,
                    progress: &told,
                },
            )
            .map_err(sections_error)?;
        #[cfg(unix)]
        set_mode(&file, PROGRAM_MODE).map_err(|source| UpdateError::Io {
            path: path.clone(),
            source,
        })?;
        close_synced(file, &path)?;
        tally.done = tally.done.saturating_add(sections.file_len());
        self.pass(FaultPoint::UpdateStage, &path)?;
        Ok(sha256)
    }

    /// The receipt the update would write, with every digest it records yet to be known held by
    /// one of the same length: what its size is judged by before anything is staged.
    fn planned_receipt(&self, judged: &Judged, moves_executable: bool) -> Receipt {
        let unknown = Sha256Digest([0; 32]);
        let receipt = self.new_receipt(judged, unknown);
        #[cfg(windows)]
        let receipt = if moves_executable {
            let mut receipt = receipt;
            let personalised = Personalised {
                executable_sha256: unknown,
                icon_sha256: unknown,
            };
            record_icon(&mut receipt, &judged.record, &personalised);
            receipt
        } else {
            receipt
        };
        #[cfg(not(windows))]
        let _ = moves_executable;
        receipt
    }

    /// Stages what follows the runtime's files: on Windows, when the executable is staged to be
    /// moved, the disc's icon set into it; then the launcher copy; and gives the receipt the
    /// update writes.
    fn finish_staging(
        &self,
        staging: &Staging,
        sections: &mut Sections<File>,
        judged: &Judged,
        moves_executable: bool,
        tally: &mut Tally,
    ) -> Result<Receipt, UpdateError> {
        #[cfg(windows)]
        let icon = if moves_executable {
            Some(self.personalise(staging, &judged.record.executable)?)
        } else {
            None
        };
        // Only Windows shows a program by an icon it carries.
        #[cfg(not(windows))]
        let _ = moves_executable;
        let launcher_sha256 = self.copy_launcher(staging, sections, tally)?;
        let receipt = self.new_receipt(judged, launcher_sha256);
        #[cfg(windows)]
        let receipt = match icon {
            Some(icon) => {
                let mut receipt = receipt;
                record_icon(&mut receipt, &judged.record, &icon);
                receipt
            }
            None => receipt,
        };
        self.check_receipt_len(&receipt)?;
        Ok(receipt)
    }

    /// The receipt the update writes: the installed one with the new release's version, commit
    /// and record digest and the new launcher copy's digest, every key it holds beyond its own
    /// kept. A digest it records of a file the update replaces or removes goes, since that file
    /// holds the release's bytes or is gone; on Windows a staged executable's is recorded again
    /// once the disc's icon is set into it.
    fn new_receipt(&self, judged: &Judged, launcher_sha256: Sha256Digest) -> Receipt {
        let record = &judged.record;
        let mut receipt = self.receipt.clone();
        receipt.version = record.version.clone();
        receipt.commit = record.commit.clone();
        receipt.build_info_sha256 = judged.record_sha256.hex();
        receipt.launcher.sha256 = launcher_sha256.hex();
        for name in judged.changes.backup() {
            receipt.installed_digests.remove(name.as_str());
        }
        receipt
    }

    /// Refuses `receipt` as [`FilesError::TooLarge`] when its JSON, as it is written, would take
    /// more than [`MAX_RECEIPT_LEN`] bytes, which no launcher would read again: before anything is
    /// moved, so that its write is never refused once the installed files are aside.
    fn check_receipt_len(&self, receipt: &Receipt) -> Result<(), UpdateError> {
        let path = self.folder.state_dir().join(RECEIPT_FILE);
        let json = serde_json::to_vec_pretty(receipt).map_err(|source| {
            UpdateError::Files(FilesError::Json {
                path: path.clone(),
                source,
            })
        })?;
        // As the receipt is written: its JSON, then a newline.
        let len = u64::try_from(json.len())
            .unwrap_or(u64::MAX)
            .saturating_add(1);
        if len > MAX_RECEIPT_LEN {
            return Err(UpdateError::Files(FilesError::TooLarge {
                path,
                limit: MAX_RECEIPT_LEN,
            }));
        }
        Ok(())
    }

    /// Waits for the game to close again, then, when the executable is staged, runs it in the
    /// staging beside the libraries it loads, with `--version`, once it is seen to be a regular
    /// file reached through no link. A staged game that does not answer is run again as installed:
    /// when that one answers, the staged release is refused as
    /// [`UpdateError::ProbeRefused`]; when it does not either, the update goes on with a notice. A
    /// cancel stops the game and the update.
    fn check_game(
        &self,
        staging: &Staging,
        executable_staged: bool,
        notices: &mut Vec<UpdateNotice>,
    ) -> Result<Probed, UpdateError> {
        self.begin(CHECK_GAME)?;
        self.game_closed()?;
        if !executable_staged {
            return Ok(Probed::NotStaged);
        }
        let executable = &self.installed.executable;
        let staged = staging.path().join(executable);
        no_links(&staged).map_err(files_error)?;
        match fs::symlink_metadata(&staged) {
            Ok(metadata) if metadata.is_file() => {}
            Ok(_) => {
                return Err(UpdateError::Io {
                    path: staged,
                    source: io::Error::new(io::ErrorKind::InvalidInput, "not a regular file"),
                });
            }
            Err(source) => {
                return Err(UpdateError::Io {
                    path: staged,
                    source,
                });
            }
        }
        let Some(failed) = self.probe(&staged)? else {
            return Ok(Probed::Staged);
        };
        let installed = self.folder.root().join(executable);
        match self.probe(&installed)? {
            Some(_) => {
                notices.push(failed.notice);
                Ok(Probed::Staged)
            }
            None => Err(UpdateError::ProbeRefused {
                detail: failed.detail.into(),
            }),
        }
    }

    /// Runs the game at `executable` with `--version`: `None` when it answered, or how it failed.
    fn probe(&self, executable: &Path) -> Result<Option<Failed>, UpdateError> {
        match (self.settings.probe)(executable, self.job.cancel) {
            Ok(probe) if probe.succeeded() => Ok(None),
            Ok(probe) => Ok(Some(Failed {
                detail: probe.status.to_string(),
                notice: UpdateNotice::ProbeFailed {
                    status: probe.status.code(),
                    first_line: probe.first_line,
                    stderr_tail: probe.stderr_tail,
                },
            })),
            Err(GameStartError::Cancelled) => Err(UpdateError::Cancelled),
            Err(error) => Ok(Some(Failed {
                detail: error.to_string(),
                notice: UpdateNotice::ProbeFailed {
                    status: None,
                    first_line: None,
                    stderr_tail: String::new(),
                },
            })),
        }
    }
}

/// How a probed game failed to answer.
struct Failed {
    /// How it failed, as technical detail.
    detail: String,
    /// The notice an update that goes on gives of it.
    notice: UpdateNotice,
}

/// Records in `receipt` the icon set into the executable of the new release `record`, as
/// `personalised` gives it: the executable's new digest, and where the icon came from with the
/// executable's digest as the release lists it; the keys the receipt's earlier record of the icon
/// held beyond its own kept.
#[cfg(windows)]
fn record_icon(receipt: &mut Receipt, record: &BuildInfo, personalised: &Personalised) {
    receipt.installed_digests.insert(
        record.executable.clone(),
        personalised.executable_sha256.hex(),
    );
    let other = receipt
        .disc_icon
        .take()
        .map_or_else(Map::new, |previous| previous.other);
    receipt.disc_icon = Some(DiscIcon {
        path: DISC_ICON.to_owned(),
        sha256: personalised.icon_sha256.hex(),
        release_executable_sha256: record.files[&record.executable].hex(),
        other,
    });
}

/// The refusal of an installed file `name` that is not what the installation records.
fn changed(name: &str) -> UpdateError {
    UpdateError::OwnedFileChanged { name: name.into() }
}

/// How many bytes the file at `path`, the installation's `name`, holds, looked at without
/// following a link; `None` when nothing is there. Anything there but a regular file is
/// [`UpdateError::OwnedFileChanged`].
fn file_len(path: &Path, name: &str) -> Result<Option<u64>, UpdateError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.is_file() => Ok(Some(metadata.len())),
        Ok(_) => Err(changed(name)),
        Err(error) if is_absent(&error) => Ok(None),
        Err(source) => Err(UpdateError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// Writes the staged file `file`, at `path`, through to its disk, then closes it, before the next
/// is made.
fn close_synced(file: File, path: &Path) -> Result<(), UpdateError> {
    file.sync_all().map_err(|source| UpdateError::Io {
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
