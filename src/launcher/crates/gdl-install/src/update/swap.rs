//! An update's moves, its commit and its cleanup, under the installation's lock and never
//! cancelled: every move's folders checked for links again, the journal moved to swapping with
//! the folders the moves make, then each installed name that changes or goes moved aside into
//! the backup, each new one moved into place, the installed launcher copy retired and the new one
//! put in its place, each by a rename that never replaces anything; then the receipt written, the
//! commit point, judged by what it left when its write fails; then the journal moved to committed,
//! and what was staged and moved aside removed, the journal last. Before the swap an update is
//! taken back by removing what it staged, since nothing has moved.

use std::fs;
use std::path::Path;

use gdl_files::{
    FilesError, RelPath, Staging, is_absent, no_links, pending_path, read_json_bounded,
    rename_noreplace, retry_locked,
};

use super::journal::{JournalFile, UPDATE_BACKUP, remove_if_there};
use super::stage::Staged;
use super::{
    CLEANUP, RetiredCopy, SWAP, Update, UpdateError, UpdateFailure, UpdateNotice, UpdateOutcome,
    Updated, files_error,
};
use crate::faults::{FaultPoint, FaultPoints};
use crate::folder::{InstallFolder, InstallLock, RECEIPT_FILE};
use crate::install::Changes;
use crate::receipt::{MAX_RECEIPT_LEN, Receipt, installed_launcher_name};

impl Update<'_> {
    /// Swaps the staged release in, writes its receipt and cleans up, as the module says, and
    /// gives what the update did. Before the journal moves to swapping a failure takes back what
    /// was staged; from then on it keeps everything, with the journal, for the next start; and
    /// past the receipt it is a notice.
    pub(super) fn swap(
        &self,
        staged: Staged,
        lock: InstallLock,
    ) -> Result<UpdateOutcome, UpdateFailure> {
        self.job.progress.phase(SWAP);
        let Staged {
            staging,
            mut journal,
            changes,
            to,
            receipt,
            moves_executable,
            launcher_missing,
            probed,
            mut notices,
            tally,
        } = staged;
        let backup = changes.backup();
        let publish = changes.publish();
        // The last look at the cancel: from the journal's swapping on, the update goes through.
        let made = self
            .job
            .cancel
            .check()
            .map_err(UpdateError::from)
            .and_then(|()| self.links_again(&backup, &publish))
            .and_then(|()| folders_to_make(self.folder.root(), &publish));
        let made = match made {
            Ok(made) => made,
            Err(error) => return Err(self.taken_back(error, staging, Some(journal))),
        };
        let swapped = journal
            .swapping(made.clone())
            .and_then(|()| self.pass(FaultPoint::UpdateCommit, journal.path()))
            .and_then(|()| self.moves(&staging, &journal, &backup, &publish, &made))
            .and_then(|()| self.commit(&receipt));
        if let Err(error) = swapped {
            // Taking back what moved is the next start's, from the journal.
            return Err(UpdateFailure {
                error,
                changes: Changes::Kept,
            });
        }
        let retired_name = journal.journal().retired.clone();
        let committed = self
            .pass(FaultPoint::UpdateCommit, journal.path())
            .and_then(|()| journal.committed())
            .and_then(|()| self.pass(FaultPoint::UpdateCommit, journal.path()));
        let retired = match committed {
            Ok(()) => {
                self.job.progress.phase(CLEANUP);
                let (retired, cleaned) =
                    self.clean_up(staging, &journal, launcher_missing, &retired_name);
                if let Err(error) = cleaned {
                    notices.push(cleanup_left(&error));
                }
                retired
            }
            Err(error) => {
                notices.push(cleanup_left(&error));
                left_retired(launcher_missing, retired_name)
            }
        };
        if moves_executable {
            (self.settings.notify_shell)(&self.folder.root().join(&self.installed.executable));
        }
        self.job.progress.advance(tally.total, tally.total, None);
        drop(lock);
        Ok(UpdateOutcome::Updated(Box::new(Updated {
            folder: self.folder.clone(),
            from: self.installed.version(),
            to,
            receipt,
            changes,
            retired,
            probed,
            notices,
        })))
    }

    /// Checks again, just before the swap, that no link is on the way to a name it moves: every
    /// installed name moved aside, every new name's place up to the installation's top, and the
    /// launcher copy's.
    fn links_again(&self, backup: &[RelPath], publish: &[RelPath]) -> Result<(), UpdateError> {
        let root = self.folder.root();
        let launcher = root.join(installed_launcher_name(self.platform));
        let places = backup
            .iter()
            .chain(publish)
            .map(|name| name.path_under(root))
            .chain([launcher]);
        for place in places {
            no_links(&place).map_err(files_error)?;
        }
        Ok(())
    }

    /// Moves, in order and each passed as it is made: each installed name of `backup` aside into
    /// the backup, its folders there made as needed; the folders `made` the new names need; each
    /// new name of `publish` from the staging into place; the installed launcher copy aside under
    /// its retired name, when there is one; and the new copy into its place.
    fn moves(
        &self,
        staging: &Staging,
        journal: &JournalFile,
        backup: &[RelPath],
        publish: &[RelPath],
        made: &[String],
    ) -> Result<(), UpdateError> {
        let root = self.folder.root();
        let state = self.folder.state_dir();
        let aside = state.join(UPDATE_BACKUP);
        for name in backup {
            let to = name.path_under(&aside);
            if let Some(folder) = to.parent() {
                // Never made through a link, which would lead the folders outside the backup.
                no_links(folder).map_err(files_error)?;
                fs::create_dir_all(folder).map_err(|source| UpdateError::Io {
                    path: folder.to_path_buf(),
                    source,
                })?;
            }
            checked_move(&name.path_under(root), &to)?;
            self.pass(FaultPoint::UpdateMove, &to)?;
        }
        for folder in made {
            make_folder(
                &folder
                    .split('/')
                    .fold(root.to_path_buf(), |path, part| path.join(part)),
            )?;
        }
        for name in publish {
            let to = name.path_under(root);
            checked_move(&name.path_under(staging.path()), &to).map_err(|error| match error {
                UpdateError::Io { source, .. }
                    if source.kind() == std::io::ErrorKind::AlreadyExists =>
                {
                    UpdateError::Changed {
                        name: name.as_str().into(),
                    }
                }
                other => other,
            })?;
            self.pass(FaultPoint::UpdateMove, &to)?;
        }
        let launcher = installed_launcher_name(self.platform);
        let installed = root.join(launcher);
        let retired = state.join(&journal.journal().retired);
        if !journal.journal().launcher_missing {
            checked_move(&installed, &retired)?;
        }
        self.pass(FaultPoint::LauncherReplace, &retired)?;
        checked_move(&staging.path().join(launcher), &installed)?;
        self.pass(FaultPoint::LauncherReplace, &installed)
    }

    /// Writes `receipt` as the installation's own: the commit point. A write that failed is the
    /// commit too when it left the new receipt in place, as one can after its rename, in putting
    /// the rename on disk; otherwise it is the update's failure.
    fn commit(&self, receipt: &Receipt) -> Result<(), UpdateError> {
        match (self.settings.write_receipt)(receipt, &self.folder) {
            Ok(()) => Ok(()),
            Err(_) if receipt_committed(receipt, &self.folder) => Ok(()),
            Err(error) => Err(files_error(error)),
        }
    }

    /// Cleans up once the journal records the update committed, passing
    /// [`FaultPoint::UpdateCleanup`] after each step: the staging goes, then the backup, then the
    /// retired launcher copy is tried once, outside the retries a scanner's hold gets, since on
    /// Windows a launcher still running from it holds it; then the journal, its pending file and
    /// the receipt's. A failure among the first two stops the cleanup with the journal kept for the
    /// next start; a retired copy that stays is only told.
    fn clean_up(
        &self,
        staging: Staging,
        journal: &JournalFile,
        launcher_missing: bool,
        retired_name: &str,
    ) -> (RetiredCopy, Result<(), UpdateError>) {
        let state = self.folder.state_dir();
        let aside = state.join(UPDATE_BACKUP);
        let staging_path = staging.path().to_path_buf();
        let removed = remove_staging(staging)
            .map_err(files_error)
            .and_then(|()| self.pass(FaultPoint::UpdateCleanup, &staging_path))
            .and_then(|()| remove_folder(&aside).map_err(files_error))
            .and_then(|()| self.pass(FaultPoint::UpdateCleanup, &aside));
        if let Err(error) = removed {
            return (
                left_retired(launcher_missing, retired_name.to_owned()),
                Err(error),
            );
        }
        let retired_path = state.join(retired_name);
        let retired = if launcher_missing {
            RetiredCopy::NotMoved
        } else {
            match fs::remove_file(&retired_path) {
                Ok(()) => RetiredCopy::Removed,
                Err(error) if is_absent(&error) => RetiredCopy::Removed,
                Err(_) => RetiredCopy::Left {
                    name: retired_name.to_owned(),
                },
            }
        };
        let finished = self
            .pass(FaultPoint::UpdateCleanup, &retired_path)
            .and_then(|()| {
                journal
                    .remove()
                    .and_then(|()| remove_if_there(&pending_path(journal.path())))
                    .and_then(|()| remove_if_there(&pending_path(&state.join(RECEIPT_FILE))))
                    .map_err(files_error)
            });
        (retired, finished)
    }
}

/// Takes back an update whose journal, when it was written, still records it staged: nothing has
/// moved, so the staging goes with what it holds, then the journal's pending file and the journal
/// itself, last, passing [`FaultPoint::UpdateRollback`] after each step once the journal was
/// written. A staging or a journal already gone counts as removed.
pub(super) fn discard(
    staging: Staging,
    journal: Option<&JournalFile>,
    faults: &dyn FaultPoints,
) -> Result<(), FilesError> {
    let path = staging.path().to_path_buf();
    remove_staging(staging)?;
    let Some(journal) = journal else {
        return Ok(());
    };
    pass_rollback(faults, &path)?;
    remove_if_there(&pending_path(journal.path()))?;
    journal.remove()?;
    pass_rollback(faults, journal.path())
}

/// Passes [`FaultPoint::UpdateRollback`], a failure there reported as one at `at`.
fn pass_rollback(faults: &dyn FaultPoints, at: &Path) -> Result<(), FilesError> {
    faults
        .pass(FaultPoint::UpdateRollback)
        .map_err(|source| FilesError::Io {
            path: at.to_path_buf(),
            source,
        })
}

/// Removes `staging` with whatever it holds, counting a staging already gone as removed.
fn remove_staging(staging: Staging) -> Result<(), FilesError> {
    match staging.remove() {
        Err(FilesError::Io { source, .. }) if is_absent(&source) => Ok(()),
        removed => removed,
    }
}

/// Removes the folder `path` with whatever it holds, a link in it as the link alone, counting one
/// already gone as removed. A removal a scanner's hold refuses for a moment is tried again.
fn remove_folder(path: &Path) -> Result<(), FilesError> {
    match retry_locked(|| fs::remove_dir_all(path)) {
        Ok(()) => Ok(()),
        Err(error) if is_absent(&error) => Ok(()),
        Err(source) => Err(FilesError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// Makes the folder `path`, which a new name needs and which was not there as the swap began; a
/// folder found there now is taken as it is.
fn make_folder(path: &Path) -> Result<(), UpdateError> {
    match fs::create_dir(path) {
        Ok(()) => Ok(()),
        Err(_) if fs::symlink_metadata(path).is_ok_and(|entry| entry.is_dir()) => Ok(()),
        Err(source) => Err(UpdateError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// Renames `from` to `to` by a rename that never replaces what is at `to`, once neither `from`,
/// nor a folder above it, nor a folder above `to`, is a link.
fn checked_move(from: &Path, to: &Path) -> Result<(), UpdateError> {
    no_links(from).map_err(files_error)?;
    if let Some(folder) = to.parent() {
        no_links(folder).map_err(files_error)?;
    }
    rename_noreplace(from, to).map_err(|source| UpdateError::Io {
        path: to.to_path_buf(),
        source,
    })
}

/// The folders under the installation's top `root` that the names of `publish` need and that are
/// not there, each once, outermost first.
fn folders_to_make(root: &Path, publish: &[RelPath]) -> Result<Vec<String>, UpdateError> {
    let mut made: Vec<String> = Vec::new();
    for name in publish {
        let components: Vec<&str> = name.components().collect();
        let mut folder = String::new();
        let mut path = root.to_path_buf();
        for component in &components[..components.len() - 1] {
            if !folder.is_empty() {
                folder.push('/');
            }
            folder.push_str(component);
            path.push(component);
            if made.contains(&folder) {
                continue;
            }
            match fs::symlink_metadata(&path) {
                Ok(_) => {}
                Err(error) if is_absent(&error) => made.push(folder.clone()),
                Err(source) => return Err(UpdateError::Io { path, source }),
            }
        }
    }
    Ok(made)
}

/// What became of a retired launcher copy that no cleanup tried to remove: none was moved aside
/// when none was there, and otherwise it stays under `name`.
fn left_retired(launcher_missing: bool, name: String) -> RetiredCopy {
    if launcher_missing {
        RetiredCopy::NotMoved
    } else {
        RetiredCopy::Left { name }
    }
}

/// The notice of a cleanup that `error` stopped.
fn cleanup_left(error: &UpdateError) -> UpdateNotice {
    UpdateNotice::CleanupLeft {
        detail: error.to_string().into(),
    }
}

/// Whether the receipt `new`, whose write failed, is in place as `folder`'s receipt: read back,
/// once a file is seen at its name, as the installation's receipt is read, and the same receipt.
fn receipt_committed(new: &Receipt, folder: &InstallFolder) -> bool {
    let path = folder.state_dir().join(RECEIPT_FILE);
    if !fs::symlink_metadata(&path).is_ok_and(|entry| entry.is_file()) {
        return false;
    }
    committed_by(read_json_bounded(&path, MAX_RECEIPT_LEN), new)
}

/// Whether a receipt write that failed committed `new`, as what was read back at the receipt's
/// name, `read`, shows: only the new receipt itself is the commit; the old one, none and one that
/// cannot be read are not.
fn committed_by(read: Result<Receipt, FilesError>, new: &Receipt) -> bool {
    read.is_ok_and(|read| read == *new)
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;

    use gdl_files::{FilesError, PAYLOAD_NAMES, RelPath, retry_locked};
    use gdl_fixtures::{InstallFixture, RuntimeFiles};

    use super::{committed_by, folders_to_make, receipt_committed};
    use crate::folder::{InstallFolder, RECEIPT_FILE, STATE_DIR};
    use crate::receipt::Receipt;
    use crate::test_platform::this_platform;

    /// An installation of the standard runtime, with its receipt as the launcher reads it.
    fn installation() -> (tempfile::TempDir, InstallFixture, Receipt) {
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(this_platform().as_str(), "1.0.0-beta");
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        let receipt = serde_json::from_value(fixture.receipt()).unwrap();
        (dir, fixture, receipt)
    }

    #[test]
    fn a_failed_write_is_judged_by_what_it_left() {
        let (_dir, fixture, old) = installation();
        let mut new = old.clone();
        new.version = "1.0.0-beta.1".to_owned();
        let path = fixture.root.join(STATE_DIR).join(RECEIPT_FILE);

        // As read back: the old receipt, none, one that cannot be read, and the new one.
        assert!(!committed_by(Ok(old.clone()), &new));
        let gone = FilesError::Io {
            path: path.clone(),
            source: io::Error::from(io::ErrorKind::NotFound),
        };
        assert!(!committed_by(Err(gone), &new));
        let unreadable = FilesError::Json {
            path: path.clone(),
            source: serde_json::from_slice::<Receipt>(b"{").unwrap_err(),
        };
        assert!(!committed_by(Err(unreadable), &new));
        assert!(committed_by(Ok(new.clone()), &new));

        // And at the receipt's place in the folder.
        let folder = InstallFolder::at(&fixture.root).unwrap();
        assert!(!receipt_committed(&new, &folder));
        new.write(&folder).unwrap();
        assert!(receipt_committed(&new, &folder));
        assert!(!receipt_committed(&old, &folder));
        retry_locked(|| fs::remove_file(&path)).unwrap();
        assert!(!receipt_committed(&new, &folder));
        fs::write(&path, b"{").unwrap();
        assert!(!receipt_committed(&new, &folder));
        retry_locked(|| fs::remove_file(&path)).unwrap();
        fs::create_dir(&path).unwrap();
        assert!(!receipt_committed(&new, &folder));
    }

    #[test]
    fn the_folders_a_publish_needs_are_measured_outermost_first() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        fs::create_dir_all(root.join("shaders")).unwrap();
        let publish: Vec<RelPath> = [
            "VERSION",
            "data/text/de.json",
            "data/text/en.json",
            "licenses/a/b.txt",
            "shaders/new.spv",
        ]
        .iter()
        .map(|text| RelPath::parse(text, &PAYLOAD_NAMES).unwrap())
        .collect();
        assert_eq!(
            folders_to_make(root, &publish).unwrap(),
            ["data", "data/text", "licenses", "licenses/a"]
        );
        // Nothing is made in measuring.
        assert!(!root.join("data").exists());
    }
}
