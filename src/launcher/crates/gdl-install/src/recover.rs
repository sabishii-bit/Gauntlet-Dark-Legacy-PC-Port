//! Recovering whatever jobs a crash, a power cut or a kill left in an installation's folder, as
//! the launcher's next start does, and as an update does before it changes anything. Under the
//! installation's lock, each job's journal is judged on its own: one a later launcher stamped
//! leaves the whole installation to that launcher, a launcher older than the installation changes
//! nothing, and another job's journal that cannot be read is refused by its own part alone. While
//! the game runs from the installation, or from the copy an update moved aside, a journal is
//! neither finished nor taken back: the recovery holds until the game is closed. The fresh
//! install's part runs first, then the update's; and once no journal is left, what jobs left in
//! the state folder that the launcher's own rules name, and only that, is swept away.

use std::collections::BTreeSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{
    FilesError, Job, PENDING_SUFFIX, is_absent, is_staging_name, pending_path, read_json_bounded,
    retry_locked,
};
use gdl_release::{Platform, Version};

use crate::awake::keep_awake;
use crate::folder::{
    FolderError, InstallFolder, InstallLock, JOURNAL_DIR, LockError, RECEIPT_FILE, names_in,
};
use crate::hold::{HeldGame, HeldLock, game_closed_holding, lock_holding};
use crate::install::{
    FolderRecord, Found, RECOVER_INSTALL, RecoveryError, RecoveryOutcome, judge_stamps,
    recover_install_held,
};
#[cfg(windows)]
use crate::platform;
use crate::probe::GameUseError;
use crate::receipt::{MAX_RECEIPT_LEN, Receipt};
use crate::update::{
    DOWNLOAD_PREFIX, FINISH_CLEANUP, RECOVER_UPDATE, UPDATE_BACKUP, UPDATE_JOURNAL,
    UPDATE_STAGING_PREFIX, UpdateJournal, UpdateSettings, game_rule, is_retired_launcher_name,
    recover_update, remove_folder, remove_named,
};

/// What recovering an installation's folder did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Recovered {
    /// What became of a fresh install left unfinished there.
    pub install: RecoveryOutcome,
    /// What became of an update left unfinished there. A fresh install taken back gives the state
    /// folder up with the lock, and leaves anything else to the next start.
    pub update: UpdateRecovery,
    /// The names that jobs left in the state folder, which no journal named, removed: names
    /// directly in it, and the update journal's pending file as `journal/update.json.pending`. In
    /// byte order.
    pub swept: Vec<String>,
    /// Such names that could not be removed yet, kept for the next start: a retired launcher copy
    /// a launcher still running from it holds on Windows, or a backup holding a file another
    /// program has open. In byte order.
    pub left: Vec<String>,
}

impl Recovered {
    /// A recovery that found nothing to do.
    fn nothing() -> Recovered {
        Recovered {
            install: RecoveryOutcome::Nothing,
            update: UpdateRecovery::Nothing,
            swept: Vec::new(),
            left: Vec::new(),
        }
    }
}

/// What recovering an update that was stopped before it ended did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UpdateRecovery {
    /// No update was left unfinished there.
    Nothing,
    /// The update was taken back: the installation is the release it was before the update began,
    /// with its receipt, but for what `kept` names.
    RolledBack {
        /// The folders the update made for new names that stay, as something was put in them
        /// since.
        kept: Vec<String>,
        /// Whether the installed launcher copy was put back at the installation's top from its
        /// retired name, so that a launcher that ran from there before the recovery now runs from
        /// another name.
        launcher_restored: bool,
    },
    /// The update was finished: its receipt stands, written by the recovery when the update had
    /// made every move but that, and what it staged and moved aside is gone.
    Finished,
    /// The update stood already, and what it left of what it staged and moved aside is gone.
    CleanedUp,
    /// The update stands, but cleaning up after it stopped part-way: its journal is kept for the
    /// next start to finish it.
    CleanupLeft {
        /// What stopped it.
        detail: Box<str>,
    },
}

/// Why the jobs left in a folder could not be recovered, as technical detail for logs. What a
/// player reads is chosen from it elsewhere. Whatever stopped the recovery, every journal it did
/// not finish with is left for the next.
#[derive(Debug, thiserror::Error)]
pub enum RecoverError {
    /// The installation's lock was refused, for another reason than another job's hold, which is
    /// waited out; or a link on the folder's path or in the state the lock guards was found before
    /// anything was changed.
    #[error(transparent)]
    Lock(LockError),
    /// Whether the game runs from the installation could not be told.
    #[error(transparent)]
    Game(GameUseError),
    /// The recovery was asked to stop while it waited for the lock or for the game to close.
    #[error("Recovery cancelled while it waited; nothing was changed")]
    Cancelled,
    /// A later launcher's receipt or journal stands in the installation.
    #[error("A later launcher's state stands in this installation; this launcher changes nothing")]
    NewerLauncher,
    /// The launcher that runs is older than the installed release, whose jobs may follow rules
    /// newer than its own.
    #[error("This launcher file ({running}) is older than the installation ({installed})")]
    OlderLauncher {
        /// The running launcher's version.
        running: Box<str>,
        /// The installed release's version.
        installed: Box<str>,
    },
    /// The fresh install's part of the recovery failed.
    #[error(transparent)]
    Install(RecoveryError),
    /// The update's journal cannot be read as one.
    #[error("Unreadable update journal: {detail}")]
    UpdateJournalUnreadable {
        /// Why, with the journal's path.
        detail: Box<str>,
    },
    /// What the update's journal records does not match what the folder holds.
    #[error("The interrupted update cannot be finished or taken back: {detail}")]
    UpdateInconsistent {
        /// What does not match.
        detail: Box<str>,
    },
    /// Another kind of job's journal stands in the installation, which this launcher does not
    /// recover.
    #[error("Another job's journal stands in this installation: {journal}")]
    OtherJob {
        /// The journal's name.
        journal: Box<str>,
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

impl RecoverError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the lock's, the
    /// game's, the install recovery's or the file operation's rule, or `cancelled`,
    /// `newerLauncher`, `olderLauncher`, `journalUnreadable`, `inconsistent`, `otherJob` or `io`.
    pub fn rule(&self) -> &'static str {
        match self {
            RecoverError::Lock(error) => error.rule(),
            RecoverError::Game(error) => game_rule(error),
            RecoverError::Cancelled => "cancelled",
            RecoverError::NewerLauncher => "newerLauncher",
            RecoverError::OlderLauncher { .. } => "olderLauncher",
            RecoverError::Install(error) => error.rule(),
            RecoverError::UpdateJournalUnreadable { .. } => "journalUnreadable",
            RecoverError::UpdateInconsistent { .. } => "inconsistent",
            RecoverError::OtherJob { .. } => "otherJob",
            RecoverError::Files(error) => error.rule(),
            RecoverError::Io { .. } => "io",
        }
    }
}

/// Recovers whatever jobs were left in the folder at `root`, for the launcher of version
/// `running`, telling `record` of the folder as a fresh install's recovery tells it, and `job`'s
/// progress of each part as it is reached: [`RECOVER_INSTALL`], [`RECOVER_UPDATE`] and
/// [`FINISH_CLEANUP`]. The recovery holds while another job has the lock, and while the game
/// runs where a journal stands, and is cancelled only during such a hold, having changed nothing.
/// The system is kept out of idle sleep while it runs, and on Windows set to fail a call that
/// meets a critical error rather than show a dialog, before anything on the path is looked at.
///
/// Nothing is done, and the lock is not taken, when the folder holds no state folder, or nothing
/// in it to recover. Otherwise, under the installation's lock:
///
/// 1. a link at a journal or at the receipt, or a journal the system will not let be read,
///    refuses the recovery; then a journal or a receipt a later launcher stamped, or a journal of
///    a kind this launcher does not know, as [`RecoverError::NewerLauncher`]; a launcher older
///    than the installed release, as [`RecoverError::OlderLauncher`]; and a journal of an
///    expansion's job, which this launcher does not recover, as [`RecoverError::OtherJob`];
/// 2. while an install's or an update's journal stands, the game is waited for until it is closed
///    at the installation's top and, beside an update's journal, in the update's backup, as an
///    update moves it there;
/// 3. the fresh install is recovered as [`recover_install`](crate::recover_install) recovers it,
///    an install journal that cannot be read refusing that part alone;
/// 4. the update is recovered from its journal: one staged or rolling back is taken back as the
///    job takes a failure back; one swapping is finished when its receipt stands, or when every
///    move it lists is found made and the launcher copy and the release record at the top are
///    the new release's, and is taken back otherwise; one committed is cleaned up. A journal that
///    cannot be read refuses that part alone, and is left as it is;
/// 5. once no journal is left, what an update leaves in the state folder by the launcher's own
///    names is swept: its stagings and download folders, its backup, the retired launcher copies
///    and the pending file of its journal; the fresh install's part removes the receipt's. Anything
///    else stays.
///
/// A part whose journal cannot be read is refused once the other part has run, and nothing is
/// swept then; a part that fails as it runs stops the recovery there. A fresh install taken back
/// gives the state folder up with the lock, and leaves the rest to the next start.
pub fn recover(
    root: &Path,
    running: &Version,
    record: &mut dyn FolderRecord,
    job: &Job<'_>,
) -> Result<Recovered, RecoverError> {
    recover_with(root, running, record, job, &UpdateSettings::shipped())
}

/// Recovers the folder at `root` as [`recover`] does, asking the world beyond the folder through
/// `settings`.
pub(crate) fn recover_with(
    root: &Path,
    running: &Version,
    record: &mut dyn FolderRecord,
    job: &Job<'_>,
    settings: &UpdateSettings<'_>,
) -> Result<Recovered, RecoverError> {
    #[cfg(windows)]
    platform::fail_critical_errors();
    let _awake = keep_awake();
    let folder = match InstallFolder::at(root) {
        Ok(folder) => folder,
        // No folder, or a file, holds no state folder.
        Err(FolderError::Absent { .. } | FolderError::NotDirectory { .. }) => {
            return Ok(Recovered::nothing());
        }
        Err(FolderError::Links { path }) => {
            return Err(RecoverError::Lock(LockError::Links { path }));
        }
        Err(FolderError::Io { path, source }) => return Err(RecoverError::Io { path, source }),
    };
    // Looked at first without the lock, so that a folder with nothing to recover is left as it is,
    // its lock file included, and a job that holds the lock does not keep it waiting.
    let platform = Platform::of_this_build();
    let found = Found::at(&folder.state_dir()).map_err(judged)?;
    if !found.is_some_and(|found| calls_for_work(&found, platform)) {
        return Ok(Recovered::nothing());
    }
    let lock = match lock_holding(&folder, job, settings.hold_poll) {
        Ok(lock) => lock,
        // Given up since it was looked at.
        Err(HeldLock::Lock(LockError::NoStateFolder { .. })) => return Ok(Recovered::nothing()),
        Err(HeldLock::Lock(error)) => return Err(RecoverError::Lock(error)),
        Err(HeldLock::Cancelled) => return Err(RecoverError::Cancelled),
    };
    let (recovered, _lock) = recover_held(&folder, lock, running, record, job, settings, true)?;
    Ok(recovered)
}

/// Recovers whatever jobs were left in the installation's folder `folder`, under its `lock`, as
/// [`recover`] does past the lock, telling `job`'s progress of each part's phase when
/// `tell_phases`; an update recovering first keeps its own phases. Gives what was done, and the
/// lock while it is still held: a fresh install taken back, or a state folder given up, let it go
/// with the state folder, and nothing else is then recovered.
pub(crate) fn recover_held(
    folder: &InstallFolder,
    lock: InstallLock,
    running: &Version,
    record: &mut dyn FolderRecord,
    job: &Job<'_>,
    settings: &UpdateSettings<'_>,
    tell_phases: bool,
) -> Result<(Recovered, Option<InstallLock>), RecoverError> {
    let tell = |phase| {
        if tell_phases {
            job.progress.phase(phase);
        }
    };
    let state = folder.state_dir();
    let platform = Platform::of_this_build();
    let Some(found) = Found::at(&state).map_err(judged)? else {
        return Ok((Recovered::nothing(), Some(lock)));
    };
    let stamps = judge_stamps(folder, &found, running).map_err(judged)?;
    refuse_older(&state, running)?;
    let mut update = None;
    for (name, read) in stamps.others {
        if name != UPDATE_JOURNAL {
            return Err(RecoverError::OtherJob {
                journal: name.into(),
            });
        }
        let path = state.join(JOURNAL_DIR).join(UPDATE_JOURNAL);
        update = Some(read.and_then(|object| {
            UpdateJournal::from_object(&object)
                .map_err(|detail| format!("{}: {detail}", path.display()).into())
        }));
    }
    // Nothing is finished or taken back under a game that runs from what a journal names; what is
    // left without a journal is no tree anyone runs.
    let update_stands = matches!(update, Some(Ok(_)));
    if matches!(stamps.install, Some(Ok(_))) || update_stands {
        hold_for_game(folder, platform, update_stands, job, settings)?;
    }
    let mut refusal = None;
    let (install, lock) = match stamps.install {
        Some(Err(unreadable)) => {
            refusal = Some(RecoverError::Install(unreadable));
            (RecoveryOutcome::Nothing, Some(lock))
        }
        journal if found.calls_for_work() => {
            tell(RECOVER_INSTALL);
            // Read, since a journal that cannot be read is the arm above's.
            let journal = journal.and_then(Result::ok);
            recover_install_held(
                folder,
                lock,
                &found,
                journal,
                record,
                settings.faults,
                settings.notify_shell,
            )
            .map_err(RecoverError::Install)?
        }
        _ => (RecoveryOutcome::Nothing, Some(lock)),
    };
    let pending = match update {
        None => None,
        Some(Ok(journal)) => Some(journal),
        Some(Err(detail)) => {
            refusal.get_or_insert(RecoverError::UpdateJournalUnreadable { detail });
            None
        }
    };
    let Some(lock) = lock else {
        if let Some(refused) = refusal {
            return Err(refused);
        }
        let recovered = Recovered {
            install,
            ..Recovered::nothing()
        };
        return Ok((recovered, None));
    };
    let mut left = BTreeSet::new();
    let update = match pending {
        None => UpdateRecovery::Nothing,
        Some(journal) => {
            tell(RECOVER_UPDATE);
            let (recovered, kept) = recover_update(folder, journal, settings)?;
            left.extend(kept);
            recovered
        }
    };
    if let Some(refused) = refusal {
        return Err(refused);
    }
    let swept = if journals_remain(&state)? {
        BTreeSet::new()
    } else {
        tell(FINISH_CLEANUP);
        sweep(&state, platform, &mut left)?
    };
    let recovered = Recovered {
        install,
        update,
        swept: swept.into_iter().collect(),
        left: left.into_iter().collect(),
    };
    Ok((recovered, Some(lock)))
}

/// The recovery's refusal for what judging an installation's state for the fresh install's part
/// refused: a link found, a later launcher's state, or a look the system refused are the whole
/// recovery's; anything else is the install's.
fn judged(error: RecoveryError) -> RecoverError {
    match error {
        RecoveryError::Lock(error) => RecoverError::Lock(error),
        RecoveryError::NewerLauncher => RecoverError::NewerLauncher,
        RecoveryError::Files(error) => RecoverError::Files(error),
        RecoveryError::Io { path, source } => RecoverError::Io { path, source },
        other => RecoverError::Install(other),
    }
}

/// Whether a recovery has anything to do in a state folder that holds what `found` lists, for a
/// launcher built for `platform`: what a fresh install's recovery has work in, a journal of any
/// kind or its pending file, or a name the sweep removes.
fn calls_for_work(found: &Found, platform: Option<Platform>) -> bool {
    found.calls_for_work()
        || found.journals.iter().flatten().next().is_some()
        || found.names.iter().any(|name| {
            name.to_str()
                .is_some_and(|name| leftover(name, platform).is_some())
        })
}

/// Refuses a recovery by the launcher of version `running` when the receipt in the state folder
/// `state` records a later release, as [`RecoverError::OlderLauncher`]: a job left there may follow
/// rules newer than this launcher's. A receipt that cannot be read as one decides nothing here.
fn refuse_older(state: &Path, running: &Version) -> Result<(), RecoverError> {
    let Ok(receipt) = read_json_bounded::<Receipt>(&state.join(RECEIPT_FILE), MAX_RECEIPT_LEN)
    else {
        return Ok(());
    };
    match Version::parse(&receipt.version) {
        Ok(installed) if *running < installed => Err(RecoverError::OlderLauncher {
            running: running.to_string().into(),
            installed: installed.to_string().into(),
        }),
        _ => Ok(()),
    }
}

/// Waits until the game of the installation `folder` is closed, for a launcher built for
/// `platform`: at the installation's top, and, when `update_stands`, in the update's backup,
/// where an update moves the installed executable aside. A place with nothing there is passed
/// over, and the job holds while the game runs at one.
fn hold_for_game(
    folder: &InstallFolder,
    platform: Option<Platform>,
    update_stands: bool,
    job: &Job<'_>,
    settings: &UpdateSettings<'_>,
) -> Result<(), RecoverError> {
    // A launcher built for no release's platform has no game of its own to find.
    let Some(platform) = platform else {
        return Ok(());
    };
    let executable = platform.executable();
    let installed = folder.root().join(executable);
    let aside = folder.state_dir().join(UPDATE_BACKUP).join(executable);
    let places = [installed.as_path(), aside.as_path()];
    let places = if update_stands {
        &places[..]
    } else {
        &places[..1]
    };
    game_closed_holding(places, settings.game_closed, job, settings.hold_poll).map_err(|held| {
        match held {
            HeldGame::Game(error) => RecoverError::Game(error),
            HeldGame::Cancelled => RecoverError::Cancelled,
        }
    })
}

/// Whether a journal of any kind is left in the state folder `state`, its pending files aside.
fn journals_remain(state: &Path) -> Result<bool, RecoverError> {
    let journals = state.join(JOURNAL_DIR);
    match names_in(&journals) {
        Ok(names) => Ok(names
            .iter()
            .any(|name| !name.as_encoded_bytes().ends_with(PENDING_SUFFIX.as_bytes()))),
        Err(error) if is_absent(&error) => Ok(false),
        Err(source) => Err(RecoverError::Io {
            path: journals,
            source,
        }),
    }
}

/// What an update leaves directly in the state folder that the sweep removes, by its kind. The
/// receipt's pending file is a fresh install's leftover too, which that part removes first.
enum Leftover {
    /// An update's staging, or its download folder, by the prefix it is made with.
    Staging(&'static str),
    /// The update's backup.
    Backup,
    /// A launcher copy an update moved aside.
    Retired,
}

/// What `name`, directly in the state folder, is to the sweep of a launcher built for `platform`:
/// a name the launcher's rules make there for what an update leaves, exactly as they make it, or
/// `None` for anything else, which stays.
fn leftover(name: &str, platform: Option<Platform>) -> Option<Leftover> {
    if let Some(prefix) = [UPDATE_STAGING_PREFIX, DOWNLOAD_PREFIX]
        .into_iter()
        .find(|prefix| is_staging_name(prefix, name))
    {
        return Some(Leftover::Staging(prefix));
    }
    if name == UPDATE_BACKUP {
        return Some(Leftover::Backup);
    }
    platform
        .is_some_and(|platform| is_retired_launcher_name(name, platform))
        .then_some(Leftover::Retired)
}

/// Sweeps the state folder `state`, which no journal names anything in, of what jobs left there,
/// for a launcher built for `platform`, as [`leftover`] tells it, and of the update journal's
/// pending file: each removed only when it is what the launcher makes there, a real folder or
/// file and no link, a folder with what it holds. Gives the names removed; those that could not be
/// removed yet are added to `left`. A retired launcher copy is tried once, since on Windows a
/// launcher still running from it holds it until it ends.
fn sweep(
    state: &Path,
    platform: Option<Platform>,
    left: &mut BTreeSet<String>,
) -> Result<BTreeSet<String>, RecoverError> {
    let names = names_in(state).map_err(|source| RecoverError::Io {
        path: state.to_path_buf(),
        source,
    })?;
    let mut swept = BTreeSet::new();
    for name in names.into_iter().filter_map(|name| name.into_string().ok()) {
        let Some(kind) = leftover(&name, platform) else {
            continue;
        };
        let path = state.join(&name);
        let Some(entry) = fs::symlink_metadata(&path).ok() else {
            continue;
        };
        let removed = match kind {
            Leftover::Staging(prefix) if entry.is_dir() => remove_named(state, prefix, &name),
            Leftover::Backup if entry.is_dir() => remove_folder(&path),
            Leftover::Retired if entry.is_file() => {
                fs::remove_file(&path).map_err(|source| FilesError::Io {
                    path: path.clone(),
                    source,
                })
            }
            // What the launcher never makes at its name is not the launcher's to remove.
            _ => continue,
        };
        if removed.is_ok() {
            swept.insert(name);
        } else {
            left.insert(name);
        }
    }
    let journal_pending = pending_path(&state.join(JOURNAL_DIR).join(UPDATE_JOURNAL));
    if fs::symlink_metadata(&journal_pending).is_ok_and(|entry| entry.is_file()) {
        let name = format!("{JOURNAL_DIR}/{UPDATE_JOURNAL}{PENDING_SUFFIX}");
        if remove_file(&journal_pending).is_ok() {
            swept.insert(name);
        } else {
            left.insert(name);
        }
    }
    Ok(swept)
}

/// Removes the file at `path`, counting one already gone as removed. A file a scanner holds for a
/// moment is waited for.
fn remove_file(path: &Path) -> Result<(), FilesError> {
    match retry_locked(|| fs::remove_file(path)) {
        Ok(()) => Ok(()),
        Err(error) if is_absent(&error) => Ok(()),
        Err(source) => Err(FilesError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};
    use std::sync::Mutex;
    use std::time::{Duration, Instant};

    use gdl_files::{Cancel, FilesError, Hold, Job, PhaseId, Progress, RelPath};
    use gdl_fixtures::{InstallFixture, RuntimeFiles, link_folder, pretty_json, snapshot};
    use gdl_release::{Platform, Version};
    use serde_json::json;
    use tempfile::TempDir;

    use super::{RecoverError, Recovered, UpdateRecovery, leftover, recover_with};
    use crate::destination::FolderChecks;
    use crate::faults::NoFaults;
    use crate::folder::{
        InstallFolder, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE, STATE_DIR,
    };
    use crate::hold::SCANNER_GRACE;
    #[cfg(windows)]
    use crate::icon::{IconError, Personalised};
    use crate::install::{INSTALL_JOURNAL, RecoveryError, RecoveryOutcome, Unrecorded};
    use crate::probe::{GameStartError, GameUseError, VersionProbe};
    use crate::receipt::Receipt;
    use crate::test_platform::{other_platform, this_platform};
    use crate::update::{FINISH_CLEANUP, UpdateSettings};

    /// The launcher's version, which the tests recover as and their installations are of.
    const VERSION: &str = "1.0.0-beta";

    /// How often the tests' holds look again: far more often than a job's, so that they run fast.
    const POLL: Duration = Duration::from_millis(20);

    /// The staging the hand-written install journals name.
    const INSTALL_STAGING: &str = "install-AbC123xy";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// An installation of the standard runtime of [`VERSION`], as the launcher leaves one, in a
    /// folder of the test's own.
    fn installation() -> (TempDir, InstallFixture) {
        installation_of(VERSION)
    }

    /// An installation of the standard runtime of `version`, with the lock file an install
    /// leaves in its state folder.
    fn installation_of(version: &str) -> (TempDir, InstallFixture) {
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(this_platform().as_str(), version);
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        fs::write(state_of(&fixture.root).join(LOCK_FILE), b"").unwrap();
        (dir, fixture)
    }

    /// The state folder of the installation at `root`.
    fn state_of(root: &Path) -> PathBuf {
        root.join(STATE_DIR)
    }

    /// Writes the journal `journal` named `name` into the state of the installation at `root`.
    fn write_journal(root: &Path, name: &str, journal: &[u8]) {
        let journals = state_of(root).join(JOURNAL_DIR);
        fs::create_dir_all(&journals).unwrap();
        fs::write(journals.join(name), journal).unwrap();
    }

    /// Lays out in the folder at `root` what a fresh install leaves once it has staged a file and
    /// written its journal: the state folder with its lock, the staging and the journal.
    fn install_left(root: &Path) {
        let state = state_of(root);
        fs::create_dir_all(state.join(INSTALL_STAGING).join("Gauntlet")).unwrap();
        fs::write(
            state.join(INSTALL_STAGING).join("Gauntlet").join("WAR.WAD"),
            b"war",
        )
        .unwrap();
        fs::write(state.join(LOCK_FILE), b"").unwrap();
        let journal = json!({
            "schema": 1,
            "minLauncher": VERSION,
            "phase": "staging",
            "madeFolders": 0,
            "staging": INSTALL_STAGING,
            "roots": [],
            "published": [],
        });
        write_journal(root, INSTALL_JOURNAL, &pretty_json(&journal));
    }

    /// A probe the recoveries never run.
    fn unprobed(_: &Path, _: &Cancel) -> Result<VersionProbe, GameStartError> {
        panic!("a recovery probes no game")
    }

    /// A personaliser the recoveries never run.
    #[cfg(windows)]
    fn unpersonalised(_: &Path, _: &[u8]) -> Result<Personalised, IconError> {
        panic!("a recovery sets no icon")
    }

    /// A closed-game probe that finds the game closed.
    fn closed(_: &Path) -> Result<(), GameUseError> {
        Ok(())
    }

    /// A shell that is told nothing it keeps.
    fn unheard(_: &Path) {}

    /// The world beyond the folder as the recovery tests give it: no faults, the game closed by
    /// `game_closed`, the receipt written as the launcher writes it, and holds that look again
    /// every [`POLL`].
    fn standing_in<'a>(
        game_closed: &'a dyn Fn(&Path) -> Result<(), GameUseError>,
    ) -> UpdateSettings<'a> {
        UpdateSettings {
            faults: &NoFaults,
            folders: &FolderChecks::SHIPPED,
            probe: &unprobed,
            game_closed,
            #[cfg(windows)]
            personalise: &unpersonalised,
            notify_shell: &unheard,
            write_receipt: &Receipt::write,
            hold_poll: POLL,
        }
    }

    /// What a progress was told of phases and holds.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Told {
        Phase(&'static str),
        Held(Hold),
        Resumed,
    }

    /// A progress that keeps what it is told and runs `on_hold` as a hold begins.
    struct Heard<'a> {
        told: Mutex<Vec<Told>>,
        on_hold: &'a (dyn Fn(Hold) + Sync),
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

    /// Recovers the folder at `root` as the launcher of version `running`, the game found as
    /// `game_closed` finds it and `on_hold` run as a hold begins; gives what it gave and what its
    /// progress was told.
    fn recover_told(
        root: &Path,
        running: &str,
        game_closed: &dyn Fn(&Path) -> Result<(), GameUseError>,
        cancel: &Cancel,
        on_hold: &(dyn Fn(Hold) + Sync),
    ) -> (Result<Recovered, RecoverError>, Vec<Told>) {
        let heard = Heard {
            told: Mutex::default(),
            on_hold,
        };
        let job = Job {
            cancel,
            progress: &heard,
        };
        let settings = standing_in(game_closed);
        let recovered = recover_with(root, &version(running), &mut Unrecorded, &job, &settings);
        (recovered, heard.told.into_inner().unwrap())
    }

    /// Recovers the folder at `root` as the launcher of version `running`, with the game closed.
    fn recover_at(root: &Path, running: &str) -> Result<Recovered, RecoverError> {
        let cancel = Cancel::default();
        recover_told(root, running, &closed, &cancel, &|_| {}).0
    }

    /// What a recovery that found nothing to do gives.
    fn nothing() -> Recovered {
        Recovered {
            install: RecoveryOutcome::Nothing,
            update: UpdateRecovery::Nothing,
            swept: Vec::new(),
            left: Vec::new(),
        }
    }

    /// What a recovery that swept `swept` and nothing else gives.
    fn swept(swept: &[&str]) -> Recovered {
        Recovered {
            swept: swept.iter().map(|&name| name.to_owned()).collect(),
            ..nothing()
        }
    }

    /// The name of a retired launcher copy for `platform`, of `version`, as an update names one.
    fn retired_name(version: &str, platform: Platform) -> String {
        let suffix = match platform {
            Platform::WindowsX64 => ".exe",
            Platform::LinuxX64 => "",
        };
        format!("retired-launcher-{version}-AbC123xy{suffix}")
    }

    /// A closed-game probe that finds the game running for `running` from its first call, then
    /// closed, counting its calls.
    struct RunsFor {
        running: Duration,
        until: Cell<Option<Instant>>,
        calls: Cell<u32>,
    }

    impl RunsFor {
        fn new(running: Duration) -> RunsFor {
            RunsFor {
                running,
                until: Cell::new(None),
                calls: Cell::new(0),
            }
        }

        fn closed(&self, _: &Path) -> Result<(), GameUseError> {
            self.calls.set(self.calls.get() + 1);
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

    /// How long a stand-in game runs, past the time a hold for it begins: on Windows past the
    /// scanner's grace.
    fn running_for() -> Duration {
        let past_hold = Duration::from_millis(300);
        if cfg!(windows) {
            SCANNER_GRACE + past_hold
        } else {
            past_hold
        }
    }

    #[test]
    fn recover_errors_name_their_rules_and_stay_small() {
        let path = PathBuf::from("install");
        let cases = [
            (
                RecoverError::Lock(LockError::Busy),
                "busy",
                "Another job is using this installation".to_owned(),
            ),
            (
                RecoverError::Game(GameUseError::Missing),
                "missing",
                "The installed executable is missing".to_owned(),
            ),
            (
                RecoverError::Cancelled,
                "cancelled",
                "Recovery cancelled while it waited; nothing was changed".to_owned(),
            ),
            (
                RecoverError::NewerLauncher,
                "newerLauncher",
                "A later launcher's state stands in this installation; this launcher changes \
                 nothing"
                    .to_owned(),
            ),
            (
                RecoverError::OlderLauncher {
                    running: "1.0.0-beta".into(),
                    installed: "1.0.0-beta.1".into(),
                },
                "olderLauncher",
                "This launcher file (1.0.0-beta) is older than the installation (1.0.0-beta.1)"
                    .to_owned(),
            ),
            (
                RecoverError::Install(RecoveryError::Inconsistent {
                    detail: "the reason".into(),
                }),
                "inconsistent",
                "The interrupted install cannot be finished or taken back: the reason".to_owned(),
            ),
            (
                RecoverError::UpdateJournalUnreadable {
                    detail: "the reason".into(),
                },
                "journalUnreadable",
                "Unreadable update journal: the reason".to_owned(),
            ),
            (
                RecoverError::UpdateInconsistent {
                    detail: "the reason".into(),
                },
                "inconsistent",
                "The interrupted update cannot be finished or taken back: the reason".to_owned(),
            ),
            (
                RecoverError::OtherJob {
                    journal: "expansion-n64.json".into(),
                },
                "otherJob",
                "Another job's journal stands in this installation: expansion-n64.json".to_owned(),
            ),
            (
                RecoverError::Files(FilesError::Link { path: path.clone() }),
                "link",
                format!(
                    "Choose a directory without links or junctions: {}",
                    path.display()
                ),
            ),
            (
                RecoverError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                },
                "io",
                format!("{}: refused", path.display()),
            ),
        ];
        for (error, rule, text) in cases {
            assert_eq!(error.rule(), rule, "{error:?}");
            assert_eq!(error.to_string(), text, "{error:?}");
        }
        assert!(size_of::<RecoverError>() <= MOST_ERROR_BYTES);
    }

    #[test]
    fn a_folder_with_nothing_to_recover_is_left_unlocked() {
        let top = tempfile::tempdir().unwrap();
        let missing = top.path().join("missing");
        let file = top.path().join("file.txt");
        fs::write(&file, b"not a folder").unwrap();
        let bare = top.path().join("bare");
        fs::create_dir(&bare).unwrap();
        let fixture = InstallFixture::write(
            &top.path().join("installed"),
            RuntimeFiles::standard(this_platform().as_str(), VERSION),
        )
        .unwrap();
        let before = snapshot(top.path()).unwrap();
        for root in [&missing, &file, &bare, &fixture.root] {
            assert_eq!(
                recover_at(root, VERSION).unwrap(),
                nothing(),
                "{}",
                root.display()
            );
        }
        // No lock file was made, nor anything else.
        assert_eq!(snapshot(top.path()).unwrap(), before);

        // A folder reached through a link is refused as the link, as taking its lock refuses it.
        let link = top.path().join("link");
        link_folder(&link, &fixture.root).unwrap();
        match recover_at(&link, VERSION) {
            Err(RecoverError::Lock(LockError::Links { path })) => assert_eq!(path, link),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn no_journal_no_game_hold() {
        // Without a journal, leftovers are swept with the game running: nobody runs what they
        // hold.
        let (_dir, fixture) = installation();
        let state = state_of(&fixture.root);
        fs::create_dir_all(state.join("stage-AbC123xy").join("shaders")).unwrap();
        fs::write(state.join("stage-AbC123xy").join("gauntlet"), b"staged").unwrap();
        let asked = Cell::new(0);
        let running = |_: &Path| {
            asked.set(asked.get() + 1);
            Err(GameUseError::Running)
        };
        let cancel = Cancel::default();
        let (recovered, told) = recover_told(&fixture.root, VERSION, &running, &cancel, &|_| {});
        assert_eq!(recovered.unwrap(), swept(&["stage-AbC123xy"]));
        assert_eq!(asked.get(), 0);
        assert_eq!(told, [Told::Phase(FINISH_CLEANUP.0)]);
        assert!(!state.join("stage-AbC123xy").exists());
    }

    #[test]
    fn the_sweep_removes_owned_leftovers_alone() {
        let (_dir, fixture) = installation();
        let state = state_of(&fixture.root);
        let platform = this_platform();
        // What the launcher's rules name, each as the launcher makes it.
        let retired = retired_name("0.9.0", platform);
        fs::create_dir_all(state.join("stage-AbC123xy").join("data")).unwrap();
        fs::write(state.join("stage-AbC123xy").join("data").join("x"), b"x").unwrap();
        fs::create_dir(state.join("download-Qw3rTy12")).unwrap();
        fs::write(state.join("download-Qw3rTy12").join("launcher"), b"l").unwrap();
        fs::create_dir_all(state.join("update-backup").join("shaders")).unwrap();
        fs::write(
            state.join("update-backup").join("shaders").join("old.spv"),
            b"o",
        )
        .unwrap();
        fs::write(state.join(&retired), b"an old launcher").unwrap();
        write_journal(&fixture.root, "update.json.pending", b"{");
        // What they do not, or not in that form, or not this launcher's.
        let other_retired = retired_name("0.9.0", other_platform());
        let kept_files = [
            "download-AbC123xy.txt",
            "retired-launcher-x",
            "retired-dreamcast-AbC123xy",
            "retired-launcher-notaversion-AbC123xy",
            "stage-x",
            "notes.txt",
            "install.json.pending",
            other_retired.as_str(),
        ];
        let kept_folders = ["stage-n64-AbC123xy", "stage-AbC123x", "update-backup2"];
        for name in kept_files {
            fs::write(state.join(name), b"kept").unwrap();
        }
        for name in kept_folders {
            fs::create_dir(state.join(name)).unwrap();
            fs::write(state.join(name).join("kept"), b"kept").unwrap();
        }
        // A link at a staging's name is not a staging the launcher made.
        let elsewhere = fixture.root.parent().unwrap().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        fs::write(elsewhere.join("kept"), b"kept").unwrap();
        link_folder(&state.join("stage-L1nk0000"), &elsewhere).unwrap();
        let mut expected = vec![
            "download-Qw3rTy12".to_owned(),
            "journal/update.json.pending".to_owned(),
            retired.clone(),
            "stage-AbC123xy".to_owned(),
            "update-backup".to_owned(),
        ];
        expected.sort();
        let recovered = recover_at(&fixture.root, VERSION).unwrap();
        assert_eq!(
            recovered,
            Recovered {
                swept: expected,
                ..nothing()
            }
        );
        for name in kept_files {
            assert_eq!(fs::read(state.join(name)).unwrap(), b"kept", "{name}");
        }
        for name in kept_folders {
            assert_eq!(
                fs::read(state.join(name).join("kept")).unwrap(),
                b"kept",
                "{name}"
            );
        }
        assert_eq!(fs::read(elsewhere.join("kept")).unwrap(), b"kept");
        assert!(fs::symlink_metadata(state.join("stage-L1nk0000")).is_ok());
        assert!(state.join(RECEIPT_FILE).is_file());
    }

    #[test]
    fn leftovers_are_known_by_the_names_the_launcher_makes() {
        let windows = Some(Platform::WindowsX64);
        let linux = Some(Platform::LinuxX64);
        for (name, platform, swept) in [
            ("stage-AbC123xy", linux, true),
            ("download-AbC123xy", linux, true),
            ("update-backup", linux, true),
            ("receipt.json.pending", linux, false),
            ("retired-launcher-1.0.0-beta-AbC123xy", linux, true),
            ("retired-launcher-1.0.0-beta-AbC123xy.exe", windows, true),
            ("retired-launcher-1.0.0-rc.1+build.2-AbC123xy", linux, true),
            ("retired-launcher-1.0.0-beta-AbC123xy", windows, false),
            ("retired-launcher-1.0.0-beta-AbC123xy.exe", linux, false),
            ("retired-launcher-1.0.0-beta-AbC123xy", None, false),
            ("retired-launcher-1.0-AbC123xy", linux, false),
            ("retired-launcher-1.0.0-beta-AbC123x", linux, false),
            ("retired-launcher-1.0.0-beta-AbC123xy-", linux, false),
            ("retired-n64-AbC123xy", linux, false),
            ("stage-n64-AbC123xy", linux, false),
            ("stage-dreamcast-AbC123xy", linux, false),
            ("install-AbC123xy", linux, false),
            ("stage-AbC12_xy", linux, false),
            ("disc-files.json.pending", linux, false),
            ("receipt.json", linux, false),
            ("lock", linux, false),
        ] {
            assert_eq!(
                leftover(name, platform).is_some(),
                swept,
                "{name} {platform:?}"
            );
        }
    }

    #[test]
    fn unknown_names_in_the_state_folder_are_left() {
        // A recovery with work to do, beside names not the launcher's: they stay as they were,
        // byte for byte.
        let (_dir, fixture) = installation();
        let state = state_of(&fixture.root);
        fs::create_dir(state.join("stage-AbC123xy")).unwrap();
        fs::write(state.join("notes.txt"), b"not ours").unwrap();
        fs::create_dir(state.join("mine")).unwrap();
        fs::write(state.join("mine").join("save.bin"), b"mine").unwrap();
        fs::write(state.join("lock2"), b"").unwrap();
        let recovered = recover_at(&fixture.root, VERSION).unwrap();
        assert_eq!(recovered, swept(&["stage-AbC123xy"]));
        assert_eq!(fs::read(state.join("notes.txt")).unwrap(), b"not ours");
        assert_eq!(
            fs::read(state.join("mine").join("save.bin")).unwrap(),
            b"mine"
        );
        assert!(state.join("lock2").is_file());
    }

    #[test]
    fn an_unreadable_update_journal_no_longer_blocks_the_install_recovery() {
        // A fresh install's journal beside an update journal that cannot be read: the install is
        // taken back, and the update journal stays for its own recovery to refuse.
        let top = tempfile::tempdir().unwrap();
        let root = top.path().join("game");
        install_left(&root);
        let unreadable = b"not JSON\n";
        write_journal(&root, "update.json", unreadable);
        let recovered = recover_at(&root, VERSION);
        match recovered {
            Err(error @ RecoverError::UpdateJournalUnreadable { .. }) => {
                assert_eq!(error.rule(), "journalUnreadable");
            }
            other => panic!("{other:?}"),
        }
        let state = state_of(&root);
        assert!(!state.join(INSTALL_STAGING).exists());
        assert!(!state.join(JOURNAL_DIR).join(INSTALL_JOURNAL).exists());
        assert_eq!(
            fs::read(state.join(JOURNAL_DIR).join("update.json")).unwrap(),
            unreadable
        );

        // An install journal that cannot be read refuses the install's part, and changes nothing
        // of the install's.
        let top = tempfile::tempdir().unwrap();
        let root = top.path().join("game");
        install_left(&root);
        write_journal(&root, INSTALL_JOURNAL, b"not JSON\n");
        let before = snapshot(top.path()).unwrap();
        match recover_at(&root, VERSION) {
            Err(error @ RecoverError::Install(RecoveryError::JournalUnreadable { .. })) => {
                assert_eq!(error.rule(), "journalUnreadable");
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(snapshot(top.path()).unwrap(), before);
    }

    #[test]
    fn an_unreadable_update_journal_is_left_for_its_writer() {
        let journals: [&[u8]; 4] = [
            b"not JSON\n",
            br#"{"schema": 1, "minLauncher": "1.0.0-beta", "phase": "staged"}"#,
            br#"{"schema": "one", "minLauncher": "1.0.0-beta"}"#,
            br#"{"schema": 1, "minLauncher": "1.0.0-beta", "phase": "staged", "from": "1.0.0-beta", "to": "1.0.0-beta.1", "staging": "stage-AbC123xy", "backup": ["saves/hero.json"], "publish": [], "madeFolders": [], "launcher": "x", "retired": "x", "receipt": {}}"#,
        ];
        for journal in journals {
            let (dir, fixture) = installation();
            let state = state_of(&fixture.root);
            fs::create_dir(state.join("stage-AbC123xy")).unwrap();
            write_journal(&fixture.root, "update.json", journal);
            let before = snapshot(dir.path()).unwrap();
            match recover_at(&fixture.root, VERSION) {
                Err(RecoverError::UpdateJournalUnreadable { detail }) => {
                    assert!(detail.contains("update.json"), "{detail}");
                }
                other => panic!("{other:?}"),
            }
            // Nothing is swept beside it, nor is it touched.
            assert_eq!(snapshot(dir.path()).unwrap(), before);
        }
    }

    #[test]
    fn a_later_launchers_journal_makes_everything_read_only() {
        let later = [
            ("update.json", json!({"schema": 1, "minLauncher": "9.0.0"})),
            ("update.json", json!({"schema": 2, "minLauncher": VERSION})),
            ("install.json", json!({"schema": 1, "minLauncher": "9.0.0"})),
            (
                "expansion-dreamcast.json",
                json!({"schema": 1, "minLauncher": "9.0.0"}),
            ),
            ("future.json", json!({"schema": 1, "minLauncher": VERSION})),
        ];
        for (name, journal) in later {
            // Beside an install's and an update's leftovers, which this launcher would otherwise
            // remove.
            let (dir, fixture) = installation();
            let state = state_of(&fixture.root);
            fs::create_dir(state.join("stage-AbC123xy")).unwrap();
            fs::create_dir(state.join(INSTALL_STAGING)).unwrap();
            write_journal(&fixture.root, name, &pretty_json(&journal));
            let before = snapshot(dir.path()).unwrap();
            match recover_at(&fixture.root, VERSION) {
                Err(error @ RecoverError::NewerLauncher) => {
                    assert_eq!(error.rule(), "newerLauncher");
                }
                other => panic!("{name}: {other:?}"),
            }
            assert_eq!(snapshot(dir.path()).unwrap(), before, "{name}");
        }
        // A receipt a later launcher stamped.
        let (dir, fixture) = installation();
        let mut receipt = fixture.receipt();
        receipt["minLauncher"] = json!("9.0.0");
        fixture.write_receipt(&receipt).unwrap();
        fs::create_dir(state_of(&fixture.root).join("stage-AbC123xy")).unwrap();
        let before = snapshot(dir.path()).unwrap();
        assert!(matches!(
            recover_at(&fixture.root, VERSION),
            Err(RecoverError::NewerLauncher)
        ));
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn an_older_launcher_recovers_nothing() {
        let (dir, fixture) = installation_of("1.0.0-beta.1");
        let state = state_of(&fixture.root);
        fs::create_dir(state.join("stage-AbC123xy")).unwrap();
        let before = snapshot(dir.path()).unwrap();
        match recover_at(&fixture.root, VERSION) {
            Err(error @ RecoverError::OlderLauncher { .. }) => {
                assert_eq!(error.rule(), "olderLauncher");
                assert!(error.to_string().contains("1.0.0-beta.1"), "{error}");
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(snapshot(dir.path()).unwrap(), before);
        // A launcher of the installation's release, or a later one, sweeps it.
        for running in ["1.0.0-beta.1", "1.0.0"] {
            let (_dir, fixture) = installation_of("1.0.0-beta.1");
            fs::create_dir(state_of(&fixture.root).join("stage-AbC123xy")).unwrap();
            assert_eq!(
                recover_at(&fixture.root, running).unwrap(),
                swept(&["stage-AbC123xy"])
            );
        }
    }

    #[test]
    fn a_recovery_waits_for_the_lock() {
        let (_dir, fixture) = installation();
        let state = state_of(&fixture.root);
        fs::create_dir(state.join("stage-AbC123xy")).unwrap();
        // Another job holds the lock, and lets it go once the recovery holds for it.
        let held = Mutex::new(Some(
            InstallFolder::at(&fixture.root).unwrap().lock().unwrap(),
        ));
        let let_go = |_: Hold| drop(held.lock().unwrap().take());
        let cancel = Cancel::default();
        let (recovered, told) = recover_told(&fixture.root, VERSION, &closed, &cancel, &let_go);
        assert_eq!(recovered.unwrap(), swept(&["stage-AbC123xy"]));
        assert_eq!(
            told,
            [
                Told::Held(Hold::LockBusy),
                Told::Resumed,
                Told::Phase(FINISH_CLEANUP.0)
            ]
        );

        // Asked to stop while it waits, it changes nothing.
        fs::create_dir(state.join("stage-AbC123xy")).unwrap();
        let before = snapshot(&state).unwrap();
        let lock = InstallFolder::at(&fixture.root).unwrap().lock().unwrap();
        let cancel = Cancel::default();
        let stop = |_: Hold| cancel.cancel();
        let (recovered, told) = recover_told(&fixture.root, VERSION, &closed, &cancel, &stop);
        drop(lock);
        assert!(
            matches!(recovered, Err(RecoverError::Cancelled)),
            "{recovered:?}"
        );
        assert_eq!(told, [Told::Held(Hold::LockBusy)]);
        assert_eq!(snapshot(&state).unwrap(), before);
    }

    #[test]
    fn an_install_journal_holds_for_a_running_game() {
        // A fresh install's journal: the game it may have put in place runs, so the recovery
        // holds before it takes the install back.
        let top = tempfile::tempdir().unwrap();
        let root = top.path().join("game");
        install_left(&root);
        let game = RunsFor::new(running_for());
        let game_closed = |path: &Path| game.closed(path);
        let asked = Mutex::new(Vec::new());
        let probing = |path: &Path| {
            asked.lock().unwrap().push(path.to_path_buf());
            game_closed(path)
        };
        let cancel = Cancel::default();
        let (recovered, told) = recover_told(&root, VERSION, &probing, &cancel, &|_| {});
        assert_eq!(
            recovered.unwrap().install,
            RecoveryOutcome::RolledBack { kept: Vec::new() }
        );
        assert_eq!(
            told,
            [
                Told::Held(Hold::GameRunning),
                Told::Resumed,
                Told::Phase(crate::install::RECOVER_INSTALL.0),
            ]
        );
        // Only the game's place at the top is asked: no update journal stands.
        let executable = root.join(this_platform().executable());
        assert!(
            asked
                .into_inner()
                .unwrap()
                .iter()
                .all(|path| *path == executable)
        );
        assert!(!root.join(STATE_DIR).exists());
    }
}
