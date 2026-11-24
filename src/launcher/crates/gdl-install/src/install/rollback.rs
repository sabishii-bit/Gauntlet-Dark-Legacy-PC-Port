//! Taking a fresh install back, and cleaning up after one that stands. A failure or a cancel before
//! the receipt is written takes back what the install made, under the installation's lock: the
//! roots the publish put in place go back into the staging, last first; then the staging goes,
//! the install's state files, its journal and the state folder with its lock; then the folders it
//! made, and the record of installations forgets the folder. The receipt in place is the commit
//! point: from then on the folder is an installation, which no rollback touches, and only the
//! staging and the journal go. Each step counts a name already gone as removed, and the journal
//! goes after everything it names, so that whatever cuts a step short, the next start finishes the
//! work from the journal, or finds nothing left to do.

use std::fs;
use std::io;
use std::path::Path;

use gdl_files::{FilesError, PAYLOAD_NAMES, RelPath, Staging, is_absent, no_links, retry_locked};

use super::journal::{INSTALL_JOURNAL, InstallPhase, JournalFile, remove_if_there};
use super::settings::{FaultPoint, FaultPoints};
use super::{CONFIG_FOLDER, FolderRecord, SAVES_FOLDER};
use crate::compat::install_leftover_files;
use crate::folder::{InstallFolder, InstallLock, JOURNAL_DIR, RECEIPT_FILE, STATE_DIR, names_in};
use crate::receipt::Receipt;

/// What an install made in its folder once it held the installation's lock, for a rollback to
/// take back.
pub(super) struct Rollback<'a> {
    /// The installation's folder.
    pub(super) folder: &'a Path,
    /// How many folders the install made: the installation's folder, when it was not there, and
    /// each missing folder above it, counted from the folder up.
    pub(super) made_folders: u32,
    /// The installation's lock, held until the state folder is given up with it.
    pub(super) lock: InstallLock,
    /// The staging and its journal, once the staging was made.
    pub(super) begun: Option<Begun<'a>>,
    /// Where the rollback passes its fault points.
    pub(super) faults: &'a dyn FaultPoints,
}

/// The staging an install made, and the journal that names it once that was written.
pub(super) struct Begun<'a> {
    /// The staging; `None` when the journal names one that is gone, as a rollback a crash cut
    /// short leaves it.
    pub(super) staging: Option<Staging>,
    /// The journal, once written: when it records the publish, what was put in place goes back
    /// into the staging first.
    pub(super) journal: Option<JournalFile<'a>>,
}

/// What a rollback took back.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct TakenBack {
    /// The roots the publish put in place that stay there, for what the player has put in them
    /// since: a `saves` or `config` folder that is not empty.
    pub(super) kept: Vec<String>,
    /// Whether the state folder went with the lock: it stays when it holds a name that is not the
    /// launcher's to remove.
    pub(super) given_up: bool,
}

impl TakenBack {
    /// Whether everything the install made of its own was taken back.
    pub(super) fn is_whole(&self) -> bool {
        self.given_up && self.kept.is_empty()
    }
}

/// Why a rollback stopped before it took the install back.
#[derive(Debug)]
pub(super) enum RollbackError {
    /// The receipt stands: the folder is an installation, which nothing takes back, and the
    /// rollback touched nothing.
    Receipted,
    /// A file operation failed where the rollback had come to.
    Files(FilesError),
}

impl From<FilesError> for RollbackError {
    fn from(error: FilesError) -> RollbackError {
        RollbackError::Files(error)
    }
}

impl Rollback<'_> {
    /// Takes the install back, in this order, passing [`FaultPoint::Rollback`] after each root
    /// moved back and after each of the next four steps:
    ///
    /// 1. when the journal records the publish, it is moved to rolling back, and each of its roots,
    ///    last first, that is in place and gone from the staging goes back into it, but a `saves`
    ///    or `config` folder that is not empty, which stays where it is;
    /// 2. the staging goes;
    /// 3. the list of the disc's files, and the pending files of it, of the receipt and of the
    ///    journal;
    /// 4. the journal;
    /// 5. the state folder, with its lock, when it holds nothing else;
    /// 6. once it went, the folders the install made, while they hold nothing, innermost first;
    /// 7. once it went and no root stayed, the record forgets the folder, a failure to forget
    ///    passed over.
    ///
    /// Anything at the receipt's name stops it before it touches anything, as
    /// [`RollbackError::Receipted`]: past the commit point nothing is taken back, and the journal
    /// is left for the next start to finish the install from. A failure to look stops it there too.
    ///
    /// A failure stops the rollback where it is, and is returned: what is left, its journal among
    /// it until the fourth step, is the next start's to take back, and the record keeps the folder.
    pub(super) fn run(self, record: &mut dyn FolderRecord) -> Result<TakenBack, RollbackError> {
        let Rollback {
            folder,
            made_folders,
            lock,
            begun,
            faults,
        } = self;
        let state = folder.join(STATE_DIR);
        if is_there(&state.join(RECEIPT_FILE))? {
            return Err(RollbackError::Receipted);
        }
        let journal_file = state.join(JOURNAL_DIR).join(INSTALL_JOURNAL);
        let mut kept = Vec::new();
        let mut staging_path = None;
        if let Some(Begun { staging, journal }) = begun {
            if let Some(mut journal) = journal
                && journal.phase() != InstallPhase::Staging
            {
                kept = take_back_published(folder, staging.as_ref(), &mut journal, faults)?;
            }
            if let Some(staging) = staging {
                staging_path = Some(staging.path().to_path_buf());
                remove_staging(staging)?;
            }
        }
        pass(faults, staging_path.as_deref().unwrap_or(&state))?;
        // No receipt stands, so the list of the disc's files is no installation's.
        for file in install_leftover_files(&state, false) {
            remove_if_there(&file)?;
        }
        pass(faults, &state)?;
        remove_if_there(&journal_file)?;
        pass(faults, &journal_file)?;
        let given_up = lock.give_up()?;
        pass(faults, &state)?;
        if given_up {
            remove_made_folders(folder, made_folders);
            if kept.is_empty() {
                let _ = record.forget(folder);
            }
        }
        Ok(TakenBack { kept, given_up })
    }
}

/// Moves the journal to rolling back, then takes each of its roots that the publish put in place
/// back from `folder` into `staging`, last first, passing [`FaultPoint::Rollback`] after each: a
/// root in place and gone from the staging moved, whether its move was recorded or not. A root
/// still in the staging, or in neither place, is passed over, and so is a `saves` or `config`
/// folder in place that is not empty, which is given back as kept. Without the staging nothing in
/// place can go back, so every root in place is kept. Gives the roots kept.
fn take_back_published(
    folder: &Path,
    staging: Option<&Staging>,
    journal: &mut JournalFile<'_>,
    faults: &dyn FaultPoints,
) -> Result<Vec<String>, FilesError> {
    if journal.phase() != InstallPhase::RollingBack {
        journal.rolling_back()?;
    }
    let mut kept = Vec::new();
    for name in journal.roots().iter().rev() {
        let target = folder.join(name);
        if !is_there(&target)? {
            continue;
        }
        let Some(staging) = staging else {
            kept.push(name.clone());
            continue;
        };
        let staged = staging.path().join(name);
        if is_there(&staged)? {
            continue;
        }
        if holds_players_files(name, &target)? {
            kept.push(name.clone());
            continue;
        }
        let root = RelPath::parse(name, &PAYLOAD_NAMES).map_err(|refused| FilesError::Io {
            path: target.clone(),
            source: io::Error::new(io::ErrorKind::InvalidData, refused),
        })?;
        staging.unpublish(&root, folder)?;
        pass(faults, &staged)?;
    }
    Ok(kept)
}

/// Whether the root `name`, in place at `target`, is one the game keeps the player's files in and
/// holds anything but an empty folder of the install's: what a player may have put there since,
/// which no rollback takes.
fn holds_players_files(name: &str, target: &Path) -> Result<bool, FilesError> {
    if name != SAVES_FOLDER && name != CONFIG_FOLDER {
        return Ok(false);
    }
    let io = |source| FilesError::Io {
        path: target.to_path_buf(),
        source,
    };
    let metadata = fs::symlink_metadata(target).map_err(io)?;
    if !metadata.is_dir() {
        return Ok(true);
    }
    Ok(!names_in(target).map_err(io)?.is_empty())
}

/// The roots of `roots` in place in `folder` that a rollback without the staging they came from
/// could not take back: every one but a `saves` or `config` folder that holds the player's files,
/// which stays in place whatever the rollback has to take back.
pub(super) fn stranded_roots(folder: &Path, roots: &[String]) -> Result<Vec<String>, FilesError> {
    let mut stranded = Vec::new();
    for name in roots {
        let target = folder.join(name);
        if is_there(&target)? && !holds_players_files(name, &target)? {
            stranded.push(name.clone());
        }
    }
    Ok(stranded)
}

/// Whether anything is at `path`, looked at without following a link.
pub(super) fn is_there(path: &Path) -> Result<bool, FilesError> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(true),
        Err(error) if is_absent(&error) => Ok(false),
        Err(source) => Err(FilesError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// Writes `receipt` as the fresh install's in `folder`, where none stood before it or the same one
/// did, and succeeds whenever it is then in place: the commit point, past which the folder is an
/// installation that nothing takes back. A write that succeeded renamed it there; one that failed
/// may have, in putting the rename on disk after it was made, as a file at its name then shows. A
/// write that failed with no receipt in place is its failure.
pub(super) fn commit_receipt(receipt: &Receipt, folder: &InstallFolder) -> Result<(), FilesError> {
    let written = receipt.write(folder);
    if in_place(&written, &folder.state_dir().join(RECEIPT_FILE)) {
        Ok(())
    } else {
        written
    }
}

/// Whether a receipt whose write at `path` gave `written` is in place: after a write that
/// succeeded always, with no look, which could fail where the write did not; after one that
/// failed, when a file is at `path`.
fn in_place(written: &Result<(), FilesError>, path: &Path) -> bool {
    written.is_ok() || fs::symlink_metadata(path).is_ok_and(|metadata| metadata.is_file())
}

/// Removes `staging` with whatever it holds, counting a staging already gone as removed.
fn remove_staging(staging: Staging) -> Result<(), FilesError> {
    match staging.remove() {
        Err(FilesError::Io { source, .. }) if is_absent(&source) => Ok(()),
        removed => removed,
    }
}

/// Passes [`FaultPoint::Rollback`], a failure there reported as one at `at`, what the step before
/// it took away.
fn pass(faults: &dyn FaultPoints, at: &Path) -> Result<(), FilesError> {
    faults
        .pass(FaultPoint::Rollback)
        .map_err(|source| FilesError::Io {
            path: at.to_path_buf(),
            source,
        })
}

/// Removes the folders an install made, `made` of them from `folder` up, innermost first, each only
/// while it is a real folder that holds nothing. The first that stays, or that the system will
/// not remove, keeps every folder above it, which holds it; what stays is left as it is.
pub(super) fn remove_made_folders(folder: &Path, made: u32) {
    let made = usize::try_from(made).unwrap_or(usize::MAX);
    for made_folder in folder.ancestors().take(made) {
        let removed = match fs::symlink_metadata(made_folder) {
            Ok(metadata) if metadata.is_dir() => {
                match retry_locked(|| fs::remove_dir(made_folder)) {
                    Ok(()) => true,
                    Err(error) => is_absent(&error),
                }
            }
            Ok(_) => false,
            Err(error) => is_absent(&error),
        };
        if !removed {
            break;
        }
    }
}

/// Removes the state folder `state`, which an install made but whose lock it does not hold, only
/// while it holds nothing, and says whether it is gone: a state folder already gone counts as
/// removed. A lock file in it, another job's or one no job holds, keeps it, since no job removes a
/// lock file whose lock it does not hold; so does a link on its path, which is never removed
/// through.
pub(super) fn remove_unlocked_state(state: &Path) -> bool {
    if no_links(state).is_err() {
        return false;
    }
    match retry_locked(|| fs::remove_dir(state)) {
        Ok(()) => true,
        Err(error) => is_absent(&error),
    }
}

/// Cleans up once the journal records the install committed, passing [`FaultPoint::Cleanup`]
/// before each step: the staging goes, then the journal, last, so that whichever step a crash
/// cuts short, the journal is left for the next start to finish from, or nothing is. A staging or
/// a journal already gone counts as removed, and so does a staging given as `None`, one the
/// journal names that a cleanup cut short has removed already.
pub(super) fn clean_up(
    staging: Option<Staging>,
    journal: &JournalFile<'_>,
    faults: &dyn FaultPoints,
) -> Result<(), FilesError> {
    let cleaned = |at: &Path| {
        faults
            .pass(FaultPoint::Cleanup)
            .map_err(|source| FilesError::Io {
                path: at.to_path_buf(),
                source,
            })
    };
    cleaned(journal.path())?;
    let removed = match staging {
        Some(staging) => {
            let removed = staging.path().to_path_buf();
            remove_staging(staging)?;
            removed
        }
        None => journal.path().to_path_buf(),
    };
    cleaned(&removed)?;
    journal.remove()
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;

    use gdl_files::FilesError;
    use gdl_fixtures::folder_names;

    use super::{TakenBack, in_place, remove_made_folders, remove_unlocked_state, stranded_roots};
    use crate::folder::RECEIPT_FILE;

    #[test]
    fn a_receipt_is_in_place_once_written_or_left_by_a_write_that_failed() {
        let state = tempfile::tempdir().unwrap();
        let path = state.path().join(RECEIPT_FILE);
        let failed = || {
            Err(FilesError::Io {
                path: path.clone(),
                source: io::Error::other("the folder's sync failed"),
            })
        };
        // A write that succeeded renamed the receipt into place, which is the commit point,
        // whatever a look at its name would find: here nothing.
        assert!(in_place(&Ok(()), &path));
        // A write that failed put it in place only when a file stands at its name.
        assert!(!in_place(&failed(), &path));
        fs::create_dir(&path).unwrap();
        assert!(!in_place(&failed(), &path));
        fs::remove_dir(&path).unwrap();
        fs::write(&path, b"{}\n").unwrap();
        assert!(in_place(&failed(), &path));
    }

    #[test]
    fn made_folders_go_innermost_first_while_they_hold_nothing() {
        let top = tempfile::tempdir().unwrap();
        let folder = top.path().join("a").join("b").join("c");
        fs::create_dir_all(&folder).unwrap();
        // Two of the three made: the top one was there before.
        remove_made_folders(&folder, 2);
        assert_eq!(folder_names(top.path()).unwrap(), ["a"]);
        assert!(folder_names(&top.path().join("a")).unwrap().is_empty());

        // A folder that holds something stays, and so does every one above it.
        let folder = top.path().join("x").join("y");
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join("theirs.txt"), b"a stranger's").unwrap();
        remove_made_folders(&folder, 2);
        assert_eq!(folder_names(&folder).unwrap(), ["theirs.txt"]);

        // A folder already gone counts as removed, and none is made counts as nothing to do.
        let gone = top.path().join("x").join("gone");
        remove_made_folders(&gone, 1);
        remove_made_folders(&folder, 0);
        assert_eq!(folder_names(&folder).unwrap(), ["theirs.txt"]);
    }

    #[test]
    fn a_state_folder_without_its_lock_goes_only_while_it_holds_nothing() {
        let top = tempfile::tempdir().unwrap();
        let state = top.path().join(".gdl-launcher");
        fs::create_dir(&state).unwrap();
        fs::write(state.join("lock"), b"").unwrap();
        assert!(!remove_unlocked_state(&state));
        assert_eq!(folder_names(&state).unwrap(), ["lock"]);
        fs::remove_file(state.join("lock")).unwrap();
        assert!(remove_unlocked_state(&state));
        assert!(!state.exists());
        // Already gone.
        assert!(remove_unlocked_state(&state));
    }

    #[test]
    fn roots_in_place_are_stranded_but_the_players_folders() {
        let folder = tempfile::tempdir().unwrap();
        let roots: Vec<String> = ["Gauntlet", "VERSION", "config", "data", "saves"]
            .map(str::to_owned)
            .to_vec();
        assert!(stranded_roots(folder.path(), &roots).unwrap().is_empty());
        fs::create_dir(folder.path().join("Gauntlet")).unwrap();
        fs::write(folder.path().join("VERSION"), b"1.0.0-beta").unwrap();
        // An empty saves folder is the install's; one the player saved into, and a file in the
        // settings folder's place, are the player's.
        fs::create_dir(folder.path().join("saves")).unwrap();
        fs::write(folder.path().join("config"), b"the player's").unwrap();
        assert_eq!(
            stranded_roots(folder.path(), &roots).unwrap(),
            ["Gauntlet", "VERSION", "saves"]
        );
        fs::write(folder.path().join("saves").join("slot1.json"), b"{}").unwrap();
        assert_eq!(
            stranded_roots(folder.path(), &roots).unwrap(),
            ["Gauntlet", "VERSION"]
        );
    }

    #[test]
    fn a_rollback_is_whole_only_when_nothing_stays() {
        let whole = TakenBack {
            kept: Vec::new(),
            given_up: true,
        };
        assert!(whole.is_whole());
        let saves = TakenBack {
            kept: vec!["saves".to_owned()],
            ..whole.clone()
        };
        assert!(!saves.is_whole());
        let state = TakenBack {
            given_up: false,
            ..whole
        };
        assert!(!state.is_whole());
    }
}
