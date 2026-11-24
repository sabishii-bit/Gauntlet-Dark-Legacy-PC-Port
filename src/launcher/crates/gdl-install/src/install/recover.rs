//! Recovering a fresh install that a crash, a power cut or a kill stopped before it finished. The
//! next start finds the install's journal, or a state folder holding only what an install makes
//! before its first journal, and under the installation's lock finishes or takes back what is
//! there: a publish cut short is finished, every root put in place and the receipt written; a
//! staging, or a rollback cut short, is taken back as the job takes a failure back; once the
//! journal records the receipt in place, only the cleanup is left; and a state folder left before
//! the first journal is given up with its lock. Every removal counts a name already gone as
//! removed, so that a recovery a second crash cuts short is finished by the next.

use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{
    FilesError, Job, Journal, MAX_JSON_LEN, PAYLOAD_NAMES, PENDING_SUFFIX, PhaseId, RelPath,
    Staging, is_absent, is_staging_name, pending_path, read_json_bounded,
};
use gdl_release::{Platform, Version};
use serde_json::{Map, Value};

use super::disc_files::DISC_FILES;
use super::journal::{INSTALL_JOURNAL, InstallJournal, InstallPhase, JournalFile, remove_if_there};
use super::rollback::{Begun, Rollback, clean_up, is_there, stranded_roots};
use super::settings::InstallSettings;
use super::{FolderRecord, INSTALL_STAGING_PREFIX, is_install_root};
use crate::awake::keep_awake;
use crate::compat::{Access, JOURNAL_NAMES, JOURNAL_SCHEMA, abandoned_install, access, stamp_of};
use crate::folder::{
    FolderError, InstallFolder, InstallLock, JOURNAL_DIR, LockError, RECEIPT_FILE, STATE_DIR,
    names_in,
};
use crate::receipt::{MAX_RECEIPT_LEN, RECEIPT_SCHEMA, Receipt};

/// The phase in which the launcher recovers a fresh install that was stopped before it finished.
pub const RECOVER_INSTALL: PhaseId = PhaseId("recoverInstall");

/// What recovering a folder did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RecoveryOutcome {
    /// Nothing: no fresh install was left unfinished there.
    Nothing,
    /// The install was taken back: the folder is as it was before the install began, but for
    /// what `kept` names.
    RolledBack {
        /// What stays of the install's: a `saves` or `config` folder the publish put in place
        /// that the player has put files in since, and the state folder when it holds a name not
        /// the launcher's to remove.
        kept: Vec<String>,
    },
    /// The publish was finished: every root is in place, the receipt is written, and the
    /// installation stands.
    Finished {
        /// The receipt written, the one the install would have written.
        receipt: Box<Receipt>,
    },
    /// The installation stood already, and what was left of its install, or what a job left
    /// beside it, was cleaned up.
    CleanedUp,
    /// A state folder an install left before its first journal was given up with its lock. The
    /// folder itself is left, empty when the install made it: nothing says who made it.
    AbandonedRemoved,
}

/// Why a fresh install could not be recovered, as technical detail for logs. What a player reads
/// is chosen from it elsewhere. Whatever stopped the recovery, the journal is left for the next.
#[derive(Debug, thiserror::Error)]
pub enum RecoveryError {
    /// The installation's lock was not taken, or would be refused: a link on the folder's path is
    /// [`LockError::Links`] before the lock is tried. [`LockError::Busy`], another job holding it,
    /// is a reason to wait rather than a failure.
    #[error(transparent)]
    Lock(LockError),
    /// A later launcher's receipt or journal stands in the installation.
    #[error("A later launcher's state stands in this installation; this launcher changes nothing")]
    NewerLauncher,
    /// The install's journal, or another job's, cannot be read as one.
    #[error("Unreadable install journal: {detail}")]
    JournalUnreadable {
        /// Why, with the journal's path.
        detail: Box<str>,
    },
    /// What the journal records does not match what the folder holds.
    #[error("The interrupted install cannot be finished or taken back: {detail}")]
    Inconsistent {
        /// What does not match.
        detail: Box<str>,
    },
    /// A file operation in the folder failed.
    #[error(transparent)]
    Files(FilesError),
    /// The system refused to look at `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// What was looked at.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
}

impl RecoveryError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the lock's rule,
    /// `newerLauncher`, `journalUnreadable`, `inconsistent`, the file operation's rule or `io`.
    pub fn rule(&self) -> &'static str {
        match self {
            RecoveryError::Lock(error) => error.rule(),
            RecoveryError::NewerLauncher => "newerLauncher",
            RecoveryError::JournalUnreadable { .. } => "journalUnreadable",
            RecoveryError::Inconsistent { .. } => "inconsistent",
            RecoveryError::Files(error) => error.rule(),
            RecoveryError::Io { .. } => "io",
        }
    }

    /// The refusal of a journal whose record `detail` tells why it cannot be read.
    fn unreadable(detail: impl std::fmt::Display) -> RecoveryError {
        RecoveryError::JournalUnreadable {
            detail: detail.to_string().into(),
        }
    }

    /// The refusal of what the folder holds against its journal, as `detail` tells it.
    fn inconsistent(detail: impl std::fmt::Display) -> RecoveryError {
        RecoveryError::Inconsistent {
            detail: detail.to_string().into(),
        }
    }
}

/// Recovers the fresh install that was stopped in the folder at `root`, for the launcher of
/// version `running`, telling `record` of the folder as [`install_game`](super::install_game)
/// tells it, in the phase [`RECOVER_INSTALL`], which `job`'s progress is told. The recovery is
/// never cancelled, and the system is kept out of idle sleep while it runs.
///
/// Nothing is done, and the lock is not taken, when the folder holds no state folder, or holds
/// nothing of a fresh install's to finish or remove. Otherwise, under the installation's lock, of
/// which another job's hold is [`RecoveryError::Lock`]: a receipt or a journal a later launcher
/// stamped refuses the recovery, as does a journal that cannot be read; then
///
/// - without the install's journal, the install's stagings and its pending files go, and the list
///   of the disc's files when no receipt stands beside it; a state folder that then holds only
///   what an install makes before its first journal is given up with its lock, and the record
///   forgets the folder;
/// - a journal of a staging install, or of a rollback, takes the install back as the job takes
///   a failure back;
/// - a journal of a publish puts every root left in the staging in place and writes the receipt
///   the journal carries, then cleans up; a root whose target something else took meanwhile
///   takes the install back instead, unless the receipt stands, which nothing takes back;
/// - a journal of a committed install cleans up.
///
/// Once the installation stands, the record is told of it and the shell of its executable. What
/// the journal records and the folder holds must agree; when they do not, nothing more is done
/// and [`RecoveryError::Inconsistent`] says why.
pub fn recover_install(
    root: &Path,
    running: &Version,
    record: &mut dyn FolderRecord,
    job: &Job<'_>,
) -> Result<RecoveryOutcome, RecoveryError> {
    recover_install_with(root, running, record, job, &InstallSettings::SHIPPED)
}

/// Recovers a fresh install as [`recover_install`] does, asking the world beyond the folder
/// through `settings`.
pub(crate) fn recover_install_with(
    root: &Path,
    running: &Version,
    record: &mut dyn FolderRecord,
    job: &Job<'_>,
    settings: &InstallSettings<'_>,
) -> Result<RecoveryOutcome, RecoveryError> {
    let _awake = keep_awake();
    job.progress.phase(RECOVER_INSTALL);
    let folder = match InstallFolder::at(root) {
        Ok(folder) => folder,
        // No folder, or a file, holds no state folder.
        Err(FolderError::Absent { .. } | FolderError::NotDirectory { .. }) => {
            return Ok(RecoveryOutcome::Nothing);
        }
        // Refused as taking the lock refuses a link on the folder's path, so that the same link
        // reports the same wherever it is met.
        Err(FolderError::Links { path }) => {
            return Err(RecoveryError::Lock(LockError::Links { path }));
        }
        Err(FolderError::Io { path, source }) => return Err(RecoveryError::Io { path, source }),
    };
    let state = folder.state_dir();
    // Looked at first without the lock, so that a folder with nothing to recover is left as it is,
    // its lock file included, and a job that holds the lock does not keep it waiting.
    if !Found::at(&state)?.is_some_and(|found| found.calls_for_work()) {
        return Ok(RecoveryOutcome::Nothing);
    }
    let lock = match folder.lock() {
        Ok(lock) => lock,
        // Given up since it was looked at.
        Err(LockError::NoStateFolder { .. }) => return Ok(RecoveryOutcome::Nothing),
        Err(error) => return Err(RecoveryError::Lock(error)),
    };
    // Looked at again under the lock, which no job can change from now on.
    let Some(found) = Found::at(&state)?.filter(Found::calls_for_work) else {
        return Ok(RecoveryOutcome::Nothing);
    };
    let journal = judge_stamps(&folder, &found, running)?;
    let recovery = Recovery {
        folder: &folder,
        state: &state,
        settings,
    };
    match journal {
        None => recovery.without_journal(lock, &found, record),
        Some(journal) => recovery.with_journal(lock, &found, journal, record),
    }
}

/// What a state folder holds, listed without opening any entry.
struct Found {
    /// The names directly in it.
    names: Vec<OsString>,
    /// The names in its journal folder; `None` when it has none.
    journals: Option<Vec<OsString>>,
}

impl Found {
    /// Lists the state folder `state` and its journal folder; `None` when no state folder is
    /// there. A link at either is refused, and a journal folder that is no folder is a journal
    /// this launcher cannot read.
    fn at(state: &Path) -> Result<Option<Found>, RecoveryError> {
        let Some(names) = listing(state)? else {
            return Ok(None);
        };
        let journals = if names.iter().any(|name| name == JOURNAL_DIR) {
            let folder = state.join(JOURNAL_DIR);
            match listing(&folder)? {
                Some(journals) => Some(journals),
                None if is_there(&folder).map_err(RecoveryError::Files)? => {
                    let not_folder = io::Error::from(io::ErrorKind::NotADirectory);
                    return Err(RecoveryError::unreadable(format_args!(
                        "{}: {not_folder}",
                        folder.display()
                    )));
                }
                None => None,
            }
        } else {
            None
        };
        Ok(Some(Found { names, journals }))
    }

    /// Whether the state folder holds the install's journal.
    fn has_install_journal(&self) -> bool {
        self.journals
            .iter()
            .flatten()
            .any(|name| name == INSTALL_JOURNAL)
    }

    /// Whether the state folder holds a name of the receipt's.
    fn has_receipt(&self) -> bool {
        self.names.iter().any(|name| name == RECEIPT_FILE)
    }

    /// Whether a recovery has anything to finish or remove: the install's journal, a staging or
    /// a pending file of the install's that no journal names, the list of the disc's files with no
    /// receipt beside it, or a state folder an install left before its first journal.
    fn calls_for_work(&self) -> bool {
        let stale = self.names.iter().any(|name| {
            name.to_str().is_some_and(|name| {
                is_staging_name(INSTALL_STAGING_PREFIX, name)
                    || name
                        .strip_suffix(PENDING_SUFFIX)
                        .is_some_and(|file| file == DISC_FILES || file == RECEIPT_FILE)
            })
        });
        let stale_journal = self.journals.iter().flatten().any(|name| {
            name.to_str()
                .and_then(|name| name.strip_suffix(PENDING_SUFFIX))
                .is_some_and(|file| file == INSTALL_JOURNAL)
        });
        let unclaimed_list =
            self.names.iter().any(|name| name == DISC_FILES) && !self.has_receipt();
        self.has_install_journal()
            || stale
            || stale_journal
            || unclaimed_list
            || abandoned_install(&self.names, self.journals.as_deref())
    }
}

/// The names the folder of the launcher's state at `path` holds; `None` when nothing, or no
/// folder, is there. A link there is refused.
fn listing(path: &Path) -> Result<Option<Vec<OsString>>, RecoveryError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() => {
            return Err(RecoveryError::Files(FilesError::Link {
                path: path.to_path_buf(),
            }));
        }
        Ok(metadata) if !metadata.is_dir() => return Ok(None),
        Ok(_) => {}
        Err(error) if is_absent(&error) => return Ok(None),
        Err(source) => {
            return Err(RecoveryError::Io {
                path: path.to_path_buf(),
                source,
            });
        }
    }
    match names_in(path) {
        Ok(names) => Ok(Some(names)),
        Err(error) if is_absent(&error) => Ok(None),
        Err(source) => Err(RecoveryError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// Judges the stamps of the journals in the state folder of the installation's folder `folder`
/// that `found` lists, and of its receipt, for the launcher of version `running`, and reads the
/// install's journal when there is one. A journal of a kind this launcher reads that cannot be
/// read refuses the recovery first; then a journal of a kind it does not know, or one a later
/// launcher stamped, and a receipt a later launcher stamped. An install journal that does not
/// hold what an install's does, or names what an install never makes, cannot be read either.
fn judge_stamps(
    folder: &InstallFolder,
    found: &Found,
    running: &Version,
) -> Result<Option<InstallJournal>, RecoveryError> {
    let state = folder.state_dir();
    let root = folder.root();
    let folder = state.join(JOURNAL_DIR);
    let mut refusal = None;
    let mut later = false;
    let mut install = None;
    let journals = found.journals.iter().flatten();
    for name in
        journals.filter(|name| !name.as_encoded_bytes().ends_with(PENDING_SUFFIX.as_bytes()))
    {
        let Some(name) = name.to_str().filter(|name| JOURNAL_NAMES.contains(name)) else {
            later = true;
            continue;
        };
        let path = folder.join(name);
        let object = match read_json_bounded::<Map<String, Value>>(&path, MAX_JSON_LEN) {
            Ok(object) => object,
            Err(error) => {
                if let Some(refused) = unread(error, &path) {
                    refusal.get_or_insert(refused);
                }
                continue;
            }
        };
        match stamp_of(&object) {
            Ok(stamp) if access(&stamp, JOURNAL_SCHEMA, running) == Access::ReadOnly => {
                later = true;
            }
            Ok(_) if name == INSTALL_JOURNAL => install = Some((path, object)),
            Ok(_) => {}
            Err(error) => {
                refusal.get_or_insert(RecoveryError::unreadable(format_args!(
                    "{}: {error}",
                    path.display()
                )));
            }
        }
    }
    if let Some(refused) = refusal {
        return Err(refused);
    }
    if later || later_receipt(&state, running) {
        return Err(RecoveryError::NewerLauncher);
    }
    install
        .map(|(path, object)| install_journal(&path, root, object))
        .transpose()
}

/// Whether the receipt in the state folder `state` carries a stamp that leaves the installation
/// closed to the launcher of version `running`. A receipt that cannot be read, or carries no
/// stamp, closes nothing: the recovery never reads it for anything else.
fn later_receipt(state: &Path, running: &Version) -> bool {
    let receipt = state.join(RECEIPT_FILE);
    let Ok(object) = read_json_bounded::<Map<String, Value>>(&receipt, MAX_RECEIPT_LEN) else {
        return false;
    };
    stamp_of(&object).is_ok_and(|stamp| access(&stamp, RECEIPT_SCHEMA, running) == Access::ReadOnly)
}

/// What a journal at `path` that could not be read for `error` is to the recovery: a refusal, or
/// `None` when the journal is gone. A link is refused as one, something there that is no regular
/// file and what holds no stamped JSON object are journals this launcher cannot read, and a
/// refusal to read a file that is there is the system's.
fn unread(error: FilesError, path: &Path) -> Option<RecoveryError> {
    match error {
        error @ (FilesError::TooLarge { .. } | FilesError::Json { .. }) => {
            Some(RecoveryError::unreadable(error))
        }
        error @ FilesError::Io { .. } => match fs::symlink_metadata(path) {
            Err(looked) if is_absent(&looked) => None,
            Ok(metadata) if !metadata.is_file() => Some(RecoveryError::unreadable(error)),
            _ => Some(RecoveryError::Files(error)),
        },
        error => Some(RecoveryError::Files(error)),
    }
}

/// The install's journal at `path`, in the installation's folder `folder`, whose stamp lets this
/// launcher change it, read from `object`: it must hold what an install's journal holds, name a
/// staging by an install's staging name, count no more folders made than lie on `folder`'s path
/// below its top, which no install makes, and list as roots, each once, names an install puts at
/// a folder's top, with every root it records in place among them. So a journal a hand or a fault
/// changed never has a recovery move or remove anything an install never makes.
fn install_journal(
    path: &Path,
    folder: &Path,
    object: Map<String, Value>,
) -> Result<InstallJournal, RecoveryError> {
    let refused =
        |what: String| RecoveryError::unreadable(format_args!("{}: {what}", path.display()));
    let journal: InstallJournal = serde_json::from_value(Value::Object(object))
        .map_err(|error| refused(error.to_string()))?;
    if !is_staging_name(INSTALL_STAGING_PREFIX, &journal.staging) {
        return Err(refused(format!(
            "its staging {:?} is no install's staging",
            journal.staging
        )));
    }
    let most_made = folder.ancestors().count().saturating_sub(1);
    if !usize::try_from(journal.made_folders).is_ok_and(|made| made <= most_made) {
        return Err(refused(format!(
            "it counts {} folders made, more than lie on the folder's path",
            journal.made_folders
        )));
    }
    for (at, root) in journal.roots.iter().enumerate() {
        let single = RelPath::parse(root, &PAYLOAD_NAMES).is_ok_and(|name| name.depth() == 1);
        if !single || !is_install_root(root) || journal.roots[..at].contains(root) {
            return Err(refused(format!(
                "its root {root:?} is no root an install puts in place"
            )));
        }
    }
    if let Some(root) = journal
        .published
        .iter()
        .find(|root| !journal.roots.contains(root))
    {
        return Err(refused(format!(
            "it records {root:?} in place, which is none of its roots"
        )));
    }
    Ok(journal)
}

/// A recovery under way in an installation's folder, its lock held.
struct Recovery<'r> {
    /// The installation's folder.
    folder: &'r InstallFolder,
    /// Its state folder.
    state: &'r Path,
    /// What the recovery asks of the world beyond the folder.
    settings: &'r InstallSettings<'r>,
}

impl<'r> Recovery<'r> {
    /// Recovers a state folder that holds no install journal: removes the install's stagings and
    /// pending files, and the list of the disc's files when no receipt stands beside it, then gives
    /// the state folder up under `lock` when it holds only what an install makes before its first
    /// journal, the record told to forget the folder.
    fn without_journal(
        &self,
        lock: InstallLock,
        found: &Found,
        record: &mut dyn FolderRecord,
    ) -> Result<RecoveryOutcome, RecoveryError> {
        let state = self.state;
        let mut went = remove_stagings(state, &found.names, None)?;
        let mut stale = vec![
            pending_path(&state.join(JOURNAL_DIR).join(INSTALL_JOURNAL)),
            pending_path(&state.join(DISC_FILES)),
            pending_path(&state.join(RECEIPT_FILE)),
        ];
        if !found.has_receipt() {
            stale.push(state.join(DISC_FILES));
        }
        for file in stale {
            went |= remove_if_there(&file).map_err(RecoveryError::Files)?;
        }
        let left = Found::at(state)?;
        let abandoned =
            left.is_some_and(|left| abandoned_install(&left.names, left.journals.as_deref()));
        if abandoned && lock.give_up().map_err(RecoveryError::Files)? {
            let _ = record.forget(self.folder.root());
            return Ok(RecoveryOutcome::AbandonedRemoved);
        }
        Ok(if went {
            RecoveryOutcome::CleanedUp
        } else {
            RecoveryOutcome::Nothing
        })
    }

    /// Recovers the install `journal` records, under `lock`: the stagings it does not name go
    /// first, then the phase it records decides.
    fn with_journal(
        &self,
        lock: InstallLock,
        found: &Found,
        journal: InstallJournal,
        record: &mut dyn FolderRecord,
    ) -> Result<RecoveryOutcome, RecoveryError> {
        let state = self.state;
        let faults = self.settings.faults;
        let journal = JournalFile::resume(&state.join(JOURNAL_DIR), journal, faults);
        remove_stagings(state, &found.names, Some(journal.staging_name()))?;
        let staging = reopen_staging(state, journal.staging_name())?;
        let receipted = is_there(&state.join(RECEIPT_FILE)).map_err(RecoveryError::Files)?;
        match journal.phase() {
            InstallPhase::Committed => {
                remove_state_pending(state)?;
                clean_up(staging, &journal, faults).map_err(RecoveryError::Files)?;
                self.tell_installed(journal.receipt(), record);
                drop(lock);
                Ok(RecoveryOutcome::CleanedUp)
            }
            InstallPhase::Publishing => self.finish(lock, staging, journal, receipted, record),
            InstallPhase::Staging | InstallPhase::RollingBack if receipted => {
                Err(RecoveryError::inconsistent(
                    "the journal says the install is taken back, beside a receipt that stands",
                ))
            }
            InstallPhase::Staging => self.roll_back(lock, staging, journal, record),
            InstallPhase::RollingBack => {
                if staging.is_none() {
                    let stranded = stranded_roots(self.folder.root(), journal.roots())
                        .map_err(RecoveryError::Files)?;
                    if let Some(root) = stranded.first() {
                        return Err(RecoveryError::inconsistent(format_args!(
                            "{root} is in place, and the staging to take it back into is gone"
                        )));
                    }
                }
                self.roll_back(lock, staging, journal, record)
            }
        }
    }

    /// Finishes the publish `journal` records: each root it lists that is not recorded in place
    /// yet is recorded when it moved, or put in place from `staging` and recorded; then the receipt
    /// the journal carries is written, the journal committed and the install cleaned up. A root in
    /// the staging whose target something else took meanwhile takes the install back instead,
    /// unless `receipted`, the receipt standing already, which nothing takes back. Nothing is
    /// changed when a root is in neither place, or one recorded in place is not there.
    fn finish(
        &self,
        lock: InstallLock,
        staging: Option<Staging>,
        mut journal: JournalFile<'r>,
        receipted: bool,
        record: &mut dyn FolderRecord,
    ) -> Result<RecoveryOutcome, RecoveryError> {
        let root = self.folder.root();
        let mut moved = Vec::new();
        let mut staged = Vec::new();
        let mut taken = None;
        for name in journal.roots() {
            let in_place = is_there(&root.join(name)).map_err(RecoveryError::Files)?;
            if journal.published_roots().contains(name) {
                if !in_place {
                    return Err(RecoveryError::inconsistent(format_args!(
                        "{name} is recorded in place and is not there"
                    )));
                }
                continue;
            }
            let in_staging = match &staging {
                Some(staging) => {
                    is_there(&staging.path().join(name)).map_err(RecoveryError::Files)?
                }
                None => false,
            };
            match (in_place, in_staging) {
                (true, false) => moved.push(root_name(name)),
                (false, true) => staged.push(root_name(name)),
                (true, true) => {
                    taken.get_or_insert(name.clone());
                }
                (false, false) => {
                    return Err(RecoveryError::inconsistent(format_args!(
                        "{name} is neither in place nor in the staging"
                    )));
                }
            }
        }
        if let Some(name) = taken {
            if receipted {
                return Err(RecoveryError::inconsistent(format_args!(
                    "something else took {name}'s place beside a receipt that stands"
                )));
            }
            return self.roll_back(lock, staging, journal, record);
        }
        let Some(receipt) = journal.receipt().cloned() else {
            return Err(RecoveryError::inconsistent(
                "the journal of the publish carries no receipt",
            ));
        };
        remove_state_pending(self.state)?;
        for name in &moved {
            journal.published(name).map_err(RecoveryError::Files)?;
        }
        // A root is staged only while the staging is there.
        let published = match &staging {
            Some(staging) if !staged.is_empty() => staging.publish_new(&staged, root, &mut journal),
            _ => Ok(()),
        };
        match published {
            Ok(()) => {}
            Err(FilesError::Exists { .. }) if !receipted => {
                return self.roll_back(lock, staging, journal, record);
            }
            Err(error) => return Err(RecoveryError::Files(error)),
        }
        self.write_receipt(&receipt)?;
        journal.committed().map_err(RecoveryError::Files)?;
        clean_up(staging, &journal, self.settings.faults).map_err(RecoveryError::Files)?;
        self.tell_installed(Some(&receipt), record);
        drop(lock);
        Ok(RecoveryOutcome::Finished {
            receipt: Box::new(receipt),
        })
    }

    /// Takes the install `journal` records back under `lock`, as the job takes a failure back,
    /// with the staging when it is still there.
    fn roll_back(
        &self,
        lock: InstallLock,
        staging: Option<Staging>,
        journal: JournalFile<'_>,
        record: &mut dyn FolderRecord,
    ) -> Result<RecoveryOutcome, RecoveryError> {
        let rollback = Rollback {
            folder: self.folder.root(),
            made_folders: journal.made_folders(),
            lock,
            begun: Some(Begun {
                staging,
                journal: Some(journal),
            }),
            faults: self.settings.faults,
        };
        let taken = rollback.run(record).map_err(RecoveryError::Files)?;
        let mut kept = taken.kept;
        if !taken.given_up {
            kept.push(STATE_DIR.to_owned());
        }
        Ok(RecoveryOutcome::RolledBack { kept })
    }

    /// Writes `receipt` as the installation's, and makes sure it is in place: a write can fail
    /// after its rename, with the receipt there all the same.
    fn write_receipt(&self, receipt: &Receipt) -> Result<(), RecoveryError> {
        let written = receipt.write(self.folder);
        let path = self.state.join(RECEIPT_FILE);
        if fs::symlink_metadata(&path).is_ok_and(|metadata| metadata.is_file()) {
            return Ok(());
        }
        Err(match written {
            Err(error) => RecoveryError::Files(error),
            Ok(()) => RecoveryError::Io {
                path,
                source: io::Error::from(io::ErrorKind::NotFound),
            },
        })
    }

    /// Tells the record that the installation stands, a refusal passed over, as the start that
    /// follows records a known installation again; and tells the shell of the executable of the
    /// platform `receipt` names, or this program's.
    fn tell_installed(&self, receipt: Option<&Receipt>, record: &mut dyn FolderRecord) {
        let root = self.folder.root();
        let _ = record.installed(root);
        let platform = receipt
            .and_then(|receipt| Platform::parse(&receipt.platform))
            .or_else(Platform::of_this_build);
        if let Some(platform) = platform {
            (self.settings.notify_shell)(&root.join(platform.executable()));
        }
    }
}

/// The root `name`, which the journal's reading held to a single name of the payload's rules.
fn root_name(name: &str) -> RelPath {
    RelPath::parse(name, &PAYLOAD_NAMES).expect("a journal's roots were read as single names")
}

/// The install's staging `name` in the state folder `state`, kept; `None` when nothing is there,
/// as a rollback or a cleanup a crash cut short leaves it.
fn reopen_staging(state: &Path, name: &str) -> Result<Option<Staging>, RecoveryError> {
    match Staging::reopen(state, INSTALL_STAGING_PREFIX, name) {
        Ok(staging) => Ok(Some(staging)),
        Err(FilesError::Io { source, .. }) if source.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(RecoveryError::Files(error)),
    }
}

/// Removes every staging of an install's among `names`, the names in the state folder `state`,
/// but the one named `kept`, and says whether any went.
fn remove_stagings(
    state: &Path,
    names: &[OsString],
    kept: Option<&str>,
) -> Result<bool, RecoveryError> {
    let mut went = false;
    for name in names.iter().filter_map(|name| name.to_str()) {
        if !is_staging_name(INSTALL_STAGING_PREFIX, name) || kept == Some(name) {
            continue;
        }
        let removed =
            Staging::reopen(state, INSTALL_STAGING_PREFIX, name).and_then(Staging::remove);
        match removed {
            Ok(()) => went = true,
            Err(FilesError::Io { source, .. }) if is_absent(&source) => {}
            Err(error) => return Err(RecoveryError::Files(error)),
        }
    }
    Ok(went)
}

/// Removes the pending files that interrupted writes of the list of the disc's files and of the
/// receipt left in the state folder `state`, each counted removed when it is already gone.
fn remove_state_pending(state: &Path) -> Result<(), RecoveryError> {
    for file in [DISC_FILES, RECEIPT_FILE] {
        remove_if_there(&pending_path(&state.join(file))).map_err(RecoveryError::Files)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::collections::BTreeMap;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};

    use gdl_files::{Cancel, FilesError, Job, NoProgress, retry_locked};
    use gdl_fixtures::{folder_names, link_folder, pretty_json, sha256_hex, snapshot};
    use gdl_release::{Platform, Version};
    use serde_json::{Map, Value, json};
    use tempfile::TempDir;

    use super::{RecoveryError, RecoveryOutcome, recover_install_with};
    use crate::folder::{
        InstallFolder, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE, STATE_DIR,
    };
    use crate::install::disc_files::DISC_FILES;
    use crate::install::journal::INSTALL_JOURNAL;
    use crate::install::settings::{InstallSettings, NoFaults};
    use crate::install::tests::{
        AsItWas, Heard, USER_BYTES, USER_FILE, answering, destination_in, heard_of, standing_in,
        tree,
    };
    use crate::receipt::{InstalledLauncher, Receipt, installed_launcher_name};

    /// The launcher's version, which the tests recover as and the hand-written state carries.
    const VERSION: &str = "1.0.0-beta";

    /// The staging every hand-written journal names.
    const STAGING: &str = "install-AbC123xy";

    /// Another staging of an install's, which no journal names.
    const OTHER_STAGING: &str = "install-XyZ987ab";

    /// The roots the hand-built install puts in place, in byte order: a folder holding a file, a
    /// file, and the two folders the game keeps the player's files in.
    const ROOTS: [&str; 4] = ["Gauntlet", "VERSION", "config", "saves"];

    /// What the hand-built install's one disc file holds.
    const WAR: &[u8] = b"war";

    /// What its `VERSION` file holds.
    const VERSION_TEXT: &[u8] = b"1.0.0-beta\n";

    /// What its list of the disc's files holds, which no recovery reads.
    const DISC_LIST: &[u8] = b"{}\n";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The platform this test program was built for.
    fn this_platform() -> Platform {
        Platform::of_this_build().expect("a release is built for the system the tests run on")
    }

    /// The receipt the hand-written journals of a publish carry, for this platform.
    fn receipt() -> Receipt {
        let platform = this_platform();
        Receipt {
            schema: 1,
            min_launcher: VERSION.to_owned(),
            version: VERSION.to_owned(),
            commit: "test-commit".to_owned(),
            platform: platform.as_str().to_owned(),
            disc: "GUNE5D".to_owned(),
            disc_revision: 1,
            build_info_sha256: "a".repeat(64),
            installed_digests: BTreeMap::new(),
            disc_icon: None,
            launcher: InstalledLauncher {
                name: installed_launcher_name(platform).to_owned(),
                sha256: "b".repeat(64),
                other: Map::new(),
            },
            other: Map::new(),
        }
    }

    /// The receipt's bytes as the launcher writes a state file.
    fn receipt_bytes() -> Vec<u8> {
        let mut json = serde_json::to_vec_pretty(&receipt()).unwrap();
        json.push(b'\n');
        json
    }

    /// The outcome of a rollback that kept what `kept` names.
    fn rolled_back(kept: &[&str]) -> RecoveryOutcome {
        RecoveryOutcome::RolledBack {
            kept: kept.iter().map(|&name| name.to_owned()).collect(),
        }
    }

    /// A fresh install's folder as a crash left it, built by hand: its folder, made under a folder
    /// of the test's own or holding a file of the player's, with the state folder, the lock, the
    /// journal folder, the staging holding every root and the list of the disc's files.
    struct Left {
        /// The folder of the test's own.
        top: TempDir,
        /// The installation's folder.
        root: PathBuf,
        /// Whether it held a file of the player's before the install.
        holds_a_file: bool,
        /// What was there before the install.
        as_it_was: AsItWas,
    }

    impl Left {
        /// Everything staged, no journal written yet.
        fn staged(holds_a_file: bool) -> Left {
            let top = tempfile::tempdir().unwrap();
            let root = destination_in(top.path(), holds_a_file);
            let as_it_was = AsItWas::of(top.path(), &root);
            let state = root.join(STATE_DIR);
            fs::create_dir_all(state.join(JOURNAL_DIR)).unwrap();
            fs::write(state.join(LOCK_FILE), b"").unwrap();
            let staging = state.join(STAGING);
            fs::create_dir_all(staging.join("Gauntlet").join("PDATA")).unwrap();
            fs::write(staging.join("Gauntlet").join("PDATA").join("WAR.WAD"), WAR).unwrap();
            fs::write(staging.join("VERSION"), VERSION_TEXT).unwrap();
            fs::create_dir(staging.join("config")).unwrap();
            fs::create_dir(staging.join("saves")).unwrap();
            fs::write(state.join(DISC_FILES), DISC_LIST).unwrap();
            Left {
                top,
                root,
                holds_a_file,
                as_it_was,
            }
        }

        /// The folder as an install leaves it part-way through `phase`: in staging everything
        /// staged; publishing or rolling back, the first root in place and recorded; committed,
        /// every root in place and the receipt written.
        fn in_phase(phase: &str, holds_a_file: bool) -> Left {
            let left = Left::staged(holds_a_file);
            match phase {
                "staging" => left.journal(phase, &[]),
                "publishing" | "rollingBack" => {
                    left.put_in_place(ROOTS[0]);
                    left.journal(phase, &ROOTS[..1]);
                }
                "committed" => {
                    for root in ROOTS {
                        left.put_in_place(root);
                    }
                    left.write_receipt();
                    left.journal(phase, &ROOTS);
                }
                other => panic!("{other} is no install's phase"),
            }
            left
        }

        fn state(&self) -> PathBuf {
            self.root.join(STATE_DIR)
        }

        fn staging(&self) -> PathBuf {
            self.state().join(STAGING)
        }

        fn journals(&self) -> PathBuf {
            self.state().join(JOURNAL_DIR)
        }

        /// The journal in `phase` with `published` recorded in place: before the publish no
        /// roots, from it on every root and the receipt.
        fn journal_value(&self, phase: &str, published: &[&str]) -> Value {
            let roots: &[&str] = if phase == "staging" { &[] } else { &ROOTS };
            let mut journal = json!({
                "schema": 1,
                "minLauncher": VERSION,
                "phase": phase,
                "madeFolders": if self.holds_a_file { 0 } else { 2 },
                "staging": STAGING,
                "roots": roots,
                "published": published,
            });
            if phase != "staging" {
                journal["receipt"] = serde_json::to_value(receipt()).unwrap();
            }
            journal
        }

        /// Writes the journal of [`journal_value`](Left::journal_value).
        fn journal(&self, phase: &str, published: &[&str]) {
            self.write_journal(&self.journal_value(phase, published));
        }

        fn write_journal(&self, journal: &Value) {
            fs::write(self.journals().join(INSTALL_JOURNAL), pretty_json(journal)).unwrap();
        }

        /// Moves the root `name` from the staging into place.
        fn put_in_place(&self, name: &str) {
            retry_locked(|| fs::rename(self.staging().join(name), self.root.join(name))).unwrap();
        }

        fn write_receipt(&self) {
            fs::write(self.state().join(RECEIPT_FILE), receipt_bytes()).unwrap();
        }

        fn remove_staging(&self) {
            retry_locked(|| fs::remove_dir_all(self.staging())).unwrap();
        }

        fn recover(&self) -> Recovered {
            recover_at(&self.root)
        }

        /// What the folder holds once the install stands, by [`tree`]'s paths.
        fn installed_tree(&self) -> BTreeMap<String, Option<String>> {
            let file = |bytes: &[u8]| Some(sha256_hex(bytes));
            let mut entries = vec![
                ("Gauntlet", None),
                ("Gauntlet/PDATA", None),
                ("Gauntlet/PDATA/WAR.WAD", file(WAR)),
                ("VERSION", file(VERSION_TEXT)),
                ("config", None),
                ("saves", None),
                (".gdl-launcher", None),
                (".gdl-launcher/lock", file(b"")),
                (".gdl-launcher/journal", None),
                (".gdl-launcher/disc-files.json", file(DISC_LIST)),
                (".gdl-launcher/receipt.json", file(&receipt_bytes())),
            ];
            if self.holds_a_file {
                entries.push((USER_FILE, file(USER_BYTES)));
            }
            entries
                .into_iter()
                .map(|(path, digest)| (path.to_owned(), digest))
                .collect()
        }

        #[track_caller]
        fn assert_installed(&self, context: &str) {
            assert_eq!(tree(&self.root), self.installed_tree(), "{context}");
        }

        /// The names the folder holds once everything of the install's but `kept` went.
        fn holding(&self, kept: &[&str]) -> Vec<String> {
            let mut names: Vec<String> = kept.iter().map(|&name| name.to_owned()).collect();
            if self.holds_a_file {
                names.push(USER_FILE.to_owned());
            }
            names.sort();
            names
        }

        /// The shell told of the installed executable, once.
        fn shell_told(&self) -> Vec<PathBuf> {
            vec![self.root.join(this_platform().executable())]
        }
    }

    /// A case of a test: what it is, and what it makes of a folder left with everything staged.
    type Case = (&'static str, fn(&Left));

    /// What a recovery gave, what the record of installations heard and what the shell was told.
    struct Recovered {
        outcome: Result<RecoveryOutcome, RecoveryError>,
        heard: Vec<(&'static str, PathBuf)>,
        shell: Vec<PathBuf>,
    }

    /// Recovers the folder at `root` as the first launcher does, with the install tests' stand-ins
    /// and a record and a shell that keep what they are told.
    fn recover_at(root: &Path) -> Recovered {
        let running = Version::parse(VERSION).unwrap();
        let mut record = Heard::default();
        let shell = RefCell::new(Vec::new());
        let notify = |path: &Path| shell.borrow_mut().push(path.to_path_buf());
        let settings = InstallSettings {
            notify_shell: &notify,
            ..standing_in(&NoFaults, &answering)
        };
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let outcome = recover_install_with(root, &running, &mut record, &job, &settings);
        Recovered {
            outcome,
            heard: record.calls,
            shell: shell.into_inner(),
        }
    }

    /// Checks that a recovery of `left` refuses with `rule` and leaves the folder of the test's
    /// own exactly as it was, telling nobody anything.
    #[track_caller]
    fn assert_left_alone(left: &Left, rule: &str, context: &str) -> RecoveryError {
        let before = snapshot(left.top.path()).unwrap();
        let recovered = left.recover();
        let error = recovered.outcome.unwrap_err();
        assert_eq!(error.rule(), rule, "{context}: {error:?}");
        assert_eq!(snapshot(left.top.path()).unwrap(), before, "{context}");
        assert!(recovered.heard.is_empty(), "{context}");
        assert!(recovered.shell.is_empty(), "{context}");
        error
    }

    #[test]
    fn each_phase_recovers_from_a_journal_written_by_hand() {
        for holds_a_file in [false, true] {
            let context = |what: &str| format!("{what}, the folder holding a file: {holds_a_file}");

            // Staging: everything staged is taken back, the folders the install made with it.
            let left = Left::in_phase("staging", holds_a_file);
            let recovered = left.recover();
            assert_eq!(recovered.outcome.unwrap(), rolled_back(&[]));
            left.as_it_was.holds(&context("staging"));
            assert_eq!(recovered.heard, heard_of(&left.root, &["forget"]));
            assert!(recovered.shell.is_empty());

            // Publishing: a root recorded in place, one in place whose move was not recorded, and
            // two still staged. Every root is put in place and the receipt written.
            let left = Left::staged(holds_a_file);
            left.put_in_place("Gauntlet");
            left.put_in_place("VERSION");
            left.journal("publishing", &["Gauntlet"]);
            let recovered = left.recover();
            assert_eq!(
                recovered.outcome.unwrap(),
                RecoveryOutcome::Finished {
                    receipt: Box::new(receipt())
                }
            );
            left.assert_installed(&context("publishing"));
            assert_eq!(recovered.heard, heard_of(&left.root, &["installed"]));
            assert_eq!(recovered.shell, left.shell_told());

            // Publishing, something else in a staged root's place: everything of the install's is
            // taken back, and what took the place stays as it was.
            let left = Left::staged(holds_a_file);
            left.put_in_place("Gauntlet");
            left.journal("publishing", &["Gauntlet"]);
            fs::create_dir(left.root.join("config")).unwrap();
            fs::write(left.root.join("config").join("theirs.txt"), b"a stranger's").unwrap();
            let recovered = left.recover();
            assert_eq!(recovered.outcome.unwrap(), rolled_back(&[]));
            assert_eq!(folder_names(&left.root).unwrap(), left.holding(&["config"]));
            assert_eq!(
                fs::read(left.root.join("config").join("theirs.txt")).unwrap(),
                b"a stranger's"
            );
            assert_eq!(recovered.heard, heard_of(&left.root, &["forget"]));

            // Publishing, a root in neither place: nothing is done, and the journal stays.
            let left = Left::staged(holds_a_file);
            left.put_in_place("Gauntlet");
            left.journal("publishing", &["Gauntlet"]);
            fs::remove_file(left.staging().join("VERSION")).unwrap();
            let error = assert_left_alone(&left, "inconsistent", &context("a root nowhere"));
            assert!(error.to_string().contains("VERSION"), "{error}");

            // Publishing, a root recorded in place that is not there, still in the staging:
            // nothing is done, so no receipt stands for an installation without it.
            let left = Left::staged(holds_a_file);
            left.put_in_place("Gauntlet");
            left.journal("publishing", &["Gauntlet", "VERSION"]);
            let error = assert_left_alone(&left, "inconsistent", &context("a recorded root gone"));
            assert!(
                error
                    .to_string()
                    .contains("VERSION is recorded in place and is not there"),
                "{error}"
            );

            // Publishing, a journal that carries no receipt to write: nothing is moved.
            let left = Left::staged(holds_a_file);
            let mut journal = left.journal_value("publishing", &[]);
            journal.as_object_mut().unwrap().remove("receipt");
            left.write_journal(&journal);
            let error = assert_left_alone(&left, "inconsistent", &context("no receipt"));
            assert!(error.to_string().contains("carries no receipt"), "{error}");

            // Rolling back: what the publish put in place goes back, then everything goes.
            let left = Left::staged(holds_a_file);
            left.put_in_place("Gauntlet");
            left.put_in_place("VERSION");
            left.journal("rollingBack", &["Gauntlet"]);
            let recovered = left.recover();
            assert_eq!(recovered.outcome.unwrap(), rolled_back(&[]));
            left.as_it_was.holds(&context("rolling back"));
            assert_eq!(recovered.heard, heard_of(&left.root, &["forget"]));

            // Rolling back, a published saves folder the player has saved into since: it stays,
            // and the record keeps the folder.
            let left = Left::staged(holds_a_file);
            for root in ROOTS {
                left.put_in_place(root);
            }
            fs::write(left.root.join("saves").join("slot1.json"), b"{}").unwrap();
            left.journal("rollingBack", &ROOTS);
            let recovered = left.recover();
            assert_eq!(recovered.outcome.unwrap(), rolled_back(&["saves"]));
            assert_eq!(folder_names(&left.root).unwrap(), left.holding(&["saves"]));
            assert_eq!(
                fs::read(left.root.join("saves").join("slot1.json")).unwrap(),
                b"{}"
            );
            assert!(recovered.heard.is_empty(), "{:?}", recovered.heard);

            // Committed: only the cleanup is left.
            let left = Left::in_phase("committed", holds_a_file);
            let recovered = left.recover();
            assert_eq!(recovered.outcome.unwrap(), RecoveryOutcome::CleanedUp);
            left.assert_installed(&context("committed"));
            assert_eq!(recovered.heard, heard_of(&left.root, &["installed"]));
            assert_eq!(recovered.shell, left.shell_told());
        }
    }

    #[test]
    fn each_phase_recovers_with_its_staging_gone() {
        for holds_a_file in [false, true] {
            let context = |what: &str| format!("{what}, the folder holding a file: {holds_a_file}");

            // Staging, the staging removed by a rollback a crash cut short.
            let left = Left::in_phase("staging", holds_a_file);
            left.remove_staging();
            assert_eq!(left.recover().outcome.unwrap(), rolled_back(&[]));
            left.as_it_was.holds(&context("staging"));

            // Publishing, every root in place.
            let left = Left::staged(holds_a_file);
            for root in ROOTS {
                left.put_in_place(root);
            }
            left.journal("publishing", &ROOTS[..2]);
            left.remove_staging();
            let recovered = left.recover();
            assert!(
                matches!(recovered.outcome, Ok(RecoveryOutcome::Finished { .. })),
                "{:?}",
                recovered.outcome
            );
            left.assert_installed(&context("publishing"));

            // Publishing, a root neither in place nor in the staging, which is gone.
            let left = Left::staged(holds_a_file);
            for root in &ROOTS[..3] {
                left.put_in_place(root);
            }
            left.journal("publishing", &ROOTS[..3]);
            left.remove_staging();
            let error = assert_left_alone(&left, "inconsistent", &context("a root nowhere"));
            assert!(error.to_string().contains("saves"), "{error}");

            // Rolling back, nothing left in place.
            let left = Left::staged(holds_a_file);
            left.journal("rollingBack", &["Gauntlet"]);
            left.remove_staging();
            assert_eq!(left.recover().outcome.unwrap(), rolled_back(&[]));
            left.as_it_was.holds(&context("rolling back"));

            // Rolling back, a root in place that nothing can take back into a staging.
            let left = Left::staged(holds_a_file);
            left.put_in_place("Gauntlet");
            left.journal("rollingBack", &["Gauntlet"]);
            left.remove_staging();
            let error = assert_left_alone(&left, "inconsistent", &context("a root stranded"));
            assert!(error.to_string().contains("Gauntlet"), "{error}");

            // Rolling back, the saves the player put in place: they stay, and so does the folder.
            let left = Left::staged(holds_a_file);
            left.put_in_place("saves");
            fs::write(left.root.join("saves").join("slot1.json"), b"{}").unwrap();
            left.journal("rollingBack", &["saves"]);
            left.remove_staging();
            assert_eq!(left.recover().outcome.unwrap(), rolled_back(&["saves"]));
            assert_eq!(folder_names(&left.root).unwrap(), left.holding(&["saves"]));

            // Committed.
            let left = Left::in_phase("committed", holds_a_file);
            left.remove_staging();
            assert_eq!(left.recover().outcome.unwrap(), RecoveryOutcome::CleanedUp);
            left.assert_installed(&context("committed"));
        }
    }

    #[test]
    fn one_recovery_leaves_no_pending_file() {
        for phase in ["staging", "publishing", "rollingBack", "committed"] {
            for at in 0..3 {
                let left = Left::in_phase(phase, false);
                let planted = match at {
                    0 => left.journals().join("install.json.pending"),
                    1 => left.state().join("disc-files.json.pending"),
                    _ => left.state().join("receipt.json.pending"),
                };
                fs::write(&planted, b"{").unwrap();
                let context = format!("{phase}, {}", planted.display());
                let outcome = left.recover().outcome.unwrap();
                if phase == "staging" || phase == "rollingBack" {
                    assert_eq!(outcome, rolled_back(&[]), "{context}");
                    assert!(!left.state().exists(), "{context}");
                    left.as_it_was.holds(&context);
                } else {
                    // The installed tree holds no pending file.
                    left.assert_installed(&context);
                }
            }
        }
    }

    #[test]
    fn an_unjournaled_staging_and_its_lock_go_in_one_recovery() {
        for holds_a_file in [false, true] {
            // Stopped between the staging and its journal: the journal folder holds nothing yet.
            let left = Left::staged(holds_a_file);
            fs::remove_file(left.state().join(DISC_FILES)).unwrap();
            assert_eq!(
                folder_names(&left.state()).unwrap(),
                [STAGING, JOURNAL_DIR, LOCK_FILE]
            );
            let recovered = left.recover();
            assert_eq!(
                recovered.outcome.unwrap(),
                RecoveryOutcome::AbandonedRemoved
            );
            assert!(!left.state().exists());
            // Nothing says who made the folder, so it stays.
            assert_eq!(folder_names(&left.root).unwrap(), left.holding(&[]));
            assert_eq!(recovered.heard, heard_of(&left.root, &["forget"]));
        }
    }

    #[test]
    fn an_abandoned_state_folder_is_removed() {
        for names in [&[][..], &[LOCK_FILE], &[LOCK_FILE, JOURNAL_DIR]] {
            let top = tempfile::tempdir().unwrap();
            let root = top.path().join("game");
            let state = root.join(STATE_DIR);
            fs::create_dir_all(&state).unwrap();
            for &name in names {
                if name == JOURNAL_DIR {
                    fs::create_dir(state.join(name)).unwrap();
                } else {
                    fs::write(state.join(name), b"").unwrap();
                }
            }
            let recovered = recover_at(&root);
            assert_eq!(
                recovered.outcome.unwrap(),
                RecoveryOutcome::AbandonedRemoved,
                "{names:?}"
            );
            assert!(folder_names(&root).unwrap().is_empty(), "{names:?}");
            assert_eq!(recovered.heard, heard_of(&root, &["forget"]), "{names:?}");
        }
    }

    #[test]
    fn a_committed_recovery_records_and_tells_the_shell() {
        let left = Left::in_phase("committed", true);
        let recovered = left.recover();
        assert_eq!(recovered.outcome.unwrap(), RecoveryOutcome::CleanedUp);
        assert_eq!(recovered.heard, heard_of(&left.root, &["installed"]));
        assert_eq!(recovered.shell, left.shell_told());
        // A journal that carries no receipt has the shell told of this program's executable.
        let left = Left::in_phase("committed", true);
        let mut journal = left.journal_value("committed", &ROOTS);
        journal.as_object_mut().unwrap().remove("receipt");
        left.write_journal(&journal);
        let recovered = left.recover();
        assert_eq!(recovered.outcome.unwrap(), RecoveryOutcome::CleanedUp);
        assert_eq!(recovered.shell, left.shell_told());
    }

    #[cfg(windows)]
    #[test]
    fn the_system_is_kept_awake_while_recovering() {
        use std::sync::Mutex;

        use gdl_files::{PhaseId, Progress, RelPath};
        use windows_sys::Win32::System::Power::{ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

        use super::RECOVER_INSTALL;
        use crate::platform;

        /// A progress that reads the thread's requests as the recovery's phase begins.
        #[derive(Default)]
        struct Awake {
            state: Mutex<Option<u32>>,
        }

        impl Progress for Awake {
            fn phase(&self, phase: PhaseId) {
                if phase == RECOVER_INSTALL {
                    *self.state.lock().unwrap() = Some(platform::execution_state());
                }
            }

            fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
        }

        let left = Left::in_phase("staging", false);
        let before = platform::execution_state();
        let awake = Awake::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &awake,
        };
        let running = Version::parse(VERSION).unwrap();
        let outcome = recover_install_with(
            &left.root,
            &running,
            &mut Heard::default(),
            &job,
            &standing_in(&NoFaults, &answering),
        )
        .unwrap();
        assert_eq!(outcome, rolled_back(&[]));
        assert_eq!(
            *awake.state.lock().unwrap(),
            Some(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
        );
        assert_eq!(platform::execution_state(), before);
    }

    #[test]
    fn a_busy_lock_is_a_hold() {
        let left = Left::in_phase("staging", false);
        let before = snapshot(left.top.path()).unwrap();
        let held = InstallFolder::at(&left.root).unwrap().lock().unwrap();
        let recovered = left.recover();
        drop(held);
        let error = recovered.outcome.unwrap_err();
        assert!(
            matches!(error, RecoveryError::Lock(LockError::Busy)),
            "{error:?}"
        );
        assert_eq!(error.rule(), "busy");
        assert!(recovered.heard.is_empty());
        assert_eq!(snapshot(left.top.path()).unwrap(), before);
        // Once the other job lets go, the recovery goes on.
        assert_eq!(left.recover().outcome.unwrap(), rolled_back(&[]));
    }

    #[test]
    fn a_later_launchers_journal_is_left() {
        let cases: [Case; 5] = [
            ("an install journal for a later launcher", |left| {
                let mut journal = left.journal_value("staging", &[]);
                journal["minLauncher"] = json!("9.0.0");
                left.write_journal(&journal);
            }),
            ("an install journal of a later schema", |left| {
                let mut journal = left.journal_value("staging", &[]);
                journal["schema"] = json!(2);
                left.write_journal(&journal);
            }),
            ("another job's journal of a later schema", |left| {
                let journal = json!({"schema": 2, "minLauncher": VERSION});
                fs::write(left.journals().join("update.json"), pretty_json(&journal)).unwrap();
            }),
            ("a journal of a kind this launcher does not know", |left| {
                let journal = json!({"schema": 1, "minLauncher": VERSION});
                fs::write(left.journals().join("future.json"), pretty_json(&journal)).unwrap();
            }),
            ("a receipt for a later launcher", |left| {
                let receipt = json!({"schema": 1, "minLauncher": "9.0.0"});
                fs::write(left.state().join(RECEIPT_FILE), pretty_json(&receipt)).unwrap();
                fs::write(left.state().join("receipt.json.pending"), b"{").unwrap();
            }),
        ];
        for (what, made) in cases {
            // Beside each, a staging no journal names, which this launcher would otherwise
            // remove.
            let left = Left::staged(false);
            made(&left);
            assert_left_alone(&left, "newerLauncher", what);
        }
    }

    #[test]
    fn an_unreadable_journal_is_left() {
        let cases: [Case; 12] = [
            ("a journal that is not JSON", |left| {
                fs::write(left.journals().join(INSTALL_JOURNAL), b"not JSON\n").unwrap();
            }),
            ("a folder in the journal's place", |left| {
                fs::create_dir(left.journals().join(INSTALL_JOURNAL)).unwrap();
            }),
            ("a journal of no phase", |left| {
                let journal = json!({"schema": 1, "minLauncher": VERSION});
                left.write_journal(&journal);
            }),
            ("a journal naming no staging of an install's", |left| {
                let mut journal = left.journal_value("staging", &[]);
                journal["staging"] = json!("elsewhere");
                left.write_journal(&journal);
            }),
            ("a root that leaves the folder", |left| {
                let mut journal = left.journal_value("publishing", &[]);
                journal["roots"] = json!(["../elsewhere"]);
                left.write_journal(&journal);
            }),
            ("the state folder as a root", |left| {
                let mut journal = left.journal_value("publishing", &[]);
                journal["roots"] = json!([".GDL-LAUNCHER"]);
                left.write_journal(&journal);
            }),
            ("a root below the folder's top", |left| {
                let mut journal = left.journal_value("publishing", &[]);
                journal["roots"] = json!(["Gauntlet/PDATA"]);
                left.write_journal(&journal);
            }),
            ("a root no install puts in place", |left| {
                let mut journal = left.journal_value("publishing", &[]);
                journal["roots"] = json!(["Gauntlet", "notes.txt"]);
                left.write_journal(&journal);
            }),
            ("more folders made than lie on the folder's path", |left| {
                let mut journal = left.journal_value("staging", &[]);
                journal["madeFolders"] = json!(1000);
                left.write_journal(&journal);
            }),
            ("a root named twice", |left| {
                let mut journal = left.journal_value("publishing", &[]);
                journal["roots"] = json!(["Gauntlet", "Gauntlet"]);
                left.write_journal(&journal);
            }),
            (
                "a root recorded in place that is none of its roots",
                |left| {
                    let mut journal = left.journal_value("publishing", &[]);
                    journal["published"] = json!(["data"]);
                    left.write_journal(&journal);
                },
            ),
            ("another job's journal that is not JSON", |left| {
                left.journal("staging", &[]);
                fs::write(left.journals().join("update.json"), b"not JSON\n").unwrap();
            }),
        ];
        for (what, made) in cases {
            let left = Left::staged(false);
            made(&left);
            assert_left_alone(&left, "journalUnreadable", what);
        }
        // A journal taking back the player's own file as though the publish had put it in place:
        // the file stays where it is.
        let left = Left::staged(true);
        let mut journal = left.journal_value("rollingBack", &[]);
        journal["roots"] = json!([USER_FILE]);
        left.write_journal(&journal);
        assert_left_alone(&left, "journalUnreadable", "the player's file as a root");
        assert_eq!(fs::read(left.root.join(USER_FILE)).unwrap(), USER_BYTES);
        // A file in the journal folder's place.
        let left = Left::staged(false);
        retry_locked(|| fs::remove_dir(left.journals())).unwrap();
        fs::write(left.journals(), b"").unwrap();
        assert_left_alone(&left, "journalUnreadable", "a file of journals");
    }

    #[test]
    fn stale_stagings_and_pending_files_go_and_unknown_names_stay() {
        // Beside an installation that stands: every staging and pending file of the install's
        // goes, and nothing else.
        let left = Left::staged(false);
        for root in ROOTS {
            left.put_in_place(root);
        }
        left.write_receipt();
        let state = left.state();
        fs::create_dir(state.join(OTHER_STAGING)).unwrap();
        fs::write(state.join(OTHER_STAGING).join("left.bin"), b"left").unwrap();
        for pending in ["receipt.json.pending", "disc-files.json.pending"] {
            fs::write(state.join(pending), b"{").unwrap();
        }
        fs::write(left.journals().join("install.json.pending"), b"{").unwrap();
        fs::write(state.join("notes.txt"), b"not ours").unwrap();
        fs::create_dir(state.join("update-backup")).unwrap();
        fs::write(state.join("update-backup").join("kept.bin"), b"kept").unwrap();
        let recovered = left.recover();
        assert_eq!(recovered.outcome.unwrap(), RecoveryOutcome::CleanedUp);
        assert_eq!(
            folder_names(&state).unwrap(),
            [
                DISC_FILES,
                JOURNAL_DIR,
                LOCK_FILE,
                "notes.txt",
                RECEIPT_FILE,
                "update-backup"
            ]
        );
        assert!(folder_names(&left.journals()).unwrap().is_empty());
        assert_eq!(fs::read(state.join("notes.txt")).unwrap(), b"not ours");
        assert_eq!(
            fs::read(state.join("update-backup").join("kept.bin")).unwrap(),
            b"kept"
        );
        assert!(recovered.heard.is_empty(), "{:?}", recovered.heard);

        // Beside a journal: the stagings it does not name go too, and a name not the
        // launcher's keeps the state folder, which the rollback says it kept.
        let left = Left::in_phase("staging", false);
        let state = left.state();
        fs::create_dir(state.join(OTHER_STAGING)).unwrap();
        fs::write(state.join("notes.txt"), b"not ours").unwrap();
        let recovered = left.recover();
        assert_eq!(recovered.outcome.unwrap(), rolled_back(&[STATE_DIR]));
        assert_eq!(
            folder_names(&state).unwrap(),
            [JOURNAL_DIR, LOCK_FILE, "notes.txt"]
        );
        assert!(recovered.heard.is_empty(), "{:?}", recovered.heard);

        // With no receipt and no journal: the staging and the list of the disc's files go, and
        // the name not the launcher's keeps the state folder with its lock.
        let left = Left::staged(false);
        let state = left.state();
        fs::write(state.join("notes.txt"), b"not ours").unwrap();
        let recovered = left.recover();
        assert_eq!(recovered.outcome.unwrap(), RecoveryOutcome::CleanedUp);
        assert_eq!(
            folder_names(&state).unwrap(),
            [JOURNAL_DIR, LOCK_FILE, "notes.txt"]
        );
        assert!(recovered.heard.is_empty(), "{:?}", recovered.heard);
    }

    #[test]
    fn nothing_is_taken_back_once_the_receipt_stands() {
        for phase in ["staging", "rollingBack"] {
            let left = Left::in_phase(phase, false);
            left.write_receipt();
            assert_left_alone(&left, "inconsistent", phase);
        }
        // Something else in a staged root's place, beside the receipt.
        let left = Left::in_phase("publishing", false);
        left.write_receipt();
        fs::create_dir(left.root.join("config")).unwrap();
        assert_left_alone(&left, "inconsistent", "a target taken");
        // Stopped once the receipt was written, before the journal recorded it: the receipt is
        // written again, the same bytes, and the cleanup done.
        let left = Left::staged(false);
        for root in ROOTS {
            left.put_in_place(root);
        }
        left.journal("publishing", &ROOTS);
        left.write_receipt();
        let recovered = left.recover();
        assert!(
            matches!(recovered.outcome, Ok(RecoveryOutcome::Finished { .. })),
            "{:?}",
            recovered.outcome
        );
        left.assert_installed("the receipt there");
    }

    #[test]
    fn a_folder_with_nothing_to_recover_is_left_unlocked() {
        let top = tempfile::tempdir().unwrap();
        let missing = top.path().join("missing");
        let file = top.path().join("file.txt");
        fs::write(&file, b"not a folder").unwrap();
        let bare = top.path().join("bare");
        fs::create_dir(&bare).unwrap();
        // An installation whose state holds no lock file, nor anything of an install's to finish.
        let installed = top.path().join("installed");
        let state = installed.join(STATE_DIR);
        fs::create_dir_all(state.join(JOURNAL_DIR)).unwrap();
        fs::write(state.join(RECEIPT_FILE), receipt_bytes()).unwrap();
        fs::write(state.join(DISC_FILES), DISC_LIST).unwrap();
        let before = snapshot(top.path()).unwrap();
        for root in [&missing, &file, &bare, &installed] {
            let recovered = recover_at(root);
            assert_eq!(
                recovered.outcome.unwrap(),
                RecoveryOutcome::Nothing,
                "{}",
                root.display()
            );
            assert!(recovered.heard.is_empty());
        }
        assert_eq!(snapshot(top.path()).unwrap(), before);

        // A folder reached through a link is refused as the link, as taking its lock refuses it.
        let link = top.path().join("link");
        link_folder(&link, &installed).unwrap();
        let error = recover_at(&link).outcome.unwrap_err();
        assert!(
            matches!(&error, RecoveryError::Lock(LockError::Links { path }) if *path == link),
            "{error:?}"
        );
        assert_eq!(error.rule(), "links");
    }

    #[test]
    fn recovering_twice_does_nothing_more() {
        let abandoned = || {
            let left = Left::staged(false);
            fs::remove_file(left.state().join(DISC_FILES)).unwrap();
            left
        };
        let stale = || {
            let left = Left::in_phase("committed", false);
            retry_locked(|| fs::remove_file(left.journals().join(INSTALL_JOURNAL))).unwrap();
            fs::write(left.state().join("receipt.json.pending"), b"{").unwrap();
            left
        };
        let lefts = [
            ("staging", Left::in_phase("staging", true)),
            ("publishing", Left::in_phase("publishing", false)),
            ("committed", Left::in_phase("committed", false)),
            ("abandoned", abandoned()),
            ("stale", stale()),
        ];
        for (what, left) in lefts {
            let first = left.recover().outcome.unwrap();
            assert_ne!(first, RecoveryOutcome::Nothing, "{what}");
            let before = snapshot(left.top.path()).unwrap();
            let again = left.recover();
            assert_eq!(again.outcome.unwrap(), RecoveryOutcome::Nothing, "{what}");
            assert!(again.heard.is_empty(), "{what}");
            assert!(again.shell.is_empty(), "{what}");
            assert_eq!(snapshot(left.top.path()).unwrap(), before, "{what}");
        }
    }

    #[test]
    fn recovery_errors_name_their_rules_and_stay_small() {
        let path = PathBuf::from("game");
        for (error, rule, text) in [
            (
                RecoveryError::Lock(LockError::Busy),
                "busy",
                "Another job is using this installation".to_owned(),
            ),
            (
                RecoveryError::NewerLauncher,
                "newerLauncher",
                "A later launcher's state stands in this installation; this launcher changes \
                 nothing"
                    .to_owned(),
            ),
            (
                RecoveryError::JournalUnreadable {
                    detail: "the reason".into(),
                },
                "journalUnreadable",
                "Unreadable install journal: the reason".to_owned(),
            ),
            (
                RecoveryError::Inconsistent {
                    detail: "the reason".into(),
                },
                "inconsistent",
                "The interrupted install cannot be finished or taken back: the reason".to_owned(),
            ),
            (
                RecoveryError::Files(FilesError::Link { path: path.clone() }),
                "link",
                format!(
                    "Choose a directory without links or junctions: {}",
                    path.display()
                ),
            ),
            (
                RecoveryError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                },
                "io",
                format!("{}: refused", path.display()),
            ),
        ] {
            assert_eq!(error.rule(), rule, "{error:?}");
            assert_eq!(error.to_string(), text, "{error:?}");
        }
        assert!(size_of::<RecoveryError>() <= MOST_ERROR_BYTES);
    }
}
