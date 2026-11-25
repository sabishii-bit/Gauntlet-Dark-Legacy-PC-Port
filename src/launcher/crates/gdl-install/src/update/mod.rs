//! Updating an installation to a newer release: the game's runtime and the launcher copy at the
//! installation's top together, from a release the feed found or from the launcher file that runs.
//! An update is planned first, without the lock and without changing anything: what it would take
//! the new release from, which runtime names it would move, and the room it would need. It then
//! runs under the installation's lock: it checks the installation, stages what changes beside it,
//! moves the installed files aside and the new ones into place under one journal, and writes the
//! receipt, the point from which the new release stands.

mod journal;
mod names;
mod settings;
mod source;
mod stage;
mod swap;

use std::collections::BTreeSet;
use std::fs::File;
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{
    Cancelled, FilesError, Job, MAX_JSON_LEN, PENDING_SUFFIX, PhaseId, RelPath, is_absent,
    read_json_bounded,
};
use gdl_release::{Platform, Release, Version};
use serde_json::{Map, Value};

pub use journal::{
    DOWNLOAD_PREFIX, RETIRED_LAUNCHER_PREFIX, UPDATE_BACKUP, UPDATE_JOURNAL, UPDATE_STAGING_PREFIX,
    UpdatePhase,
};
pub use names::FileChanges;
pub use source::UpdateSource;

use self::journal::UpdateJournal;
use self::names::judge_names;
pub(crate) use self::settings::UpdateSettings;
use crate::awake::keep_awake;
use crate::compat::{Access, JOURNAL_NAMES, JOURNAL_SCHEMA, access, stamp_of};
use crate::folder::{FolderError, InstallFolder, JOURNAL_DIR, LockError, names_in};
use crate::hold::{HeldLock, lock_holding};
use crate::icon::IconError;
use crate::inspect::FolderState;
use crate::install::{Changes, SPACE_MARGIN};
use crate::payload::{
    BuildInfo, LIB_FOLDER, LIBRARY_SUFFIX, PayloadError, ends_with_ignoring_case,
};
#[cfg(windows)]
use crate::platform;
use crate::probe::GameUseError;
use crate::receipt::{Receipt, ReceiptError};
use crate::sections::{Sections, SectionsError};

/// The phase in which an update takes the installation's lock and reads the installation through
/// its receipt.
pub const CHECK_INSTALLED: PhaseId = PhaseId("checkInstalled");

/// The phase in which an update reads its source's sections against their table and judges the
/// names the new release moves.
pub const VERIFY_DOWNLOAD: PhaseId = PhaseId("verifyDownload");

/// The phase in which an update hashes the installed files again and stages what changes.
pub const STAGE: PhaseId = PhaseId("stage");

/// The phase in which an update moves the installed files aside and the new ones into place, and
/// writes the receipt. It is never cancelled.
pub const SWAP: PhaseId = PhaseId("swap");

/// The phase in which an update that stands removes what it staged and moved aside. It is never
/// cancelled.
pub const CLEANUP: PhaseId = PhaseId("cleanup");

/// What an update would do, read without a lock and without changing anything.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UpdatePlan {
    /// The installation's folder, absolute and folded by name.
    pub root: PathBuf,
    /// The installed release's version.
    pub from: Version,
    /// The version the source would bring. One that is not newer is planned too, since another
    /// launcher may have updated the folder after the source was chosen.
    pub to: Version,
    /// Where the new release would come from.
    pub source: SourceSummary,
    /// Which runtime names the update would move; `None` for a release, whose runtime is inside
    /// the launcher file it downloads.
    pub changes: Option<FileChanges>,
    /// The room the update would need on the volume of the launcher's state: the files it stages,
    /// the libraries and executable it stages beside them for the game's probe, the launcher copy
    /// and [`SPACE_MARGIN`]; `None` for a release, which is measured once downloaded.
    pub needed_bytes: Option<u64>,
    /// Whether a job of this launcher's was left unfinished in the folder, which the update would
    /// finish or take back first.
    pub recovery_pending: bool,
    /// Whether an earlier update's cleanup is still to be finished.
    pub cleanup_pending: bool,
}

/// Where an update would take the new release from, as a plan tells it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SourceSummary {
    /// The launcher file that runs, which carries the release and is copied whole as the new
    /// launcher copy.
    Bundled {
        /// How many bytes the launcher file holds.
        file_len: u64,
    },
    /// A release of the feed's, whose launcher file is downloaded.
    Release {
        /// The release's tag.
        tag: String,
        /// How many bytes its launcher file holds.
        download_bytes: u64,
    },
}

/// Where an update is planned to take the new release from.
pub enum PlanSource<'a> {
    /// The launcher file that runs, through the handle it opened as it started.
    Bundled(&'a mut Sections<File>),
    /// A release the feed found.
    Release(&'a Release),
}

/// Plans the update of the installation at `root` by the launcher of version `running` from
/// `source`, reading the folder without its lock and changing nothing.
///
/// The installation is read through its receipt as [`InstallFolder::installed`] reads it, and a
/// refusal is [`UpdateError::Receipt`]. A receipt or a journal that a later launcher stamped is
/// [`UpdateError::NewerLauncher`], and a launcher older than the installed release
/// [`UpdateError::OlderLauncher`]: neither may change the installation. A job left unfinished, as
/// [`InstallFolder::inspect`] finds one, is planned as pending.
///
/// From the launcher file that runs, its runtime is read no further than its release record and
/// held to the one this launcher installs, of its platform and version, and its names are judged
/// against the installed release's as an update moves them; on Windows an installation whose
/// receipt records no disc icon has its executable staged whatever its bytes, so that the update
/// sets the icon. The room it needs counts what the update stages for the game's probe beside
/// what it moves. From a release, nothing of the runtime is known before its download.
pub fn plan_update(
    root: &Path,
    running: &Version,
    source: PlanSource<'_>,
) -> Result<UpdatePlan, UpdateError> {
    let folder = InstallFolder::at(root).map_err(folder_error)?;
    let installed = folder.installed(running).map_err(UpdateError::Receipt)?;
    let (Access::Writable, Some(receipt)) = (installed.access, installed.receipt.as_ref()) else {
        return Err(UpdateError::NewerLauncher);
    };
    let from = installed.build_info.version();
    if *running < from {
        return Err(UpdateError::OlderLauncher {
            running: running.to_string().into(),
            installed: from.to_string().into(),
        });
    }
    let recovery_pending = match InstallFolder::inspect(folder.root(), running).state {
        FolderState::Pending => true,
        FolderState::NewerLauncher => return Err(UpdateError::NewerLauncher),
        _ => false,
    };
    let (to, source, changes, needed_bytes) = match source {
        PlanSource::Bundled(launcher) => {
            let file_len = launcher.file_len();
            let payload = launcher.runtime().map_err(UpdateError::Runtime)?;
            payload
                .for_launcher(&running.to_string())
                .map_err(UpdateError::Runtime)?;
            let changes = judge_names(
                folder.root(),
                &installed.build_info,
                payload.build_info(),
                stages_executable(&installed.build_info, receipt),
            )?;
            let copies = probe_copies(&changes, payload.build_info());
            let staged: BTreeSet<&RelPath> = changes
                .replaced
                .iter()
                .chain(&changes.added)
                .chain(&copies)
                .collect();
            let staged_bytes: u64 = payload
                .entries()
                .iter()
                .filter(|entry| staged.contains(&entry.name))
                .map(|entry| entry.size)
                .sum();
            let needed = staged_bytes
                .saturating_add(file_len)
                .saturating_add(SPACE_MARGIN);
            (
                payload.build_info().version(),
                SourceSummary::Bundled { file_len },
                Some(changes),
                Some(needed),
            )
        }
        PlanSource::Release(release) => (
            release.version.clone(),
            SourceSummary::Release {
                tag: release.tag.clone(),
                download_bytes: release.launcher.size,
            },
            None,
            None,
        ),
    };
    Ok(UpdatePlan {
        root: folder.root().to_path_buf(),
        from,
        to,
        source,
        changes,
        needed_bytes,
        recovery_pending,
        cleanup_pending: false,
    })
}

/// Whether an update of the installation whose release record is `installed` and whose receipt
/// is `receipt` stages the executable whatever its bytes: on Windows, when the receipt records no
/// disc icon, so that the update sets the icon.
fn stages_executable(installed: &BuildInfo, receipt: &Receipt) -> bool {
    installed.platform == Platform::WindowsX64 && receipt.disc_icon.is_none()
}

/// The files an update stages beside those it moves, so that the game's probe runs the tree the
/// swap will make, and which no move takes: when the executable is staged to be moved, or a
/// library the game loads changes, comes or goes, every library the new record `new` lists that
/// is not staged to be moved, and the executable when it is not either. The libraries are those
/// under `lib/` for Linux and those at the top ending in `.dll` for Windows. In byte order.
pub(crate) fn probe_copies(changes: &FileChanges, new: &BuildInfo) -> Vec<RelPath> {
    let library = |name: &RelPath| match new.platform {
        Platform::LinuxX64 => name.depth() > 1 && name.first() == LIB_FOLDER,
        Platform::WindowsX64 => {
            name.depth() == 1 && ends_with_ignoring_case(name.as_str(), LIBRARY_SUFFIX)
        }
    };
    let publish = changes.publish();
    let moves_executable = publish.iter().any(|name| name.as_str() == new.executable);
    let library_changes = publish.iter().chain(&changes.removed).any(library);
    if !(moves_executable || library_changes) {
        return Vec::new();
    }
    let listed = new
        .files
        .keys()
        .filter_map(|text| RelPath::parse(text, &gdl_files::PAYLOAD_NAMES).ok());
    let mut copies: Vec<RelPath> = listed
        .filter(|name| library(name) || name.as_str() == new.executable)
        .filter(|name| !publish.contains(name))
        .collect();
    copies.sort();
    copies
}

/// An update to run: the installation's folder, the launcher that updates it and the release it
/// brings, from the source given.
pub struct UpdateRequest<'a> {
    /// The installation's folder.
    pub root: &'a Path,
    /// The version of the launcher that runs.
    pub running: &'a Version,
    /// Where the new release comes from.
    pub source: UpdateSource<'a>,
}

/// What an update did.
#[derive(Debug)]
pub enum UpdateOutcome {
    /// The new release stands.
    Updated(Box<Updated>),
    /// The installation is of the source's release already, or of a newer one at the same
    /// precedence, as another launcher may have left it: nothing was changed.
    AlreadyCurrent {
        /// The installed release's version.
        version: Version,
    },
}

/// An update that stands: its folder, the receipt it wrote, what it moved, what became of the
/// retired launcher copy, whether the game was probed, and what the player should be told of it.
#[derive(Debug)]
pub struct Updated {
    /// The installation's folder.
    pub folder: InstallFolder,
    /// The version installed before.
    pub from: Version,
    /// The version installed now.
    pub to: Version,
    /// The receipt the update wrote.
    pub receipt: Receipt,
    /// The runtime names the update moved.
    pub changes: FileChanges,
    /// What became of the launcher copy that was installed before.
    pub retired: RetiredCopy,
    /// Whether the update ran the game's probe.
    pub probed: Probed,
    /// What went wrong without undoing the update, in the order it happened.
    pub notices: Vec<UpdateNotice>,
}

/// What became of the launcher copy that was installed before an update.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RetiredCopy {
    /// None was there to move aside.
    NotMoved,
    /// It was moved aside and removed.
    Removed,
    /// It was moved aside under `name`, in the state folder, and could not be removed yet, as a
    /// launcher still running from it holds it on Windows: the launcher that takes over, or the
    /// next start, removes it.
    Left {
        /// Its name in the state folder.
        name: String,
    },
}

/// What went wrong in an update that stands, as technical detail for logs. What a player reads is
/// chosen from it elsewhere.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UpdateNotice {
    /// The staged game did not answer when it was asked for its version, and neither did the
    /// installed one, so the update went on: it ended with another status than 0, did not start
    /// or did not end in time.
    ProbeFailed {
        /// The staged game's exit code, when it started and ended with one.
        status: Option<i32>,
        /// The first line it wrote to its standard output.
        first_line: Option<String>,
        /// The end of what it wrote to its standard error.
        stderr_tail: String,
    },
    /// Cleaning up after the receipt was written stopped part-way: the journal it leaves has the
    /// next start finish it.
    CleanupLeft {
        /// What stopped it.
        detail: Box<str>,
    },
}

/// Whether an update ran the game's probe.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Probed {
    /// The staged executable was probed, beside the libraries it loads: the executable or a
    /// library changed.
    Staged,
    /// Nothing the game loads changed, so the game was not probed.
    NotStaged,
}

/// Why an update failed, and what it left.
#[derive(Debug, thiserror::Error)]
#[error("{error}")]
pub struct UpdateFailure {
    /// Why it failed.
    pub error: UpdateError,
    /// What it left in the folder.
    pub changes: Changes,
}

impl UpdateFailure {
    /// The failure's stable machine name, its error's.
    pub fn rule(&self) -> &'static str {
        self.error.rule()
    }
}

/// Updates the installation that `request` names, from its source, for the launcher that runs,
/// its progress and cancel taken from `job`. The system is kept out of idle sleep while it runs,
/// and on Windows set to fail a call that meets a critical error rather than show a dialog.
///
/// In phases, each told before it begins:
///
/// 1. [`CHECK_INSTALLED`]: the installation's lock taken, held for while another job has it; an
///    update refused while a job's journal stands in the installation, a committed update's as
///    [`UpdateError::CleanupPending`]; then the installation read through its receipt, a later
///    launcher's being [`UpdateError::NewerLauncher`] and a launcher older than the installed
///    release [`UpdateError::OlderLauncher`]. A source of the installed release, by precedence, is
///    [`UpdateOutcome::AlreadyCurrent`], and nothing changes.
/// 2. [`VERIFY_DOWNLOAD`]: the launcher file's sections read against their table, its runtime held
///    to the one this launcher installs, and the names the update moves judged.
/// 3. [`STAGE`]: every installed file and the launcher copy hashed again against what the receipt
///    and the installed release record give, a file that changed refusing the update; the room
///    the staging needs measured; the game found closed, held for while it runs; then, in a
///    staging folder of the state folder's, each new or changed file, the files the game's probe
///    runs beside them, on Windows the disc's icon set into a staged executable, the launcher
///    copy and the journal.
/// 4. [`CHECK_GAME`](crate::CHECK_GAME): the game found closed again, held for; a staged
///    executable probed in the staging, and the installed one when it fails, an update whose
///    staged game fails where the installed one answers being refused.
/// 5. [`SWAP`], never cancelled once told: the links checked again, the journal moved to
///    swapping, the installed files moved aside, the new ones into place, the launcher copy
///    retired and the new one put in place, then the receipt written: the commit point.
/// 6. [`CLEANUP`]: the staging and what was moved aside removed, the retired launcher copy tried
///    once, then the journal.
///
/// The cancel is looked at as each phase but the last two begins, and before every block and
/// every file within one, for the last time just before the swap. A failure or a cancel before the
/// receipt is written takes back what the update made, [`Changes::RolledBack`], and the
/// installation is as it was: before the swap the staging goes; from the swap on the journal moves
/// to rolling back and every move is undone, the new launcher copy, once in place, going aside
/// under a retired name of its own rather than into the staging, since on Windows a launcher that
/// runs from it would keep the staging from going. A rollback that stops leaves what it has not
/// taken back, with the journal, for the next start, [`Changes::Kept`]. Past the receipt nothing
/// is taken back: what fails in cleaning up is a notice, [`UpdateNotice::CleanupLeft`], with the
/// journal kept for the next start to finish.
pub fn update(request: UpdateRequest<'_>, job: &Job<'_>) -> Result<UpdateOutcome, UpdateFailure> {
    update_with(request, job, &UpdateSettings::shipped())
}

/// Updates the installation as [`update`] does, asking the world beyond the folder through
/// `settings`.
pub(crate) fn update_with(
    request: UpdateRequest<'_>,
    job: &Job<'_>,
    settings: &UpdateSettings<'_>,
) -> Result<UpdateOutcome, UpdateFailure> {
    #[cfg(windows)]
    platform::fail_critical_errors();
    let _awake = keep_awake();
    let UpdateRequest {
        root,
        running,
        mut source,
    } = request;
    job.progress.phase(CHECK_INSTALLED);
    let folder = InstallFolder::at(root)
        .map_err(folder_error)
        .map_err(unchanged)?;
    let lock = lock_holding(&folder, job, settings.hold_poll).map_err(|held| {
        unchanged(match held {
            HeldLock::Lock(error) => UpdateError::Lock(error),
            HeldLock::Cancelled => UpdateError::Cancelled,
        })
    })?;
    check_no_job_left(&folder, running).map_err(unchanged)?;
    let installed = folder
        .installed(running)
        .map_err(|error| unchanged(UpdateError::Receipt(error)))?;
    let (Access::Writable, Some(receipt)) = (installed.access, installed.receipt) else {
        return Err(unchanged(UpdateError::NewerLauncher));
    };
    let from = installed.build_info.version();
    if *running < from {
        return Err(unchanged(UpdateError::OlderLauncher {
            running: running.to_string().into(),
            installed: from.to_string().into(),
        }));
    }
    // The launcher file that runs carries the runtime of its own version alone.
    if *running == from {
        drop(lock);
        return Ok(UpdateOutcome::AlreadyCurrent { version: from });
    }
    let update = Update {
        platform: installed.build_info.platform,
        folder,
        job: *job,
        settings,
        running,
        installed: installed.build_info,
        receipt,
    };
    let staged = update.stage(source.launcher_file())?;
    update.swap(staged, lock)
}

/// An update as it runs, once its lock is held and the installation read through its receipt.
struct Update<'a> {
    /// The installation's folder.
    folder: InstallFolder,
    /// How the job is asked to stop and where its progress goes.
    job: Job<'a>,
    /// What the update asks of the world beyond its folder.
    settings: &'a UpdateSettings<'a>,
    /// The version of the launcher that runs.
    running: &'a Version,
    /// The installed release's record.
    installed: BuildInfo,
    /// The installation's receipt.
    receipt: Receipt,
    /// The platform the installation is for, which is this program's.
    platform: Platform,
}

/// Refuses an update while a job's journal stands in the installation `folder`, for the launcher
/// of version `running`: this launcher does not finish or take back a job before an update, and
/// an update beside one would lose what the journal needs. A journal a later launcher stamped, or
/// of a kind this launcher does not know, is [`UpdateError::NewerLauncher`]; a committed update's,
/// whose cleanup is all that is left, [`UpdateError::CleanupPending`]; any other
/// [`UpdateError::UnfinishedJob`]. A link at a journal is refused as one, and the pending files
/// that interrupted writes leave are no journals.
fn check_no_job_left(folder: &InstallFolder, running: &Version) -> Result<(), UpdateError> {
    let journals = folder.state_dir().join(JOURNAL_DIR);
    let mut names = match names_in(&journals) {
        Ok(names) => names,
        Err(error) if is_absent(&error) => return Ok(()),
        Err(source) => {
            return Err(UpdateError::Io {
                path: journals,
                source,
            });
        }
    };
    names.retain(|name| !name.as_encoded_bytes().ends_with(PENDING_SUFFIX.as_bytes()));
    names.sort();
    let mut unfinished = None;
    let mut cleanup = false;
    for name in &names {
        let Some(text) = name.to_str().filter(|text| JOURNAL_NAMES.contains(text)) else {
            return Err(UpdateError::NewerLauncher);
        };
        let path = journals.join(text);
        let object = match read_json_bounded::<Map<String, Value>>(&path, MAX_JSON_LEN) {
            Ok(object) => object,
            Err(FilesError::Link { path }) => return Err(UpdateError::Links { path }),
            // Gone since the folder was listed, as a job that ends removes its journal.
            Err(FilesError::Io { source, .. }) if is_absent(&source) => continue,
            Err(_) => {
                unfinished.get_or_insert(text);
                continue;
            }
        };
        match stamp_of(&object) {
            Ok(stamp) if access(&stamp, JOURNAL_SCHEMA, running) == Access::ReadOnly => {
                return Err(UpdateError::NewerLauncher);
            }
            Ok(_)
                if text == UPDATE_JOURNAL
                    && UpdateJournal::from_object(&object)
                        .is_ok_and(|journal| journal.phase == UpdatePhase::Committed) =>
            {
                cleanup = true;
            }
            _ => {
                unfinished.get_or_insert(text);
            }
        }
    }
    match unfinished {
        Some(journal) => Err(UpdateError::UnfinishedJob {
            journal: journal.into(),
        }),
        None if cleanup => Err(UpdateError::CleanupPending),
        None => Ok(()),
    }
}

/// The update's failure for `error`, before it changed anything.
fn unchanged(error: UpdateError) -> UpdateFailure {
    UpdateFailure {
        error,
        changes: Changes::None,
    }
}

/// The update's refusal of a folder that cannot hold an installation: a link on its path is
/// refused as one, and nothing there, or something other than a folder, is the system's answer.
fn folder_error(error: FolderError) -> UpdateError {
    match error {
        FolderError::Links { path } => UpdateError::Links { path },
        FolderError::Absent { path } => UpdateError::Io {
            path,
            source: io::Error::from(io::ErrorKind::NotFound),
        },
        FolderError::NotDirectory { path } => UpdateError::Io {
            path,
            source: io::Error::from(io::ErrorKind::NotADirectory),
        },
        FolderError::Io { path, source } => UpdateError::Io { path, source },
    }
}

/// The update's failure for what a file operation reported: a link refused as one, anything else
/// as the operation's own failure.
fn files_error(error: FilesError) -> UpdateError {
    match error {
        FilesError::Link { path } => UpdateError::Links { path },
        other => UpdateError::Files(other),
    }
}

/// The update's failure for what reading the launcher file's sections reported.
fn sections_error(error: SectionsError) -> UpdateError {
    match error {
        SectionsError::Cancelled => UpdateError::Cancelled,
        other => UpdateError::Source(other),
    }
}

/// The update's failure for what reading the runtime reported.
fn payload_error(error: PayloadError) -> UpdateError {
    match error {
        PayloadError::Cancelled | PayloadError::Sections(SectionsError::Cancelled) => {
            UpdateError::Cancelled
        }
        other => UpdateError::Runtime(other),
    }
}

impl From<Cancelled> for UpdateError {
    fn from(Cancelled: Cancelled) -> UpdateError {
        UpdateError::Cancelled
    }
}

/// Why an update could not be planned or made, as technical detail for logs. What a player reads
/// is chosen from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum UpdateError {
    /// The installation's lock was refused, for another reason than another job's hold, which is
    /// waited out.
    #[error(transparent)]
    Lock(LockError),
    /// The installation cannot be read through its receipt.
    #[error(transparent)]
    Receipt(ReceiptError),
    /// A later launcher's receipt or journal stands in the installation.
    #[error("A later launcher's state stands in this installation; this launcher changes nothing")]
    NewerLauncher,
    /// The launcher file that runs is older than the installed release.
    #[error("This launcher file ({running}) is older than the installation ({installed})")]
    OlderLauncher {
        /// The running launcher's version.
        running: Box<str>,
        /// The installed release's version.
        installed: Box<str>,
    },
    /// The previous update stands, but its cleanup is still to be finished.
    #[error(
        "The previous update is installed. Close and reopen the launcher to release its old \
         runtime files before updating again."
    )]
    CleanupPending,
    /// A job's journal stands in the installation, which the launcher's next start is to finish
    /// or take back before anything else changes the installation.
    #[error("A job left unfinished in this installation is to be recovered first: {journal}")]
    UnfinishedJob {
        /// The journal's name.
        journal: Box<str>,
    },
    /// The launcher file's sections do not hold what their table says, or its copy could not be
    /// made.
    #[error(transparent)]
    Source(SectionsError),
    /// The new runtime is refused, or is not the one this launcher brings.
    #[error(transparent)]
    Runtime(PayloadError),
    /// The installed release record names a file that no runtime may hold.
    #[error("The installed release record names a file no runtime holds: {name}")]
    InstalledNames {
        /// The name.
        name: Box<str>,
    },
    /// A new name equals an installed one in ASCII letter case alone.
    #[error("Update changes a managed path's letter case: {old} -> {new}")]
    CaseChange {
        /// The installed name.
        old: Box<str>,
        /// The new name.
        new: Box<str>,
    },
    /// Something the launcher did not put there is at a name the new release adds, or at a folder
    /// above it.
    #[error("Update would overwrite an unowned file: {name}")]
    Unowned {
        /// The new name.
        name: Box<str>,
    },
    /// `path` is a link: the installation's folder, a folder above it, or a runtime name or a
    /// folder on the way to one.
    #[error("Choose a directory without links or junctions: {}", .path.display())]
    Links {
        /// The link.
        path: PathBuf,
    },
    /// An installed file, or the launcher copy, is not what the installation records.
    #[error(
        "Installed runtime file was modified or removed: {name}. Keep personal settings in \
         config/settings.json."
    )]
    OwnedFileChanged {
        /// The file's name under the installation's top.
        name: Box<str>,
    },
    /// The volume of the launcher's state has too little room for what the update stages.
    #[error("Not enough disk space to stage the update safely")]
    Space {
        /// The bytes the update needs.
        needed: u64,
        /// The bytes the volume has for this process.
        available: u64,
    },
    /// Whether the game runs could not be told.
    #[error(transparent)]
    Game(GameUseError),
    /// The staged game did not answer when it was asked for its version, though the installed one
    /// did.
    #[error("The staged game does not answer --version, though the installed one does: {detail}")]
    ProbeRefused {
        /// How the staged game failed.
        detail: Box<str>,
    },
    /// The disc's icon could not be set into the staged executable.
    #[error(transparent)]
    Icon(IconError),
    /// The update's journal would hold more than its readers read.
    #[error("The update's journal would take more than {limit} bytes")]
    JournalTooLarge {
        /// The most bytes a journal may hold.
        limit: u64,
    },
    /// Something took the place of a name the update puts in place after it was found free.
    #[error("Destination changed during update: {name}")]
    Changed {
        /// The name.
        name: Box<str>,
    },
    /// What an update moved is not where its journal says it can be, so it can be neither
    /// finished nor taken back: a name in neither of its places, or in both where only one may
    /// be, a file left in the backup, or a link on the way of a move back. The journal is kept.
    #[error("The interrupted update cannot be finished or taken back: {detail}")]
    Inconsistent {
        /// What was found.
        detail: Box<str>,
    },
    /// A file operation in the folder failed.
    #[error(transparent)]
    Files(FilesError),
    /// The system refused to look at `path`, or nothing is there to hold an installation.
    #[error("{}: {source}", .path.display())]
    Io {
        /// What was looked at.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The job was asked to stop.
    #[error("Update cancelled; the installation was not changed.")]
    Cancelled,
}

impl UpdateError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the lock's, the
    /// receipt's, the sections', the runtime's, the game's, the icon's or the file operation's
    /// rule, or `newerLauncher`, `olderLauncher`, `cleanupPending`, `unfinishedJob`,
    /// `installedNames`, `caseChange`, `unowned`, `links`, `ownedFileChanged`, `space`,
    /// `probeRefused`, `journalTooLarge`, `changed`, `inconsistent`, `io` or `cancelled`.
    pub fn rule(&self) -> &'static str {
        match self {
            UpdateError::Lock(error) => error.rule(),
            UpdateError::Receipt(error) => error.rule(),
            UpdateError::NewerLauncher => "newerLauncher",
            UpdateError::OlderLauncher { .. } => "olderLauncher",
            UpdateError::CleanupPending => "cleanupPending",
            UpdateError::UnfinishedJob { .. } => "unfinishedJob",
            UpdateError::Source(error) => error.rule(),
            UpdateError::Runtime(error) => error.rule(),
            UpdateError::InstalledNames { .. } => "installedNames",
            UpdateError::CaseChange { .. } => "caseChange",
            UpdateError::Unowned { .. } => "unowned",
            UpdateError::Links { .. } => "links",
            UpdateError::OwnedFileChanged { .. } => "ownedFileChanged",
            UpdateError::Space { .. } => "space",
            UpdateError::Game(error) => game_rule(error),
            UpdateError::ProbeRefused { .. } => "probeRefused",
            UpdateError::Icon(error) => error.rule(),
            UpdateError::JournalTooLarge { .. } => "journalTooLarge",
            UpdateError::Changed { .. } => "changed",
            UpdateError::Inconsistent { .. } => "inconsistent",
            UpdateError::Files(error) => error.rule(),
            UpdateError::Io { .. } => "io",
            UpdateError::Cancelled => "cancelled",
        }
    }
}

/// The stable machine name of why the game counts as in use, or could not be checked.
fn game_rule(error: &GameUseError) -> &'static str {
    match error {
        GameUseError::Running => "running",
        GameUseError::Missing => "missing",
        GameUseError::NotWritable { .. } => "notWritable",
        GameUseError::Io { .. } => "io",
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::collections::{BTreeMap, BTreeSet};
    use std::fs::{self, File};
    use std::io;
    use std::path::{Path, PathBuf};
    use std::process::ExitStatus;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};

    use gdl_files::{
        Cancel, FilesError, Hold, Job, NoProgress, PAYLOAD_NAMES, PhaseId, Progress, RelPath,
        Volume, retry_locked,
    };
    #[cfg(windows)]
    use gdl_fixtures::fake_sign;
    use gdl_fixtures::{
        EntryState, InstallFixture, RawEntry, RuntimeFiles, append_sections, link_folder,
        pretty_json, sha256_hex, snapshot, snapshot_outside, tiny_elf, tiny_pe, tree_entries,
    };
    use gdl_media::Sha256Digest;
    use gdl_release::{Asset, Release, Version, checksums_asset, launcher_asset};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::settings::{ProbeFn, UpdateSettings};
    use super::{
        CLEANUP, FileChanges, PlanSource, Probed, RETIRED_LAUNCHER_PREFIX, RetiredCopy, SWAP,
        SourceSummary, UPDATE_BACKUP, UPDATE_JOURNAL, UpdateError, UpdateFailure, UpdateNotice,
        UpdateOutcome, UpdatePlan, UpdateRequest, UpdateSource, Updated, plan_update, update_with,
    };
    use crate::destination::FolderChecks;
    use crate::faults::{FailAt, FaultPoint, FaultPoints, Faults, NoFaults, OnPass, PanicAt};
    use crate::folder::{
        InstallFolder, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE, STATE_DIR,
    };
    use crate::hold::SCANNER_GRACE;
    use crate::install::{Changes, INSTALL_JOURNAL, SPACE_MARGIN};
    use crate::payload::{BUILD_INFO, PayloadError};
    use crate::probe::{GameStartError, GameUseError, VersionProbe};
    use crate::receipt::{MAX_RECEIPT_LEN, Receipt, ReceiptError, installed_launcher_name};
    use crate::sections::{RUNTIME_SECTION, Sections, SectionsError};
    use crate::test_platform::{other_platform, this_platform};

    /// The version the tests' installations are of.
    const INSTALLED: &str = "1.0.0-beta";

    /// The version of the release the tests update them to.
    const NEXT: &str = "1.0.0-beta.1";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// An installation of the standard runtime of `version` for the platform this test program was
    /// built for, as the launcher leaves one, in a folder of the test's own.
    fn installation_of(version: &str) -> (TempDir, InstallFixture) {
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(this_platform().as_str(), version);
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        (dir, fixture)
    }

    /// A launcher file of this system's kind carrying `runtime`, written into `dir` outside the
    /// installation, signed on Windows as a release is, with its sections found as the launcher
    /// finds its own.
    fn bundled(dir: &Path, runtime: &RuntimeFiles) -> Sections<File> {
        bundled_as(dir, "launcher", runtime).1
    }

    /// A launcher file as [`bundled`] writes one, named `name` in `dir`, and its path.
    fn bundled_as(dir: &Path, name: &str, runtime: &RuntimeFiles) -> (PathBuf, Sections<File>) {
        carrying(dir, name, &runtime.zip())
    }

    /// A launcher file as [`bundled`] writes one, named `name` in `dir`, carrying the runtime zip
    /// `zip`, and its path.
    fn carrying(dir: &Path, name: &str, zip: &[u8]) -> (PathBuf, Sections<File>) {
        let mut file = if cfg!(windows) { tiny_pe() } else { tiny_elf() };
        append_sections(&mut file, &[(RUNTIME_SECTION, zip)], 0);
        #[cfg(windows)]
        fake_sign(&mut file);
        let path = dir.join(name);
        fs::write(&path, file).unwrap();
        let sections = Sections::of_path(&path).unwrap().unwrap();
        (path, sections)
    }

    /// `texts`, each as a name.
    fn names(texts: &[&str]) -> Vec<RelPath> {
        texts
            .iter()
            .map(|text| RelPath::parse(text, &PAYLOAD_NAMES).unwrap())
            .collect()
    }

    /// How many bytes the files `texts` of `runtime` hold, its release record among them.
    fn bytes_of(runtime: &RuntimeFiles, texts: &[&str]) -> u64 {
        let record = runtime.build_info_bytes();
        texts
            .iter()
            .map(|text| match *text {
                BUILD_INFO => record.len(),
                _ => {
                    let (_, data) = runtime.files.iter().find(|(name, _)| name == text).unwrap();
                    data.len()
                }
            })
            .sum::<usize>() as u64
    }

    #[test]
    fn a_plan_names_what_changes() {
        let (dir, fixture) = installation_of(INSTALLED);
        let next = fixture.runtime.next(NEXT);
        let mut launcher = bundled(dir.path(), &next);
        let file_len = launcher.file_len();
        let before = snapshot(dir.path()).unwrap();

        let plan = plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        let publish = [
            "VERSION",
            "build-info.json",
            "data/text/en.json",
            "shaders/next.spv",
        ];
        assert_eq!(
            plan,
            UpdatePlan {
                root: fixture.root.clone(),
                from: version(INSTALLED),
                to: version(NEXT),
                source: SourceSummary::Bundled { file_len },
                changes: Some(FileChanges {
                    replaced: names(&["VERSION", "build-info.json", "data/text/en.json"]),
                    added: names(&["shaders/next.spv"]),
                    removed: names(&["shaders/test.spv"]),
                    unchanged: 3,
                }),
                needed_bytes: Some(bytes_of(&next, &publish) + file_len + SPACE_MARGIN),
                recovery_pending: false,
                cleanup_pending: false,
            }
        );
        // Nothing was made, changed or removed, in the installation or beside it.
        assert_eq!(snapshot(dir.path()).unwrap(), before);
        assert!(!fixture.root.join(STATE_DIR).join(LOCK_FILE).exists());
    }

    #[test]
    fn a_source_no_newer_than_the_installation_is_still_planned() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime);
        let plan = plan_update(
            &fixture.root,
            &version(INSTALLED),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        assert_eq!(plan.from, plan.to);
        let changes = plan.changes.unwrap();
        assert_eq!(changes.replaced, names(&[BUILD_INFO]));
        assert_eq!(changes.unchanged, fixture.runtime.files.len());
    }

    #[test]
    fn a_release_plans_its_download() {
        let (_dir, fixture) = installation_of(INSTALLED);
        let to = version(NEXT);
        let platform = this_platform();
        let asset = |name: String, size: u64| Asset {
            url: format!("https://github.com/example/releases/download/v{to}/{name}"),
            name,
            size,
            sha256: Sha256Digest::of(b"launcher"),
        };
        let release = Release {
            version: to.clone(),
            tag: format!("v{to}"),
            platform,
            launcher: asset(launcher_asset(&to, platform), 7_340_032),
            checksums: asset(checksums_asset(&to, platform), 300),
            signature: None,
            notes: None,
        };
        let plan = plan_update(
            &fixture.root,
            &version(INSTALLED),
            PlanSource::Release(&release),
        )
        .unwrap();
        assert_eq!(
            plan,
            UpdatePlan {
                root: fixture.root.clone(),
                from: version(INSTALLED),
                to,
                source: SourceSummary::Release {
                    tag: release.tag.clone(),
                    download_bytes: 7_340_032,
                },
                changes: None,
                needed_bytes: None,
                recovery_pending: false,
                cleanup_pending: false,
            }
        );
    }

    #[test]
    fn an_older_launcher_plans_nothing() {
        let (dir, fixture) = installation_of(NEXT);
        let mut launcher = bundled(
            dir.path(),
            &RuntimeFiles::standard(this_platform().as_str(), INSTALLED),
        );
        let before = snapshot(dir.path()).unwrap();
        match plan_update(
            &fixture.root,
            &version(INSTALLED),
            PlanSource::Bundled(&mut launcher),
        ) {
            Err(UpdateError::OlderLauncher { running, installed }) => {
                assert_eq!((&*running, &*installed), (INSTALLED, NEXT));
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn a_later_launchers_installation_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let running = version(NEXT);

        // A receipt a later launcher stamped.
        let mut receipt = fixture.receipt();
        receipt["minLauncher"] = json!("9.0.0");
        fixture.write_receipt(&receipt).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let plan = plan_update(&fixture.root, &running, PlanSource::Bundled(&mut launcher));
        assert!(matches!(plan, Err(UpdateError::NewerLauncher)), "{plan:?}");
        assert_eq!(snapshot(dir.path()).unwrap(), before);

        // A journal a later launcher stamped, beside a receipt this one may change.
        fixture.write_receipt(&fixture.receipt()).unwrap();
        let journals = fixture.root.join(STATE_DIR).join(JOURNAL_DIR);
        fs::create_dir(&journals).unwrap();
        let journal = json!({"schema": 2, "minLauncher": INSTALLED});
        fs::write(journals.join(INSTALL_JOURNAL), journal.to_string()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let plan = plan_update(&fixture.root, &running, PlanSource::Bundled(&mut launcher));
        assert!(matches!(plan, Err(UpdateError::NewerLauncher)), "{plan:?}");
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn a_job_left_unfinished_is_planned_as_pending() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let journals = fixture.root.join(STATE_DIR).join(JOURNAL_DIR);
        fs::create_dir(&journals).unwrap();
        let journal = json!({"schema": 1, "minLauncher": INSTALLED});
        fs::write(journals.join(INSTALL_JOURNAL), journal.to_string()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let plan = plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        assert!(plan.recovery_pending);
        assert!(!plan.cleanup_pending);
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn a_runtime_that_is_not_this_launchers_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        match plan_update(
            &fixture.root,
            &version("1.0.0-beta.2"),
            PlanSource::Bundled(&mut launcher),
        ) {
            Err(UpdateError::Runtime(PayloadError::OtherVersion { found, expected })) => {
                assert_eq!((&*found, &*expected), (NEXT, "1.0.0-beta.2"));
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn an_installed_record_that_claims_a_players_file_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let save = fixture.root.join("saves").join("hero.json");
        fs::write(&save, b"user-owned").unwrap();
        // The record and the receipt agree with each other, but the record claims the save.
        let mut claiming = fixture.runtime.build_info();
        claiming["files"]["saves/hero.json"] = json!(sha256_hex(b"user-owned"));
        let record = pretty_json(&claiming);
        fs::write(fixture.root.join(BUILD_INFO), &record).unwrap();
        let mut receipt = fixture.receipt();
        receipt["buildInfoSha256"] = json!(sha256_hex(&record));
        fixture.write_receipt(&receipt).unwrap();
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = snapshot(dir.path()).unwrap();
        match plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        ) {
            Err(UpdateError::Receipt(ReceiptError::Disagrees { field })) => {
                assert_eq!(field, BUILD_INFO);
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(snapshot(dir.path()).unwrap(), before);
        assert_eq!(fs::read(&save).unwrap(), b"user-owned");
    }

    #[test]
    fn a_folder_that_holds_no_installation_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let running = version(NEXT);
        let missing = dir.path().join("missing");
        match plan_update(&missing, &running, PlanSource::Bundled(&mut launcher)) {
            Err(UpdateError::Io { path, source }) => {
                assert_eq!(path, missing);
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("{other:?}"),
        }
        let receipt = fixture.root.join(STATE_DIR).join(RECEIPT_FILE);
        retry_locked(|| fs::remove_file(&receipt)).unwrap();
        match plan_update(&fixture.root, &running, PlanSource::Bundled(&mut launcher)) {
            Err(UpdateError::Receipt(ReceiptError::Unreadable { .. })) => {}
            other => panic!("{other:?}"),
        }
    }

    #[cfg(windows)]
    #[test]
    fn a_windows_receipt_without_its_icon_stages_the_executable() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut receipt = fixture.receipt();
        receipt.as_object_mut().unwrap().remove("discIcon");
        fixture.write_receipt(&receipt).unwrap();
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let plan = plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        let changes = plan.changes.unwrap();
        assert_eq!(
            changes.replaced,
            names(&[
                "VERSION",
                "build-info.json",
                "data/text/en.json",
                "gauntlet.exe"
            ])
        );
        assert_eq!(changes.unchanged, 2);
    }

    #[test]
    fn update_errors_name_their_rules_and_stay_small() {
        let path = PathBuf::from("install");
        let cases = [
            (
                UpdateError::Receipt(ReceiptError::Disagrees { field: "version" }),
                "disagrees",
                "Installation receipts disagree; choose a complete installed game (version)"
                    .to_owned(),
            ),
            (
                UpdateError::NewerLauncher,
                "newerLauncher",
                "A later launcher's state stands in this installation; this launcher changes \
                 nothing"
                    .to_owned(),
            ),
            (
                UpdateError::OlderLauncher {
                    running: INSTALLED.into(),
                    installed: NEXT.into(),
                },
                "olderLauncher",
                format!("This launcher file ({INSTALLED}) is older than the installation ({NEXT})"),
            ),
            (
                UpdateError::Runtime(PayloadError::Sections(SectionsError::NoSections)),
                "noSections",
                SectionsError::NoSections.to_string(),
            ),
            (
                UpdateError::InstalledNames {
                    name: "saves/hero.json".into(),
                },
                "installedNames",
                "The installed release record names a file no runtime holds: saves/hero.json"
                    .to_owned(),
            ),
            (
                UpdateError::CaseChange {
                    old: "data/text/en.json".into(),
                    new: "data/Text/en.json".into(),
                },
                "caseChange",
                "Update changes a managed path's letter case: data/text/en.json -> \
                 data/Text/en.json"
                    .to_owned(),
            ),
            (
                UpdateError::Unowned {
                    name: "shaders/new.spv".into(),
                },
                "unowned",
                "Update would overwrite an unowned file: shaders/new.spv".to_owned(),
            ),
            (
                UpdateError::Links { path: path.clone() },
                "links",
                format!(
                    "Choose a directory without links or junctions: {}",
                    path.display()
                ),
            ),
            (
                UpdateError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                },
                "io",
                format!("{}: refused", path.display()),
            ),
            (
                UpdateError::Lock(LockError::Busy),
                "busy",
                "Another job is using this installation".to_owned(),
            ),
            (
                UpdateError::CleanupPending,
                "cleanupPending",
                "The previous update is installed. Close and reopen the launcher to release its \
                 old runtime files before updating again."
                    .to_owned(),
            ),
            (
                UpdateError::UnfinishedJob {
                    journal: INSTALL_JOURNAL.into(),
                },
                "unfinishedJob",
                "A job left unfinished in this installation is to be recovered first: \
                 install.json"
                    .to_owned(),
            ),
            (
                UpdateError::Source(SectionsError::NoSections),
                "noSections",
                SectionsError::NoSections.to_string(),
            ),
            (
                UpdateError::OwnedFileChanged {
                    name: "gauntlet".into(),
                },
                "ownedFileChanged",
                "Installed runtime file was modified or removed: gauntlet. Keep personal settings \
                 in config/settings.json."
                    .to_owned(),
            ),
            (
                UpdateError::Space {
                    needed: 2,
                    available: 1,
                },
                "space",
                "Not enough disk space to stage the update safely".to_owned(),
            ),
            (
                UpdateError::Game(GameUseError::Running),
                "running",
                "Close the game before changing its installation".to_owned(),
            ),
            (
                UpdateError::Game(GameUseError::Missing),
                "missing",
                "The installed executable is missing".to_owned(),
            ),
            (
                UpdateError::Game(GameUseError::NotWritable { path: path.clone() }),
                "notWritable",
                format!(
                    "The installed executable is not writable: {}",
                    path.display()
                ),
            ),
            (
                UpdateError::Game(GameUseError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                }),
                "io",
                format!("{}: refused", path.display()),
            ),
            (
                UpdateError::ProbeRefused {
                    detail: "exit status: 3".into(),
                },
                "probeRefused",
                "The staged game does not answer --version, though the installed one does: exit \
                 status: 3"
                    .to_owned(),
            ),
            (
                UpdateError::Icon(crate::icon::IconError::Decoded),
                "decoded",
                "Invalid decoded icon".to_owned(),
            ),
            (
                UpdateError::JournalTooLarge { limit: 4 << 20 },
                "journalTooLarge",
                "The update's journal would take more than 4194304 bytes".to_owned(),
            ),
            (
                UpdateError::Changed {
                    name: "shaders/new.spv".into(),
                },
                "changed",
                "Destination changed during update: shaders/new.spv".to_owned(),
            ),
            (
                UpdateError::Inconsistent {
                    detail: "VERSION is neither in place nor in the backup".into(),
                },
                "inconsistent",
                "The interrupted update cannot be finished or taken back: VERSION is neither in \
                 place nor in the backup"
                    .to_owned(),
            ),
            (
                UpdateError::Files(FilesError::TooLarge {
                    path: path.clone(),
                    limit: 1,
                }),
                "tooLarge",
                format!("Metadata is too large: {}", path.display()),
            ),
            (
                UpdateError::Cancelled,
                "cancelled",
                "Update cancelled; the installation was not changed.".to_owned(),
            ),
        ];
        for (error, rule, text) in cases {
            assert_eq!(error.rule(), rule, "{error:?}");
            assert_eq!(error.to_string(), text, "{error:?}");
        }
        assert!(size_of::<UpdateError>() <= MOST_ERROR_BYTES);
        let failure = UpdateFailure {
            error: UpdateError::Cancelled,
            changes: Changes::RolledBack,
        };
        assert_eq!(failure.rule(), "cancelled");
        assert_eq!(failure.to_string(), UpdateError::Cancelled.to_string());
        assert!(size_of::<UpdateFailure>() <= MOST_ERROR_BYTES);
    }

    /// How often the tests' holds look again: far more often than a job's, so that they run fast.
    const POLL: Duration = Duration::from_millis(20);

    /// What each of the player's files holds.
    const PLAYERS_BYTES: &[u8] = b"user-owned";

    /// The player's files a test lays out beside an installation: a save, the settings, a file of
    /// the player's among the runtime's data, and a note at the top.
    const PLAYERS_FILES: [&str; 4] = [
        "saves/hero.json",
        "config/settings.json",
        "data/local.json",
        "notes.txt",
    ];

    /// What the tests' personaliser appends to an executable in place of the disc's icon.
    #[cfg(windows)]
    const ICON_MARK: &[u8] = b" with the disc's icon";

    /// The version of a release after [`NEXT`].
    const LATER: &str = "1.0.0-beta.2";

    /// The library the probe tests' runtimes carry beside the game.
    const LIBRARY: &str = if cfg!(windows) {
        "runtime-stand-in.dll"
    } else {
        "lib/libstdc++.so.6"
    };

    /// `name`, a name with `/` between its parts, under `root`.
    fn under(root: &Path, name: &str) -> PathBuf {
        name.split('/')
            .fold(root.to_path_buf(), |path, part| path.join(part))
    }

    /// An installation of the standard runtime of [`INSTALLED`] with the player's files beside it.
    fn players_installation() -> (TempDir, InstallFixture) {
        let (dir, fixture) = installation_of(INSTALLED);
        add_players_files(&fixture.root);
        (dir, fixture)
    }

    /// Lays the player's files out in the installation at `root`.
    fn add_players_files(root: &Path) {
        for name in PLAYERS_FILES {
            let path = under(root, name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, PLAYERS_BYTES).unwrap();
        }
    }

    /// An installation of the standard runtime of [`INSTALLED`] that carries [`LIBRARY`] beside
    /// the game, with the player's files.
    fn library_installation() -> (TempDir, InstallFixture) {
        let dir = tempfile::tempdir().unwrap();
        let mut runtime = RuntimeFiles::standard(this_platform().as_str(), INSTALLED);
        runtime
            .files
            .push((LIBRARY.to_owned(), b"a library".to_vec()));
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        add_players_files(&fixture.root);
        (dir, fixture)
    }

    /// Gives the file `name` of `runtime` the bytes `data`.
    fn set_file(runtime: &mut RuntimeFiles, name: &str, data: &[u8]) {
        let file = runtime
            .files
            .iter_mut()
            .find(|(file, _)| file == name)
            .unwrap();
        file.1 = data.to_vec();
    }

    /// The game's executable in the tests' runtimes.
    fn executable() -> &'static str {
        this_platform().executable()
    }

    /// The launcher copy at the tests' installations' top.
    fn launcher_copy() -> &'static str {
        installed_launcher_name(this_platform())
    }

    /// A closed-game probe that finds the game closed.
    fn closed(_: &Path) -> Result<(), GameUseError> {
        Ok(())
    }

    /// A shell that is told nothing it keeps.
    fn unheard(_: &Path) {}

    /// The status a program that ended with exit code `code` has.
    #[cfg(unix)]
    fn exit_status(code: u8) -> ExitStatus {
        use std::os::unix::process::ExitStatusExt;
        ExitStatus::from_raw(i32::from(code) << 8)
    }

    /// The status a program that ended with exit code `code` has.
    #[cfg(windows)]
    fn exit_status(code: u8) -> ExitStatus {
        use std::os::windows::process::ExitStatusExt;
        ExitStatus::from_raw(u32::from(code))
    }

    /// What a probed game that ends with exit code `code` answers: with 0 its version, otherwise
    /// a refusal and why.
    fn answered(code: u8) -> Result<VersionProbe, GameStartError> {
        Ok(VersionProbe {
            status: exit_status(code),
            first_line: Some(if code == 0 { NEXT } else { "no" }.to_owned()),
            stderr_tail: if code == 0 {
                String::new()
            } else {
                "the game is broken".to_owned()
            },
        })
    }

    /// A probe that finds the game answering.
    fn answering(_: &Path, _: &Cancel) -> Result<VersionProbe, GameStartError> {
        answered(0)
    }

    /// A personaliser that appends [`ICON_MARK`] to the executable rather than set an icon into
    /// it, as no executable the tests stage is a program the system edits.
    #[cfg(windows)]
    fn marking(
        executable: &Path,
        tpl: &[u8],
    ) -> Result<crate::icon::Personalised, crate::icon::IconError> {
        let mut bytes = fs::read(executable).unwrap();
        bytes.extend_from_slice(ICON_MARK);
        fs::write(executable, &bytes).unwrap();
        Ok(crate::icon::Personalised {
            executable_sha256: Sha256Digest::of(&bytes),
            icon_sha256: Sha256Digest::of(tpl),
        })
    }

    /// The world beyond the folder as the update tests give it: `faults`, the folder's volume as
    /// the system measures it, the probe `probe`, a game that is closed, on Windows a personaliser
    /// that marks the executable rather than edit it, a shell nobody tells, the receipt written as
    /// the launcher writes it, and holds that look again every [`POLL`].
    fn standing_in<'a>(faults: &'a dyn FaultPoints, probe: &'a ProbeFn<'a>) -> UpdateSettings<'a> {
        UpdateSettings {
            faults,
            folders: &FolderChecks::SHIPPED,
            probe,
            game_closed: &closed,
            #[cfg(windows)]
            personalise: &marking,
            notify_shell: &unheard,
            write_receipt: &Receipt::write,
            hold_poll: POLL,
        }
    }

    /// Updates the installation `fixture` from the launcher file `source` by the launcher of
    /// version `running`, with `job`'s cancel and progress and the world `settings` gives.
    fn update_by(
        fixture: &InstallFixture,
        source: &mut Sections<File>,
        running: &str,
        job: &Job<'_>,
        settings: &UpdateSettings<'_>,
    ) -> Result<UpdateOutcome, UpdateFailure> {
        let running = version(running);
        let request = UpdateRequest {
            root: &fixture.root,
            running: &running,
            source: UpdateSource::Bundled(source),
        };
        update_with(request, job, settings)
    }

    /// Updates as [`update_by`] does, with a cancel nobody makes and a progress nobody follows.
    fn update_simply(
        fixture: &InstallFixture,
        source: &mut Sections<File>,
        running: &str,
        settings: &UpdateSettings<'_>,
    ) -> Result<UpdateOutcome, UpdateFailure> {
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        update_by(fixture, source, running, &job, settings)
    }

    /// The update that `result` says stands.
    #[track_caller]
    fn updated(result: Result<UpdateOutcome, UpdateFailure>) -> Updated {
        match result {
            Ok(UpdateOutcome::Updated(updated)) => *updated,
            other => panic!("{other:?}"),
        }
    }

    /// The failure that `result` says the update ended in.
    #[track_caller]
    fn failed(result: Result<UpdateOutcome, UpdateFailure>) -> UpdateFailure {
        match result {
            Err(failure) => failure,
            Ok(outcome) => panic!("{outcome:?}"),
        }
    }

    /// What an update that changes nothing leaves as it was: every entry outside the state folder,
    /// the receipt's bytes, and the names in the state folder but the lock and an empty journal
    /// folder.
    #[derive(Debug, PartialEq, Eq)]
    struct AsItWas {
        outside: BTreeMap<PathBuf, EntryState>,
        receipt: Vec<u8>,
        state: Vec<String>,
    }

    /// What the installation at `root` holds, as [`AsItWas`] keeps it.
    fn as_it_is(root: &Path) -> AsItWas {
        AsItWas {
            outside: snapshot_outside(root, STATE_DIR).unwrap(),
            receipt: fs::read(root.join(STATE_DIR).join(RECEIPT_FILE)).unwrap(),
            state: state_names(root),
        }
    }

    /// What an update taken back after its moves leaves as it was: what [`AsItWas`] keeps, but for
    /// the times entries were changed, since moving a file out of its folder and back changes the
    /// folder's: every path outside the state folder with its kind and a file's bytes, the
    /// receipt's bytes, and the names in the state folder but the lock and an empty journal folder.
    #[derive(Debug, PartialEq, Eq)]
    struct Contents {
        outside: BTreeMap<PathBuf, (fs::FileType, Option<[u8; 32]>)>,
        receipt: Vec<u8>,
        state: Vec<String>,
    }

    /// What the installation at `root` holds, as [`Contents`] keeps it.
    fn contents_of(root: &Path) -> Contents {
        let AsItWas {
            outside,
            receipt,
            state,
        } = as_it_is(root);
        Contents {
            outside: outside
                .into_iter()
                .map(|(path, entry)| (path, (entry.file_type, entry.digest)))
                .collect(),
            receipt,
            state,
        }
    }

    /// The names in the state folder of the installation at `root`, but the lock and an empty
    /// journal folder, sorted.
    fn state_names(root: &Path) -> Vec<String> {
        let state = root.join(STATE_DIR);
        let journals_empty =
            fs::read_dir(state.join(JOURNAL_DIR)).is_ok_and(|mut names| names.next().is_none());
        let mut names: Vec<String> = fs::read_dir(&state)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .filter(|name| name != LOCK_FILE && !(name == JOURNAL_DIR && journals_empty))
            .collect();
        names.sort();
        names
    }

    /// The update's staging folders in the state folder of the installation at `root`.
    fn stagings(root: &Path) -> Vec<String> {
        state_names(root)
            .into_iter()
            .filter(|name| name.starts_with("stage-"))
            .collect()
    }

    /// The phase the update's journal in the installation at `root` records; `None` when there is
    /// no journal.
    fn journal_phase(root: &Path) -> Option<String> {
        let path = root.join(STATE_DIR).join(JOURNAL_DIR).join(UPDATE_JOURNAL);
        let bytes = fs::read(path).ok()?;
        let journal: Value = serde_json::from_slice(&bytes).unwrap();
        Some(journal["phase"].as_str().unwrap().to_owned())
    }

    /// The receipt of the installation at `root`, as its file holds it.
    fn receipt_at(root: &Path) -> Value {
        let bytes = fs::read(root.join(STATE_DIR).join(RECEIPT_FILE)).unwrap();
        serde_json::from_slice(&bytes).unwrap()
    }

    /// Every entry of the installation at `root` outside its state folder, as `tree_entries` gives
    /// them.
    fn outside_entries(root: &Path) -> BTreeMap<String, Option<String>> {
        let state = format!("{STATE_DIR}/");
        tree_entries(root)
            .into_iter()
            .filter(|(path, _)| path != STATE_DIR && !path.starts_with(&state))
            .collect()
    }

    /// How long a stand-in game runs once started, past the time a hold for it begins: on Windows
    /// past the scanner's grace.
    fn running_for() -> Duration {
        let past_hold = Duration::from_millis(300);
        if cfg!(windows) {
            SCANNER_GRACE + past_hold
        } else {
            past_hold
        }
    }

    /// A closed-game probe that finds the game closed for its first `closed_for` calls, then
    /// running for `running` from the first call that finds it so, then closed again.
    struct StartedLater {
        closed_for: u32,
        running: Duration,
        calls: Cell<u32>,
        until: Cell<Option<Instant>>,
    }

    impl StartedLater {
        fn new(closed_for: u32, running: Duration) -> StartedLater {
            StartedLater {
                closed_for,
                running,
                calls: Cell::new(0),
                until: Cell::new(None),
            }
        }

        fn closed(&self, _: &Path) -> Result<(), GameUseError> {
            let call = self.calls.get() + 1;
            self.calls.set(call);
            if call <= self.closed_for {
                return Ok(());
            }
            let until = self
                .until
                .get()
                .unwrap_or_else(|| Instant::now() + self.running);
            self.until.set(Some(until));
            if Instant::now() < until {
                Err(GameUseError::Running)
            } else {
                Ok(())
            }
        }
    }

    /// What an update told its progress of its phases and holds.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Told {
        Phase(&'static str),
        Held(Hold),
        Resumed,
    }

    /// A progress that keeps the phases and holds it is told, and runs `on_hold` as a hold begins.
    struct Heard<'a> {
        told: Mutex<Vec<Told>>,
        on_hold: &'a (dyn Fn(Hold) + Sync),
    }

    impl<'a> Heard<'a> {
        fn new(on_hold: &'a (dyn Fn(Hold) + Sync)) -> Heard<'a> {
            Heard {
                told: Mutex::default(),
                on_hold,
            }
        }

        /// The holds and their ends told, in order.
        fn holds(&self) -> Vec<Told> {
            let told = self.told.lock().unwrap();
            told.iter()
                .copied()
                .filter(|told| !matches!(told, Told::Phase(_)))
                .collect()
        }

        /// Everything told, in order.
        fn told(&self) -> Vec<Told> {
            self.told.lock().unwrap().clone()
        }
    }

    impl Progress for Heard<'_> {
        fn phase(&self, phase: PhaseId) {
            self.told.lock().unwrap().push(Told::Phase(phase.0));
        }

        fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}

        fn hold(&self, hold: Hold) {
            self.told.lock().unwrap().push(Told::Held(hold));
            (self.on_hold)(hold);
        }

        fn resumed(&self) {
            self.told.lock().unwrap().push(Told::Resumed);
        }
    }

    #[test]
    fn an_update_keeps_the_players_and_the_discs_files() {
        let (dir, fixture) = players_installation();
        let root = fixture.root.clone();
        let mut receipt = fixture.receipt();
        receipt["laterKey"] = json!("kept");
        fixture.write_receipt(&receipt).unwrap();
        let disc = (
            tree_entries(&root.join("Gauntlet")),
            tree_entries(&root.join("carddemo")),
        );
        let newer = fixture.runtime.newer(NEXT);
        let (source_path, mut source) = bundled_as(dir.path(), "next-launcher", &newer);
        let source_bytes = fs::read(&source_path).unwrap();

        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        assert_eq!(
            (done.from.clone(), done.to.clone()),
            (version(INSTALLED), version(NEXT))
        );
        let mut replaced = vec![
            "VERSION",
            "build-info.json",
            "data/config.json",
            "data/text/en.json",
            executable(),
            "portable.flag",
        ];
        replaced.sort_unstable();
        assert_eq!(
            done.changes,
            FileChanges {
                replaced: names(&replaced),
                added: names(&["shaders/new.spv"]),
                removed: names(&["shaders/test.spv"]),
                unchanged: 0,
            }
        );
        assert_eq!(done.retired, RetiredCopy::Removed);
        assert_eq!(done.probed, Probed::Staged);
        assert!(done.notices.is_empty(), "{:?}", done.notices);

        // The disc's and the player's files are as they were.
        assert_eq!(
            (
                tree_entries(&root.join("Gauntlet")),
                tree_entries(&root.join("carddemo"))
            ),
            disc
        );
        for name in PLAYERS_FILES {
            assert_eq!(
                fs::read(under(&root, name)).unwrap(),
                PLAYERS_BYTES,
                "{name}"
            );
        }
        // The runtime is the new release's.
        assert!(!root.join("shaders").join("test.spv").exists());
        assert_eq!(
            fs::read(root.join("shaders").join("new.spv")).unwrap(),
            b"new-shader"
        );
        assert_eq!(fs::read(root.join("VERSION")).unwrap(), b"1.0.0-beta.1\n");
        assert_eq!(
            fs::read(root.join(BUILD_INFO)).unwrap(),
            newer.build_info_bytes()
        );
        let game = fs::read(root.join(executable())).unwrap();
        #[cfg(windows)]
        assert_eq!(game, [b"new-runtime".as_slice(), ICON_MARK].concat());
        #[cfg(unix)]
        assert_eq!(game, b"new-runtime");
        // The launcher copy is the file that ran, which is as it was.
        assert_eq!(fs::read(root.join(launcher_copy())).unwrap(), source_bytes);
        assert_eq!(fs::read(&source_path).unwrap(), source_bytes);

        // The receipt records all of it, and keeps what it does not know.
        let written = receipt_at(&root);
        assert_eq!(written["version"], json!(NEXT));
        assert_eq!(written["commit"], json!("new-commit"));
        assert_eq!(written["discRevision"], json!(1));
        assert_eq!(written["laterKey"], json!("kept"));
        assert_eq!(
            written["buildInfoSha256"],
            json!(sha256_hex(&newer.build_info_bytes()))
        );
        assert_eq!(
            written["launcher"]["sha256"],
            json!(sha256_hex(&source_bytes))
        );
        #[cfg(windows)]
        {
            assert_eq!(
                written["installedDigests"][executable()],
                json!(sha256_hex(&game))
            );
            assert_eq!(
                written["discIcon"]["releaseExecutableSha256"],
                json!(sha256_hex(b"new-runtime"))
            );
            assert_eq!(
                written["discIcon"]["sha256"],
                json!(sha256_hex(&fixture.icon))
            );
        }
        assert_eq!(serde_json::to_value(&done.receipt).unwrap(), written);

        // Nothing of the update's is left in the state folder, and the installation reads as the
        // new release's.
        assert_eq!(state_names(&root), [RECEIPT_FILE]);
        let installed = InstallFolder::at(&root)
            .unwrap()
            .installed(&version(NEXT))
            .unwrap();
        assert_eq!(installed.build_info.version(), version(NEXT));
    }

    #[test]
    fn an_offline_bundled_update_installs_this_file() {
        let (dir, fixture) = players_installation();
        let (source_path, mut source) =
            bundled_as(dir.path(), "next-launcher", &fixture.runtime.next(NEXT));
        let source_bytes = fs::read(&source_path).unwrap();
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        assert_eq!(done.retired, RetiredCopy::Removed);
        assert_eq!(
            fs::read(fixture.root.join(launcher_copy())).unwrap(),
            source_bytes
        );
        assert_eq!(fs::read(&source_path).unwrap(), source_bytes);
        assert_eq!(
            done.receipt.launcher.sha256,
            Sha256Digest::of(&source_bytes).hex()
        );
    }

    #[test]
    fn only_the_installed_launcher_copy_changes_outside_the_state_folder() {
        let (dir, fixture) = players_installation();
        let (source_path, mut source) =
            bundled_as(dir.path(), "next-launcher", &fixture.runtime.next(NEXT));
        let source_before = snapshot(&source_path).unwrap();
        let before = outside_entries(&fixture.root);
        updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        let after = outside_entries(&fixture.root);
        let changed: BTreeSet<&str> = before
            .keys()
            .chain(after.keys())
            .filter(|path| before.get(*path) != after.get(*path))
            .map(String::as_str)
            .collect();
        let expected: BTreeSet<&str> = [
            launcher_copy(),
            "VERSION",
            "build-info.json",
            "data/text/en.json",
            "shaders/next.spv",
            "shaders/test.spv",
        ]
        .into_iter()
        .collect();
        assert_eq!(changed, expected);
        assert_eq!(snapshot(&source_path).unwrap(), source_before);
    }

    #[test]
    fn an_equal_source_is_already_current() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime);
        let before = as_it_is(&fixture.root);
        match update_simply(
            &fixture,
            &mut source,
            INSTALLED,
            &standing_in(&NoFaults, &answering),
        ) {
            Ok(UpdateOutcome::AlreadyCurrent { version: current }) => {
                assert_eq!(current, version(INSTALLED));
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(as_it_is(&fixture.root), before);

        // Build metadata alone is no newer release.
        let built = "1.0.0-beta+build.2";
        let (_, mut source) = bundled_as(dir.path(), "built", &fixture.runtime.next(built));
        match update_simply(
            &fixture,
            &mut source,
            built,
            &standing_in(&NoFaults, &answering),
        ) {
            Ok(UpdateOutcome::AlreadyCurrent { .. }) => {}
            other => panic!("{other:?}"),
        }
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn an_installation_updates_through_beta_rc_stable_and_preview() {
        let (dir, fixture) = players_installation();
        let mut runtime = fixture.runtime.clone();
        for (index, step) in ["1.0.0-beta.1", "1.0.0-rc.1", "1.0.0", "1.1.0-preview.1"]
            .into_iter()
            .enumerate()
        {
            runtime = runtime.next(step);
            let (_, mut source) = bundled_as(dir.path(), &format!("launcher-{index}"), &runtime);
            let done = updated(update_simply(
                &fixture,
                &mut source,
                step,
                &standing_in(&NoFaults, &answering),
            ));
            assert_eq!(done.to, version(step));
            assert_eq!(receipt_at(&fixture.root)["version"], json!(step));
            assert_eq!(
                fs::read(fixture.root.join("saves").join("hero.json")).unwrap(),
                PLAYERS_BYTES
            );
        }
        // A launcher older than the installation changes nothing.
        let older = RuntimeFiles::standard(this_platform().as_str(), "1.0.0-rc.2");
        let (_, mut source) = bundled_as(dir.path(), "older", &older);
        let before = as_it_is(&fixture.root);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            "1.0.0-rc.2",
            &standing_in(&NoFaults, &answering),
        ));
        match &failure.error {
            UpdateError::OlderLauncher { running, installed } => {
                assert_eq!(
                    (&**running, &**installed),
                    ("1.0.0-rc.2", "1.1.0-preview.1")
                );
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn byte_identical_files_are_not_moved() {
        use std::io::Read;

        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let config = fixture.root.join("data").join("config.json");
        let held = File::open(&config).unwrap();
        #[cfg(unix)]
        let identity = |path: &Path| {
            use std::os::unix::fs::MetadataExt;
            let metadata = fs::metadata(path).unwrap();
            (metadata.dev(), metadata.ino())
        };
        #[cfg(unix)]
        let before = identity(&config);
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        let moved: Vec<RelPath> = done
            .changes
            .backup()
            .into_iter()
            .chain(done.changes.publish())
            .collect();
        assert!(
            !moved.iter().any(|name| name.as_str() == "data/config.json"),
            "{moved:?}"
        );
        assert_eq!(done.changes.unchanged, 3);
        #[cfg(unix)]
        assert_eq!(identity(&config), before);
        let mut read = Vec::new();
        (&held).read_to_end(&mut read).unwrap();
        assert_eq!(read, b"{}");
        assert_eq!(fs::read(&config).unwrap(), b"{}");
    }

    #[test]
    fn a_changed_runtime_file_is_never_overwritten() {
        let (dir, fixture) = players_installation();
        let game = fixture.root.join(executable());
        let mut bytes = fs::read(&game).unwrap();
        bytes.extend_from_slice(b"personal");
        fs::write(&game, &bytes).unwrap();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = as_it_is(&fixture.root);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        match &failure.error {
            UpdateError::OwnedFileChanged { name } => assert_eq!(&**name, executable()),
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.rule(), "ownedFileChanged");
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);
        assert_eq!(fs::read(&game).unwrap(), bytes);
    }

    #[test]
    fn a_changed_launcher_copy_is_never_overwritten() {
        let (dir, fixture) = players_installation();
        let copy = fixture.root.join(launcher_copy());
        fs::write(&copy, b"a launcher of the player's").unwrap();
        let (source_path, mut source) =
            bundled_as(dir.path(), "next-launcher", &fixture.runtime.next(NEXT));
        let source_bytes = fs::read(&source_path).unwrap();
        let before = as_it_is(&fixture.root);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        match &failure.error {
            UpdateError::OwnedFileChanged { name } => assert_eq!(&**name, launcher_copy()),
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);
        assert_eq!(fs::read(&copy).unwrap(), b"a launcher of the player's");
        assert_eq!(fs::read(&source_path).unwrap(), source_bytes);
    }

    #[test]
    fn a_missing_launcher_copy_is_restored() {
        let (dir, fixture) = players_installation();
        let copy = fixture.root.join(launcher_copy());
        retry_locked(|| fs::remove_file(&copy)).unwrap();
        let (source_path, mut source) =
            bundled_as(dir.path(), "next-launcher", &fixture.runtime.next(NEXT));
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        assert_eq!(done.retired, RetiredCopy::NotMoved);
        assert_eq!(fs::read(&copy).unwrap(), fs::read(&source_path).unwrap());
        assert_eq!(state_names(&fixture.root), [RECEIPT_FILE]);
    }

    #[test]
    fn receipts_that_disagree_or_another_platform_are_refused() {
        let (dir, fixture) = players_installation();
        let mut receipt = fixture.receipt();
        receipt["version"] = json!("1.0.0-alpha.2");
        fixture.write_receipt(&receipt).unwrap();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = as_it_is(&fixture.root);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        match &failure.error {
            UpdateError::Receipt(ReceiptError::Disagrees { field }) => {
                assert_eq!(*field, "version")
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);

        // An installation of the other platform's, where the system can hold one: a system that
        // ignores letter case cannot hold Linux's executable beside the disc's folder.
        if cfg!(unix) {
            let other = tempfile::tempdir().unwrap();
            let runtime = RuntimeFiles::standard(other_platform().as_str(), INSTALLED);
            let fixture = InstallFixture::write(&other.path().join("install"), runtime).unwrap();
            let before = as_it_is(&fixture.root);
            let failure = failed(update_simply(
                &fixture,
                &mut source,
                NEXT,
                &standing_in(&NoFaults, &answering),
            ));
            assert!(
                matches!(
                    failure.error,
                    UpdateError::Receipt(ReceiptError::OtherPlatform { .. })
                ),
                "{failure:?}"
            );
            assert_eq!(as_it_is(&fixture.root), before);
        }
    }

    #[test]
    fn too_little_space_changes_nothing() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = as_it_is(&fixture.root);
        let full = |_: &Path| -> io::Result<Volume> {
            Ok(Volume {
                available: 0,
                noexec: false,
            })
        };
        let folders = FolderChecks {
            volume: &full,
            ..FolderChecks::SHIPPED
        };
        let settings = UpdateSettings {
            folders: &folders,
            ..standing_in(&NoFaults, &answering)
        };
        let failure = failed(update_simply(&fixture, &mut source, NEXT, &settings));
        match &failure.error {
            UpdateError::Space { needed, available } => {
                assert_eq!(*available, 0);
                assert!(*needed > SPACE_MARGIN);
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn a_damaged_source_leaves_the_installation() {
        let (dir, fixture) = players_installation();
        let mut raw = fixture.runtime.newer(NEXT).raw();
        let entry = raw
            .entries
            .iter_mut()
            .find(|entry| entry.name == b"data/config.json")
            .unwrap();
        *entry = RawEntry::deflated("data/config.json", b"bad");
        let (_, mut source) = carrying(dir.path(), "damaged", &raw.to_bytes());
        let before = as_it_is(&fixture.root);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        match &failure.error {
            UpdateError::Runtime(PayloadError::Damaged { name, .. }) => {
                assert_eq!(&**name, "data/config.json");
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.changes, Changes::RolledBack);
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn a_job_left_unfinished_refuses_the_update() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let journals = fixture.root.join(STATE_DIR).join(JOURNAL_DIR);
        fs::create_dir(&journals).unwrap();
        let journals_of = [
            (
                INSTALL_JOURNAL,
                json!({"schema": 1, "minLauncher": INSTALLED}),
                "unfinishedJob",
            ),
            (
                UPDATE_JOURNAL,
                json!({"schema": 1, "minLauncher": INSTALLED, "phase": "staged"}),
                "unfinishedJob",
            ),
            (
                UPDATE_JOURNAL,
                json!({"schema": 2, "minLauncher": INSTALLED}),
                "newerLauncher",
            ),
            (
                "later.json",
                json!({"schema": 1, "minLauncher": INSTALLED}),
                "newerLauncher",
            ),
        ];
        for (name, journal, rule) in journals_of {
            let path = journals.join(name);
            fs::write(&path, journal.to_string()).unwrap();
            let before = as_it_is(&fixture.root);
            let failure = failed(update_simply(
                &fixture,
                &mut source,
                NEXT,
                &standing_in(&NoFaults, &answering),
            ));
            assert_eq!(failure.rule(), rule, "{name}: {failure:?}");
            if let UpdateError::UnfinishedJob { journal } = &failure.error {
                assert_eq!(&**journal, name);
            }
            assert_eq!(failure.changes, Changes::None);
            assert_eq!(as_it_is(&fixture.root), before);
            retry_locked(|| fs::remove_file(&path)).unwrap();
        }

        // What an interrupted write of a journal left is no journal, and goes with the update.
        let pending = journals.join("update.json.pending");
        fs::write(&pending, b"{").unwrap();
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        assert_eq!(done.to, version(NEXT));
        assert!(!pending.exists());
    }

    #[test]
    fn a_running_game_or_another_job_holds_and_a_cancel_changes_nothing() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = as_it_is(&fixture.root);

        // A game that runs as the update would stage.
        let cancel = Cancel::default();
        let stop = |_: Hold| cancel.cancel();
        let heard = Heard::new(&stop);
        let job = Job {
            cancel: &cancel,
            progress: &heard,
        };
        let game = StartedLater::new(0, Duration::from_secs(3600));
        let game_closed = |path: &Path| game.closed(path);
        let settings = UpdateSettings {
            game_closed: &game_closed,
            ..standing_in(&NoFaults, &answering)
        };
        let failure = failed(update_by(&fixture, &mut source, NEXT, &job, &settings));
        assert!(
            matches!(failure.error, UpdateError::Cancelled),
            "{failure:?}"
        );
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(heard.holds(), [Told::Held(Hold::GameRunning)]);
        assert_eq!(as_it_is(&fixture.root), before);

        // Another job's lock.
        let held = InstallFolder::at(&fixture.root).unwrap().lock().unwrap();
        let cancel = Cancel::default();
        let stop = |_: Hold| cancel.cancel();
        let heard = Heard::new(&stop);
        let job = Job {
            cancel: &cancel,
            progress: &heard,
        };
        let failure = failed(update_by(
            &fixture,
            &mut source,
            NEXT,
            &job,
            &standing_in(&NoFaults, &answering),
        ));
        assert!(
            matches!(failure.error, UpdateError::Cancelled),
            "{failure:?}"
        );
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(heard.holds(), [Told::Held(Hold::LockBusy)]);
        drop(held);
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn a_game_started_after_staging_holds_with_the_staging_kept() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let root = fixture.root.clone();
        let seen = Mutex::new(None);
        let look = |_: Hold| {
            let staged =
                stagings(&root).len() == 1 && journal_phase(&root).as_deref() == Some("staged");
            *seen.lock().unwrap() = Some(staged);
        };
        let heard = Heard::new(&look);
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &heard,
        };
        let game = StartedLater::new(1, running_for());
        let game_closed = |path: &Path| game.closed(path);
        let settings = UpdateSettings {
            game_closed: &game_closed,
            ..standing_in(&NoFaults, &answering)
        };
        let done = updated(update_by(&fixture, &mut source, NEXT, &job, &settings));
        assert_eq!(done.to, version(NEXT));
        assert_eq!(
            heard.holds(),
            [Told::Held(Hold::GameRunning), Told::Resumed]
        );
        assert_eq!(*seen.lock().unwrap(), Some(true));
        // The hold came as the staged game was to be checked.
        let told = heard.told();
        let checking = told
            .iter()
            .position(|told| *told == Told::Phase(crate::CHECK_GAME.0))
            .unwrap();
        let held = told
            .iter()
            .position(|told| *told == Told::Held(Hold::GameRunning))
            .unwrap();
        assert!(checking < held, "{told:?}");
    }

    #[test]
    fn a_cancel_during_the_staged_hold_takes_the_update_back() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = as_it_is(&fixture.root);
        let cancel = Cancel::default();
        let stop = |_: Hold| cancel.cancel();
        let heard = Heard::new(&stop);
        let job = Job {
            cancel: &cancel,
            progress: &heard,
        };
        let game = StartedLater::new(1, Duration::from_secs(3600));
        let game_closed = |path: &Path| game.closed(path);
        let settings = UpdateSettings {
            game_closed: &game_closed,
            ..standing_in(&NoFaults, &answering)
        };
        let failure = failed(update_by(&fixture, &mut source, NEXT, &job, &settings));
        assert!(
            matches!(failure.error, UpdateError::Cancelled),
            "{failure:?}"
        );
        assert_eq!(failure.changes, Changes::RolledBack);
        assert_eq!(heard.holds(), [Told::Held(Hold::GameRunning)]);
        assert_eq!(as_it_is(&fixture.root), before);
        assert_eq!(journal_phase(&fixture.root), None);
    }

    #[test]
    fn a_staged_game_that_fails_while_the_installed_one_answers_refuses() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let before = as_it_is(&fixture.root);
        let staged = fixture.root.join(STATE_DIR);
        let ran = Mutex::new(Vec::new());
        let probe = |path: &Path, _: &Cancel| {
            ran.lock().unwrap().push(path.to_path_buf());
            answered(if path.starts_with(&staged) { 3 } else { 0 })
        };
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &probe),
        ));
        match &failure.error {
            UpdateError::ProbeRefused { detail } => assert!(!detail.is_empty()),
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.rule(), "probeRefused");
        assert_eq!(failure.changes, Changes::RolledBack);
        let ran = ran.into_inner().unwrap();
        assert_eq!(ran.len(), 2, "{ran:?}");
        assert!(ran[0].starts_with(&staged), "{ran:?}");
        assert_eq!(ran[1], fixture.root.join(executable()));
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn a_staged_game_that_fails_like_the_installed_one_goes_on_with_a_notice() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let failing = |_: &Path, _: &Cancel| answered(3);
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &failing),
        ));
        assert_eq!(done.probed, Probed::Staged);
        assert_eq!(
            done.notices,
            [UpdateNotice::ProbeFailed {
                status: Some(3),
                first_line: Some("no".to_owned()),
                stderr_tail: "the game is broken".to_owned(),
            }]
        );
        assert_eq!(receipt_at(&fixture.root)["version"], json!(NEXT));
    }

    #[test]
    fn a_staged_game_that_answers_probes_nothing_else() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let ran = Mutex::new(Vec::new());
        let probe = |path: &Path, _: &Cancel| {
            ran.lock().unwrap().push(path.to_path_buf());
            answered(0)
        };
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &probe),
        ));
        assert_eq!(done.probed, Probed::Staged);
        assert!(done.notices.is_empty(), "{:?}", done.notices);
        let ran = ran.into_inner().unwrap();
        assert_eq!(ran.len(), 1, "{ran:?}");
        let staging = ran[0].parent().unwrap();
        assert_eq!(staging.parent().unwrap(), fixture.root.join(STATE_DIR));
        assert!(
            staging
                .file_name()
                .unwrap()
                .to_str()
                .unwrap()
                .starts_with("stage-")
        );
        assert_eq!(ran[0].file_name().unwrap(), executable());
    }

    #[test]
    fn an_unchanged_executable_is_not_probed() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let ran = Mutex::new(Vec::new());
        let probe = |path: &Path, _: &Cancel| {
            ran.lock().unwrap().push(path.to_path_buf());
            answered(0)
        };
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &probe),
        ));
        assert_eq!(done.probed, Probed::NotStaged);
        assert!(ran.into_inner().unwrap().is_empty());
    }

    #[test]
    fn the_staged_game_is_probed_beside_its_libraries() {
        let (dir, fixture) = library_installation();
        let mut next = fixture.runtime.next(NEXT);
        set_file(&mut next, executable(), b"a new game");
        let mut source = bundled(dir.path(), &next);

        // The plan counts the library staged beside the game in the room it needs.
        let plan = plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut source),
        )
        .unwrap();
        let changes = plan.changes.unwrap();
        let publish: Vec<String> = changes
            .publish()
            .iter()
            .map(|name| name.as_str().to_owned())
            .collect();
        assert!(
            publish.iter().any(|name| name == executable()),
            "{publish:?}"
        );
        assert!(!publish.iter().any(|name| name == LIBRARY), "{publish:?}");
        let texts: Vec<&str> = publish
            .iter()
            .map(String::as_str)
            .chain([LIBRARY])
            .collect();
        let SourceSummary::Bundled { file_len } = plan.source else {
            panic!("{:?}", plan.source);
        };
        assert_eq!(
            plan.needed_bytes,
            Some(bytes_of(&next, &texts) + file_len + SPACE_MARGIN)
        );

        let ran = Mutex::new(Vec::new());
        let probe = |path: &Path, _: &Cancel| {
            ran.lock().unwrap().push(path.to_path_buf());
            let beside = under(path.parent().unwrap(), LIBRARY);
            answered(if beside.is_file() { 0 } else { 5 })
        };
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &probe),
        ));
        assert_eq!(done.probed, Probed::Staged);
        assert!(done.notices.is_empty(), "{:?}", done.notices);
        let ran = ran.into_inner().unwrap();
        assert_eq!(ran.len(), 1, "{ran:?}");
        assert!(ran[0].starts_with(fixture.root.join(STATE_DIR)), "{ran:?}");
        let moved: Vec<RelPath> = done
            .changes
            .backup()
            .into_iter()
            .chain(done.changes.publish())
            .collect();
        assert!(
            !moved.iter().any(|name| name.as_str() == LIBRARY),
            "{moved:?}"
        );
        assert_eq!(
            fs::read(under(&fixture.root, LIBRARY)).unwrap(),
            b"a library"
        );
        assert!(stagings(&fixture.root).is_empty());
    }

    #[test]
    fn a_library_that_changes_beside_the_same_executable_is_probed() {
        let (dir, fixture) = library_installation();
        let receipt = fixture.receipt();
        let game = fs::read(fixture.root.join(executable())).unwrap();
        let mut next = fixture.runtime.next(NEXT);
        set_file(&mut next, LIBRARY, b"a new library");
        let mut source = bundled(dir.path(), &next);
        let ran = Mutex::new(Vec::new());
        let probe = |path: &Path, _: &Cancel| {
            ran.lock().unwrap().push(path.to_path_buf());
            answered(0)
        };
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &probe),
        ));
        assert_eq!(done.probed, Probed::Staged);
        assert!(
            done.changes
                .replaced
                .iter()
                .any(|name| name.as_str() == LIBRARY)
        );
        assert!(
            !done
                .changes
                .replaced
                .iter()
                .any(|name| name.as_str() == executable())
        );
        let ran = ran.into_inner().unwrap();
        assert_eq!(ran.len(), 1, "{ran:?}");
        assert!(ran[0].starts_with(fixture.root.join(STATE_DIR)), "{ran:?}");
        assert_eq!(ran[0].file_name().unwrap(), executable());
        // The executable stayed where it was, with what the receipt records of it.
        assert_eq!(fs::read(fixture.root.join(executable())).unwrap(), game);
        let written = receipt_at(&fixture.root);
        assert_eq!(written["installedDigests"], receipt["installedDigests"]);
        assert_eq!(written.get("discIcon"), receipt.get("discIcon"));
        assert_eq!(
            fs::read(under(&fixture.root, LIBRARY)).unwrap(),
            b"a new library"
        );
    }

    #[cfg(windows)]
    #[test]
    fn a_windows_receipt_whose_icon_record_disagrees_is_refused() {
        for (key, value) in [
            ("path", json!("carddemo/other.tpl")),
            (
                "releaseExecutableSha256",
                json!(sha256_hex(b"another game")),
            ),
        ] {
            let (dir, fixture) = players_installation();
            let mut receipt = fixture.receipt();
            receipt["discIcon"][key] = value;
            fixture.write_receipt(&receipt).unwrap();
            let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
            let before = as_it_is(&fixture.root);
            let failure = failed(update_simply(
                &fixture,
                &mut source,
                NEXT,
                &standing_in(&NoFaults, &answering),
            ));
            match &failure.error {
                UpdateError::Receipt(ReceiptError::Disagrees { field }) => {
                    assert_eq!(*field, "discIcon", "{key}");
                }
                other => panic!("{key}: {other:?}"),
            }
            assert_eq!(failure.rule(), "disagrees");
            assert_eq!(failure.changes, Changes::None);
            assert_eq!(as_it_is(&fixture.root), before);
        }
    }

    #[cfg(windows)]
    #[test]
    fn the_icon_is_read_from_its_fixed_place() {
        // A record that names another file is refused by the record's check alone, and that file
        // is never read.
        let (dir, fixture) = players_installation();
        let mut receipt = fixture.receipt();
        receipt["discIcon"]["path"] = json!("carddemo/missing.tpl");
        fixture.write_receipt(&receipt).unwrap();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        assert!(
            matches!(
                failure.error,
                UpdateError::Receipt(ReceiptError::Disagrees { field: "discIcon" })
            ),
            "{failure:?}"
        );

        // With a sound record, the icon set into the new executable is the one at its place.
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let read = Mutex::new(Vec::new());
        let personalise = |executable: &Path, tpl: &[u8]| {
            read.lock().unwrap().push(tpl.to_vec());
            marking(executable, tpl)
        };
        let settings = UpdateSettings {
            personalise: &personalise,
            ..standing_in(&NoFaults, &answering)
        };
        updated(update_simply(&fixture, &mut source, NEXT, &settings));
        assert_eq!(
            read.lock().unwrap().as_slice(),
            std::slice::from_ref(&fixture.icon)
        );
        let written = receipt_at(&fixture.root);
        assert_eq!(written["discIcon"]["path"], json!(crate::icon::DISC_ICON));
        assert_eq!(
            written["discIcon"]["sha256"],
            json!(sha256_hex(&fixture.icon))
        );
        let game = [b"new-runtime".as_slice(), ICON_MARK].concat();
        assert_eq!(fs::read(fixture.root.join(executable())).unwrap(), game);
        assert_eq!(
            written["installedDigests"][executable()],
            json!(sha256_hex(&game))
        );
    }

    #[test]
    fn a_receipt_write_that_failed_after_its_rename_is_the_commit() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let written_then_failed =
            |receipt: &Receipt, folder: &InstallFolder| -> Result<(), FilesError> {
                receipt.write(folder)?;
                Err(FilesError::Io {
                    path: folder.state_dir().join(RECEIPT_FILE),
                    source: io::Error::other("the folder's sync failed"),
                })
            };
        let settings = UpdateSettings {
            write_receipt: &written_then_failed,
            ..standing_in(&NoFaults, &answering)
        };
        let done = updated(update_simply(&fixture, &mut source, NEXT, &settings));
        assert!(done.notices.is_empty(), "{:?}", done.notices);
        assert_eq!(
            receipt_at(&fixture.root),
            serde_json::to_value(&done.receipt).unwrap()
        );
        assert_eq!(journal_phase(&fixture.root), None);
        assert_eq!(state_names(&fixture.root), [RECEIPT_FILE]);
    }

    #[test]
    fn a_receipt_too_large_is_refused_before_staging() {
        let (dir, fixture) = players_installation();
        let mut receipt = fixture.receipt();
        receipt["padding"] = json!("");
        let unpadded = pretty_json(&receipt).len() as u64;
        // The installed receipt fits its bound, a byte short; the new one names a longer version.
        let padding = usize::try_from(MAX_RECEIPT_LEN - 1 - unpadded).unwrap();
        receipt["padding"] = json!("p".repeat(padding));
        fixture.write_receipt(&receipt).unwrap();
        assert_eq!(pretty_json(&receipt).len() as u64, MAX_RECEIPT_LEN - 1);
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = as_it_is(&fixture.root);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        match &failure.error {
            UpdateError::Files(FilesError::TooLarge { limit, .. }) => {
                assert_eq!(*limit, MAX_RECEIPT_LEN);
            }
            other => panic!("{other:?}"),
        }
        // Refused before a file was staged.
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);
    }

    /// Runs `act` once, at the first pass of `point` that finds the update's journal at
    /// `journal`.
    struct OnceJournalled<'a> {
        point: FaultPoint,
        journal: PathBuf,
        act: &'a dyn Fn(),
        done: Cell<bool>,
    }

    impl FaultPoints for OnceJournalled<'_> {
        fn pass(&self, point: FaultPoint) -> io::Result<()> {
            if point == self.point && !self.done.get() && self.journal.exists() {
                self.done.set(true);
                (self.act)();
            }
            Ok(())
        }
    }

    #[test]
    fn a_link_planted_after_staging_refuses_the_swap() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let shaders = fixture.root.join("shaders");
        let outside = dir.path().join("outside");
        let shaders_before = tree_entries(&shaders);
        let plant = || {
            fs::rename(&shaders, &outside).unwrap();
            link_folder(&shaders, &outside).unwrap();
        };
        let faults = OnceJournalled {
            point: FaultPoint::UpdateStage,
            journal: fixture
                .root
                .join(STATE_DIR)
                .join(JOURNAL_DIR)
                .join(UPDATE_JOURNAL),
            act: &plant,
            done: Cell::new(false),
        };
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&faults, &answering),
        ));
        assert!(faults.done.get());
        match &failure.error {
            UpdateError::Links { path } => assert_eq!(path, &shaders),
            other => panic!("{other:?}"),
        }
        assert_eq!(failure.changes, Changes::RolledBack);
        // Nothing reached the folder the link leads to, and nothing of the update's is left.
        assert_eq!(tree_entries(&outside), shaders_before);
        assert!(stagings(&fixture.root).is_empty());
        assert_eq!(journal_phase(&fixture.root), None);
        assert_eq!(receipt_at(&fixture.root)["version"], json!(INSTALLED));
    }

    #[test]
    fn a_link_planted_mid_swap_stops_the_moves() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let text = fixture.root.join("data").join("text");
        let outside = dir.path().join("outside");
        let text_before = tree_entries(&text);
        let plant = || -> io::Result<()> {
            fs::rename(&text, &outside)?;
            link_folder(&text, &outside)
        };
        // After the second move aside: VERSION's, then build-info.json's.
        let faults = OnPass::new(FaultPoint::UpdateMove, 2, &plant);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&faults, &answering),
        ));
        match &failure.error {
            UpdateError::Links { path } => assert_eq!(path, &text),
            other => panic!("{other:?}"),
        }
        // The moves stopped there and were taken back, and nothing reached the folder the link
        // leads to.
        assert_eq!(failure.changes, Changes::RolledBack);
        assert_eq!(journal_phase(&fixture.root), None);
        assert_eq!(tree_entries(&outside), text_before);
        assert_eq!(receipt_at(&fixture.root)["version"], json!(INSTALLED));
        assert_eq!(
            fs::read(fixture.root.join("VERSION")).unwrap(),
            b"1.0.0-beta\n"
        );
        assert_eq!(
            fs::read(fixture.root.join(BUILD_INFO)).unwrap(),
            fixture.runtime.build_info_bytes()
        );
        assert_eq!(state_names(&fixture.root), [RECEIPT_FILE]);
    }

    #[test]
    fn every_move_that_fails_is_taken_back() {
        // Each move aside and into place of a release that replaces, adds and removes names, the
        // pass as the journal records the swap, and both of the launcher copy's moves.
        let moves = 14;
        let mut failing: Vec<(FaultPoint, u32)> =
            (1..=moves).map(|at| (FaultPoint::UpdateMove, at)).collect();
        failing.extend([
            (FaultPoint::UpdateCommit, 1),
            (FaultPoint::LauncherReplace, 1),
            (FaultPoint::LauncherReplace, 2),
        ]);
        for (point, at) in failing {
            let context = format!("{} {at}", point.as_str());
            let (dir, fixture) = players_installation();
            let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
            let before = contents_of(&fixture.root);
            let fail = FailAt::new(point, at);
            let failure = failed(update_simply(
                &fixture,
                &mut source,
                NEXT,
                &standing_in(&fail, &answering),
            ));
            assert_eq!(failure.rule(), "io", "{context}: {failure:?}");
            assert_eq!(failure.changes, Changes::RolledBack, "{context}");
            assert_eq!(contents_of(&fixture.root), before, "{context}");
        }
        // Those were every move the update makes of the runtime's files.
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let past = FailAt::new(FaultPoint::UpdateMove, moves + 1);
        updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&past, &answering),
        ));
    }

    #[test]
    fn a_receipt_write_refused_is_taken_back() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let path = fixture.root.join(STATE_DIR).join(RECEIPT_FILE);
        let old = fs::read(&path).unwrap();
        let before = contents_of(&fixture.root);
        let refused = |_: &Receipt, folder: &InstallFolder| -> Result<(), FilesError> {
            Err(FilesError::Io {
                path: folder.state_dir().join(RECEIPT_FILE),
                source: io::Error::other("the disk refused the write"),
            })
        };
        let settings = UpdateSettings {
            write_receipt: &refused,
            ..standing_in(&NoFaults, &answering)
        };
        let failure = failed(update_simply(&fixture, &mut source, NEXT, &settings));
        assert!(
            matches!(failure.error, UpdateError::Files(FilesError::Io { .. })),
            "{failure:?}"
        );
        assert_eq!(failure.changes, Changes::RolledBack);
        assert_eq!(contents_of(&fixture.root), before);
        assert_eq!(fs::read(&path).unwrap(), old);
    }

    #[test]
    fn a_launcher_move_refused_is_taken_back_and_a_rerun_finishes() {
        let (dir, fixture) = players_installation();
        let (source_path, mut source) =
            bundled_as(dir.path(), "next-launcher", &fixture.runtime.next(NEXT));
        let before = contents_of(&fixture.root);
        let fail = FailAt::new(FaultPoint::LauncherReplace, 1);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&fail, &answering),
        ));
        assert_eq!(failure.changes, Changes::RolledBack);
        assert_eq!(contents_of(&fixture.root), before);

        // Nothing is left in the way of the same update again.
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        assert_eq!(done.to, version(NEXT));
        assert_eq!(done.retired, RetiredCopy::Removed);
        assert_eq!(
            fs::read(fixture.root.join(launcher_copy())).unwrap(),
            fs::read(&source_path).unwrap()
        );
        assert_eq!(state_names(&fixture.root), [RECEIPT_FILE]);
    }

    /// The names in the state folder of the installation at `root` that a launcher copy of
    /// `version` was retired to.
    fn retired_copies(root: &Path, version: &str) -> Vec<String> {
        let prefix = format!("{RETIRED_LAUNCHER_PREFIX}{version}-");
        state_names(root)
            .into_iter()
            .filter(|name| name.starts_with(&prefix))
            .collect()
    }

    #[test]
    fn a_rollback_retires_the_new_launcher_copy() {
        let (dir, fixture) = players_installation();
        let (source_path, mut source) =
            bundled_as(dir.path(), "next-launcher", &fixture.runtime.next(NEXT));
        let new = fs::read(&source_path).unwrap();
        let installed = fixture.root.join(launcher_copy());
        let old = fs::read(&installed).unwrap();
        let state = fixture.root.join(STATE_DIR);
        let before = contents_of(&fixture.root);
        // The new copy stands in place when the update fails.
        let in_place = Cell::new(false);
        let refuse = || {
            in_place.set(fs::read(&installed)? == new);
            Err(io::Error::other("the receipt's volume went away"))
        };
        let replaced = OnPass::new(FaultPoint::LauncherReplace, 2, &refuse);
        // As the rollback's first move is made, the new copy has gone aside under a retired name
        // of its own version, not into the staging, and nothing stands in its place.
        let aside = Cell::new(false);
        let look = || {
            let retired = retired_copies(&fixture.root, NEXT);
            let staged = stagings(&fixture.root)
                .iter()
                .any(|staging| state.join(staging).join(launcher_copy()).exists());
            let retired_new = match retired.as_slice() {
                [name] => fs::read(state.join(name))? == new,
                _ => false,
            };
            aside.set(retired_new && !staged && !installed.exists());
            Ok(())
        };
        let retiring = OnPass::new(FaultPoint::UpdateRollback, 1, &look);
        let faults = Faults(&[&replaced, &retiring]);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&faults, &answering),
        ));
        assert!(in_place.get());
        assert!(aside.get());
        assert_eq!(failure.changes, Changes::RolledBack);
        assert_eq!(fs::read(&installed).unwrap(), old);
        // No retired copy of either version is left, nor the staging.
        assert_eq!(contents_of(&fixture.root), before);
    }

    #[cfg(windows)]
    #[test]
    fn a_new_copy_held_as_an_image_is_left_for_the_sweep() {
        use std::cell::RefCell;

        use crate::platform::map_image;

        let (dir, fixture) = players_installation();
        // The new launcher copy is this program with the release's runtime, which the system maps
        // as it maps a launcher that runs from it.
        let mut new = fs::read(std::env::current_exe().unwrap()).unwrap();
        append_sections(
            &mut new,
            &[(RUNTIME_SECTION, &fixture.runtime.next(NEXT).zip())],
            0,
        );
        let source_path = dir.path().join("next-launcher.exe");
        fs::write(&source_path, &new).unwrap();
        let mut source = Sections::of_path(&source_path).unwrap().unwrap();
        let installed = fixture.root.join(launcher_copy());
        let old = fs::read(&installed).unwrap();
        let mapped = RefCell::new(None);
        let run = || {
            *mapped.borrow_mut() = Some(map_image(&installed)?);
            Err(io::Error::other("the receipt's volume went away"))
        };
        let faults = OnPass::new(FaultPoint::LauncherReplace, 2, &run);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&faults, &answering),
        ));
        assert!(mapped.borrow().is_some());
        assert_eq!(failure.changes, Changes::RolledBack);
        assert_eq!(fs::read(&installed).unwrap(), old);
        assert!(stagings(&fixture.root).is_empty());
        assert_eq!(journal_phase(&fixture.root), None);
        // The held copy stays under its retired name, which no journal names.
        let retired = retired_copies(&fixture.root, NEXT);
        let [name] = retired.as_slice() else {
            panic!("{retired:?}");
        };
        assert!(name.ends_with(".exe"), "{name}");
        assert_eq!(state_names(&fixture.root), [RECEIPT_FILE, name.as_str()]);
        let path = fixture.root.join(STATE_DIR).join(name);
        assert_eq!(fs::read(&path).unwrap(), new);
        // The mapping alone holds it: refused while mapped, removed once let go.
        assert!(fs::remove_file(&path).is_err());
        drop(mapped.take());
        retry_locked(|| fs::remove_file(&path)).unwrap();
    }

    #[test]
    fn a_rollback_that_fails_keeps_the_backups_for_the_next() {
        let (dir, fixture) = players_installation();
        let root = fixture.root.clone();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let old_launcher = fs::read(root.join(launcher_copy())).unwrap();
        let replace = FailAt::new(FaultPoint::LauncherReplace, 2);
        // The rollback's third pass: the new launcher copy gone aside, the installed one back,
        // and the last new name back in the staging.
        let rollback = FailAt::new(FaultPoint::UpdateRollback, 3);
        let faults = Faults(&[&replace, &rollback]);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&faults, &answering),
        ));
        // The failure reported is the update's own.
        assert_eq!(failure.rule(), "io");
        assert_eq!(failure.changes, Changes::Kept);
        assert_eq!(journal_phase(&root).as_deref(), Some("rollingBack"));
        // The installed files wait in the backup for the next start to put back.
        let journal: Value = serde_json::from_slice(
            &fs::read(root.join(STATE_DIR).join(JOURNAL_DIR).join(UPDATE_JOURNAL)).unwrap(),
        )
        .unwrap();
        let backup: Vec<&str> = journal["backup"]
            .as_array()
            .unwrap()
            .iter()
            .map(|name| name.as_str().unwrap())
            .collect();
        assert_eq!(backup.len(), 7);
        let aside = root.join(STATE_DIR).join(UPDATE_BACKUP);
        for name in &backup {
            assert!(under(&aside, name).is_file(), "{name}");
        }
        assert_eq!(fs::read(under(&aside, "VERSION")).unwrap(), b"1.0.0-beta\n");
        assert_eq!(stagings(&root).len(), 1);
        assert_eq!(fs::read(root.join(launcher_copy())).unwrap(), old_launcher);
        assert_eq!(receipt_at(&root)["version"], json!(INSTALLED));
    }

    #[test]
    fn a_file_left_in_the_backup_keeps_the_journal() {
        let (dir, fixture) = players_installation();
        let root = fixture.root.clone();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let stray = root
            .join(STATE_DIR)
            .join(UPDATE_BACKUP)
            .join("do-not-delete");
        let leave = || fs::write(&stray, b"kept for whoever looks");
        let left = OnPass::new(FaultPoint::UpdateMove, 1, &leave);
        let fail = FailAt::new(FaultPoint::UpdateMove, 2);
        let faults = Faults(&[&left, &fail]);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&faults, &answering),
        ));
        assert_eq!(failure.changes, Changes::Kept);
        assert_eq!(journal_phase(&root).as_deref(), Some("rollingBack"));
        assert_eq!(fs::read(&stray).unwrap(), b"kept for whoever looks");
        // What moved was put back, and the staging went, before the backup was found holding
        // what nobody named.
        assert_eq!(fs::read(root.join("VERSION")).unwrap(), b"1.0.0-beta\n");
        assert!(stagings(&root).is_empty());
        assert_eq!(receipt_at(&root)["version"], json!(INSTALLED));
    }

    #[test]
    fn a_panic_while_staging_or_swapping_keeps_what_recovery_needs() {
        use std::panic::{AssertUnwindSafe, catch_unwind};

        // At the staging's pass once the journal is written, after the third move, and as the
        // installed launcher copy went aside: the journal's phase then, and how many installed
        // files were moved aside.
        for (at, phase, backed_up) in [
            ("journal", "staged", 0),
            ("move", "swapping", 3),
            ("launcher", "swapping", 7),
        ] {
            let (dir, fixture) = players_installation();
            let root = fixture.root.clone();
            let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
            let journalled = OnceJournalled {
                point: FaultPoint::UpdateStage,
                journal: root.join(STATE_DIR).join(JOURNAL_DIR).join(UPDATE_JOURNAL),
                act: &|| panic!("the test panics once the journal is written"),
                done: Cell::new(false),
            };
            let moved = PanicAt::new(FaultPoint::UpdateMove, 3);
            let retired = PanicAt::new(FaultPoint::LauncherReplace, 1);
            let faults: &dyn FaultPoints = match at {
                "journal" => &journalled,
                "move" => &moved,
                _ => &retired,
            };
            let unwound = catch_unwind(AssertUnwindSafe(|| {
                update_simply(
                    &fixture,
                    &mut source,
                    NEXT,
                    &standing_in(faults, &answering),
                )
            }));
            assert!(unwound.is_err(), "{at}");
            // What the pass left stays for the next start: the journal, the staging and the
            // installed files moved aside; and the lock was let go as the job unwound.
            assert_eq!(journal_phase(&root).as_deref(), Some(phase), "{at}");
            assert_eq!(stagings(&root).len(), 1, "{at}");
            let aside = root.join(STATE_DIR).join(UPDATE_BACKUP);
            let files = if aside.exists() {
                tree_entries(&aside)
                    .values()
                    .filter(|entry| entry.is_some())
                    .count()
            } else {
                0
            };
            assert_eq!(files, backed_up, "{at}");
            assert_eq!(
                retired_copies(&root, INSTALLED).len(),
                usize::from(at == "launcher")
            );
            drop(InstallFolder::at(&root).unwrap().lock().unwrap());
        }
    }

    /// What [`CancelAt`] has been told: how many calls, the phase last begun, and, at the call it
    /// cancels at, that phase and whether the update's staging stood then.
    #[derive(Default)]
    struct Calls {
        count: usize,
        phase: Option<&'static str>,
        cancelled: Option<(&'static str, bool)>,
    }

    /// A progress that asks the update of the installation at `root` to stop at its `at`-th call,
    /// phases and advances counted together.
    struct CancelAt<'a> {
        cancel: Cancel,
        at: usize,
        root: &'a Path,
        calls: Mutex<Calls>,
    }

    impl CancelAt<'_> {
        fn call(&self, phase: Option<&'static str>) {
            let mut calls = self.calls.lock().unwrap();
            if phase.is_some() {
                calls.phase = phase;
            }
            calls.count += 1;
            if calls.count == self.at {
                let begun = calls.phase.expect("a phase is told before anything else");
                calls.cancelled = Some((begun, !stagings(self.root).is_empty()));
                self.cancel.cancel();
            }
        }
    }

    impl Progress for CancelAt<'_> {
        fn phase(&self, phase: PhaseId) {
            self.call(Some(phase.0));
        }

        fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {
            self.call(None);
        }
    }

    #[test]
    fn a_cancel_at_any_step_before_the_swap_leaves_nothing() {
        let mut reached = BTreeSet::new();
        for at in 1.. {
            let (dir, fixture) = players_installation();
            let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
            let before = as_it_is(&fixture.root);
            let cancel = Cancel::default();
            let progress = CancelAt {
                cancel: cancel.clone(),
                at,
                root: &fixture.root,
                calls: Mutex::default(),
            };
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let result = update_by(
                &fixture,
                &mut source,
                NEXT,
                &job,
                &standing_in(&NoFaults, &answering),
            );
            let calls = progress.calls.into_inner().unwrap();
            let Some((phase, staged)) = calls.cancelled else {
                // The update told fewer calls than that, so nothing asked it to stop.
                assert_eq!(calls.count, at - 1);
                updated(result);
                break;
            };
            let context = format!("call {at}, in {phase}, a staging standing: {staged}");
            if phase == SWAP.0 || phase == CLEANUP.0 {
                // From the swap on, the update goes through.
                match result {
                    Ok(UpdateOutcome::Updated(done)) => assert_eq!(done.to, version(NEXT)),
                    other => panic!("{context}: {other:?}"),
                }
                assert_eq!(state_names(&fixture.root), [RECEIPT_FILE], "{context}");
                reached.insert("updated");
                continue;
            }
            let failure = match result {
                Err(failure) => failure,
                Ok(outcome) => panic!("{context}: {outcome:?}"),
            };
            assert!(
                matches!(failure.error, UpdateError::Cancelled),
                "{context}: {failure:?}"
            );
            // Before the staging is made nothing is; from then on what was staged is taken back.
            let changes = if staged {
                Changes::RolledBack
            } else {
                Changes::None
            };
            assert_eq!(failure.changes, changes, "{context}");
            assert_eq!(as_it_is(&fixture.root), before, "{context}");
            reached.insert(if staged { "rolledBack" } else { "none" });
        }
        assert_eq!(reached, BTreeSet::from(["none", "rolledBack", "updated"]));
    }

    #[test]
    fn a_cleanup_that_stops_keeps_the_journal_for_the_next_start() {
        let (dir, fixture) = players_installation();
        let next = fixture.runtime.next(NEXT);
        let (_, mut source) = bundled_as(dir.path(), "next-launcher", &next);
        let fail = FailAt::new(FaultPoint::UpdateCleanup, 1);
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&fail, &answering),
        ));
        assert!(
            matches!(done.notices.as_slice(), [UpdateNotice::CleanupLeft { .. }]),
            "{:?}",
            done.notices
        );
        let RetiredCopy::Left { name } = &done.retired else {
            panic!("{:?}", done.retired);
        };
        assert!(name.starts_with("retired-launcher-1.0.0-beta-"), "{name}");
        let state = fixture.root.join(STATE_DIR);
        assert!(state.join(name).is_file());
        assert!(state.join(UPDATE_BACKUP).is_dir());
        assert!(stagings(&fixture.root).is_empty());
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("committed"));
        // The new release stands.
        let installed = InstallFolder::at(&fixture.root)
            .unwrap()
            .installed(&version(NEXT))
            .unwrap();
        assert_eq!(installed.build_info.version(), version(NEXT));

        // Another update waits for that cleanup.
        let (_, mut later) = bundled_as(dir.path(), "later-launcher", &next.next(LATER));
        let before = as_it_is(&fixture.root);
        let failure = failed(update_simply(
            &fixture,
            &mut later,
            LATER,
            &standing_in(&NoFaults, &answering),
        ));
        assert!(
            matches!(failure.error, UpdateError::CleanupPending),
            "{failure:?}"
        );
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn a_failure_past_the_receipt_is_still_an_update() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        // The pass after the receipt is written.
        let fail = FailAt::new(FaultPoint::UpdateCommit, 2);
        let done = updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&fail, &answering),
        ));
        assert!(
            matches!(done.notices.as_slice(), [UpdateNotice::CleanupLeft { .. }]),
            "{:?}",
            done.notices
        );
        assert!(
            matches!(done.retired, RetiredCopy::Left { .. }),
            "{:?}",
            done.retired
        );
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("swapping"));
        assert_eq!(
            receipt_at(&fixture.root),
            serde_json::to_value(&done.receipt).unwrap()
        );
        assert_eq!(
            fs::read(fixture.root.join("VERSION")).unwrap(),
            b"1.0.0-beta.1\n"
        );
    }

    #[test]
    fn every_move_is_listed_before_the_first() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let root = fixture.root.clone();
        let seen = Mutex::new(None);
        let look = || -> io::Result<()> {
            let path = root.join(STATE_DIR).join(JOURNAL_DIR).join(UPDATE_JOURNAL);
            let journal: Value = serde_json::from_slice(&fs::read(path)?).unwrap();
            let untouched = fs::read(root.join("VERSION"))? == b"1.0.0-beta\n";
            *seen.lock().unwrap() = Some((journal, untouched));
            Ok(())
        };
        // The pass after the journal moves to swapping, before the first move.
        let on = OnPass::new(FaultPoint::UpdateCommit, 1, &look);
        updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&on, &answering),
        ));
        let (journal, untouched) = seen.into_inner().unwrap().unwrap();
        assert!(untouched);
        assert_eq!(journal["phase"], json!("swapping"));
        assert_eq!(journal["backup"].as_array().unwrap().len(), 7);
        assert_eq!(journal["publish"].as_array().unwrap().len(), 7);
        assert_eq!(journal["madeFolders"], json!([]));
        assert_eq!(journal["launcher"], json!(launcher_copy()));
        let retired = journal["retired"].as_str().unwrap();
        assert!(
            retired.starts_with("retired-launcher-1.0.0-beta-"),
            "{retired}"
        );
    }

    #[cfg(windows)]
    #[test]
    fn the_system_is_kept_awake_while_updating() {
        use windows_sys::Win32::System::Power::{ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

        use crate::platform;

        /// A progress that reads the thread's requests as the staging begins.
        #[derive(Default)]
        struct Awake {
            state: Mutex<Option<u32>>,
        }

        impl Progress for Awake {
            fn phase(&self, phase: PhaseId) {
                if phase == super::STAGE {
                    *self.state.lock().unwrap() = Some(platform::execution_state());
                }
            }

            fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
        }

        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = platform::execution_state();
        let awake = Awake::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &awake,
        };
        updated(update_by(
            &fixture,
            &mut source,
            NEXT,
            &job,
            &standing_in(&NoFaults, &answering),
        ));
        assert_eq!(
            *awake.state.lock().unwrap(),
            Some(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
        );
        assert_eq!(platform::execution_state(), before);
    }

    /// The variable that makes a run of this test program update a folder from the system's
    /// default error mode, in `an_update_fails_critical_errors_rather_than_showing_them`. Unset, as
    /// in a run of the tests, that part does nothing.
    #[cfg(windows)]
    const ERROR_MODE_CHILD: &str = "GDL_UPDATE_ERROR_MODE_CHILD";

    /// The test that a child run of this program, started in the default error mode, runs alone.
    #[cfg(windows)]
    const UPDATING_TEST: &str = "update::tests::updating_from_the_default_error_mode";

    /// The update is made in a child started in the system's default error mode, which shows
    /// critical errors, as a launcher started from Explorer is.
    #[cfg(windows)]
    #[test]
    fn an_update_fails_critical_errors_rather_than_showing_them() {
        crate::default_error_mode::run_in_default_error_mode(UPDATING_TEST, ERROR_MODE_CHILD);
    }

    /// The child's part of `an_update_fails_critical_errors_rather_than_showing_them`.
    #[cfg(windows)]
    #[test]
    fn updating_from_the_default_error_mode() {
        use windows_sys::Win32::System::Diagnostics::Debug::SEM_FAILCRITICALERRORS;

        use crate::default_error_mode::{ERROR_MODE_CHECKED, started_in_default_error_mode};
        use crate::platform::error_mode;

        /// A progress that reads the process's error mode as the update's first phase begins,
        /// before the update looks at the folder.
        #[derive(Default)]
        struct Mode {
            at_phase: Mutex<Option<u32>>,
        }

        impl Progress for Mode {
            fn phase(&self, phase: PhaseId) {
                if phase == super::CHECK_INSTALLED {
                    *self.at_phase.lock().unwrap() = Some(error_mode());
                }
            }

            fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
        }

        let Some(started) = started_in_default_error_mode(ERROR_MODE_CHILD) else {
            return;
        };
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let mode = Mode::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &mode,
        };
        updated(update_by(
            &fixture,
            &mut source,
            NEXT,
            &job,
            &standing_in(&NoFaults, &answering),
        ));
        // Critical errors fail calls from before the update's first look on, and the rest of the
        // mode is as it was.
        let failing = started | SEM_FAILCRITICALERRORS;
        assert_eq!(*mode.at_phase.lock().unwrap(), Some(failing));
        assert_eq!(error_mode(), failing);
        // Removed before the exit, which runs no destructor.
        drop(source);
        dir.close().unwrap();
        std::process::exit(ERROR_MODE_CHECKED);
    }
}
