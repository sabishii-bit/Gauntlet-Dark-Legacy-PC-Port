//! An update's moves, its commit and its cleanup, under the installation's lock and never
//! cancelled: every move's folders checked for links again, the journal moved to swapping with
//! the folders the moves make, then each installed name that changes or goes moved aside into
//! the backup, each new one moved into place, the installed launcher copy retired and the new one
//! put in its place, each by a rename that never replaces anything; then the receipt written, the
//! commit point, judged by what it left when its write fails; then the journal moved to committed,
//! and what was staged and moved aside removed, the journal last.
//!
//! Before the swap an update is taken back by removing what it staged, since nothing has moved.
//! From the swap on, until the receipt stands, it is taken back from its journal alone, from
//! wherever it stopped: the journal moved to rolling back, every move undone in the reverse order,
//! each name's place telling whether its move was made, then what the update left in the state
//! folder removed, the journal last. The next start's recovery takes an update back, and cleans
//! one up, the same way.

use std::collections::BTreeSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{
    FilesError, PAYLOAD_NAMES, RelPath, Staging, is_absent, no_links, pending_path,
    read_json_bounded, rename_noreplace, retry_locked,
};
use gdl_release::Platform;

use super::journal::{
    DOWNLOAD_PREFIX, JournalFile, UPDATE_BACKUP, UPDATE_STAGING_PREFIX, UpdatePhase,
    remove_if_there, retired_name,
};
use super::settings::WriteReceiptFn;
use super::stage::Staged;
use super::{
    CLEANUP, RetiredCopy, SWAP, Update, UpdateError, UpdateFailure, UpdateNotice, UpdateOutcome,
    Updated, files_error,
};
use crate::faults::{FaultPoint, FaultPoints};
use crate::folder::{InstallFolder, InstallLock, RECEIPT_FILE};
use crate::install::Changes;
use crate::receipt::{MAX_RECEIPT_LEN, Receipt, installed_launcher_name};
use crate::recover::Recovered;

impl Update<'_> {
    /// Swaps the staged release in, writes its receipt and cleans up, as the module says, and
    /// gives what the update did, with what its recovery did first, `recovered`. A failure before
    /// the journal records the swap takes back what was staged; from then on until the receipt
    /// stands, every move made is taken back from the journal; and past the receipt a failure is a
    /// notice.
    pub(super) fn swap(
        &self,
        staged: Staged,
        lock: InstallLock,
        recovered: Recovered,
    ) -> Result<UpdateOutcome, UpdateFailure> {
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
        // The last look at the cancel, before the swap is told: from then on the update goes
        // through.
        if let Err(cancelled) = self.job.cancel.check() {
            return Err(self.taken_back(cancelled.into(), staging, Some(journal)));
        }
        self.job.progress.phase(SWAP);
        let backup = changes.backup();
        let publish = changes.publish();
        let made = self
            .links_again(&backup, &publish)
            .and_then(|()| folders_to_make(self.folder.root(), &publish));
        let made = match made {
            Ok(made) => made,
            Err(error) => return Err(self.taken_back(error, staging, Some(journal))),
        };
        // A swap the journal could not record has moved nothing, so what was staged goes as it
        // would have before the swap.
        if let Err(error) = journal.swapping(made.clone()) {
            return Err(self.taken_back(error, staging, Some(journal)));
        }
        let swapped = self
            .pass(FaultPoint::UpdateCommit, journal.path())
            .and_then(|()| self.moves(&staging, &journal, &backup, &publish, &made))
            .and_then(|()| self.commit(&receipt));
        if let Err(error) = swapped {
            // The staging is kept, and the rollback takes it back by the name the journal gives.
            return Err(self.rolled_back(error, &mut journal));
        }
        let retired_name = journal.journal().retired.clone();
        let committed = self
            .pass(FaultPoint::UpdateCommit, journal.path())
            .and_then(|()| journal.committed())
            .and_then(|()| self.pass(FaultPoint::UpdateCommit, journal.path()));
        // Kept: the cleanup removes it by the name the journal gives.
        drop(staging);
        let retired = match committed {
            Ok(()) => {
                self.job.progress.phase(CLEANUP);
                let (retired, cleaned) = clean_up(&self.folder, &journal, self.settings.faults);
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
            recovered,
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

    /// Writes `receipt` as the installation's own: the commit point, as [`commit_receipt`] writes
    /// it.
    fn commit(&self, receipt: &Receipt) -> Result<(), UpdateError> {
        commit_receipt(receipt, &self.folder, self.settings.write_receipt)
    }

    /// The update's failure for `error` once the journal recorded the swap and before the
    /// receipt stood: every move made is taken back from `journal`, and the installation is as it
    /// was. A rollback that stops, or that keeps a folder the swap made because something was put
    /// in it since, leaves what it has not taken back, its journal among it while that names
    /// anything, for the next start; the failure reported is the update's own.
    fn rolled_back(&self, error: UpdateError, journal: &mut JournalFile) -> UpdateFailure {
        let changes = match roll_back(&self.folder, journal, self.platform, self.settings.faults) {
            Ok(taken) if taken.kept.is_empty() => Changes::RolledBack,
            Ok(_) | Err(_) => Changes::Kept,
        };
        UpdateFailure { error, changes }
    }
}

/// Cleans up the update of the installation `folder` whose journal `journal` records it
/// committed, passing [`FaultPoint::UpdateCleanup`] at `faults` after each step: the staging the
/// journal names goes, then the backup, then the download folder when it names one, then the
/// retired launcher copy is tried once, outside the retries a scanner's hold gets, since on
/// Windows a launcher still running from it holds it; then the journal, its pending file and the
/// receipt's. A failure among the first three stops the cleanup with the journal kept for the
/// next start; a retired copy that stays is only told. Gives what became of the retired copy, and
/// how the cleanup ended.
pub(super) fn clean_up(
    folder: &InstallFolder,
    journal: &JournalFile,
    faults: &dyn FaultPoints,
) -> (RetiredCopy, Result<(), UpdateError>) {
    let written = journal.journal();
    let state = folder.state_dir();
    let aside = state.join(UPDATE_BACKUP);
    let pass = |at: &Path| pass_cleanup(faults, at);
    let mut removed = remove_named(&state, UPDATE_STAGING_PREFIX, &written.staging)
        .map_err(files_error)
        .and_then(|()| pass(&state.join(&written.staging)))
        .and_then(|()| remove_folder(&aside).map_err(files_error))
        .and_then(|()| pass(&aside));
    if let Some(download) = &written.download {
        removed = removed
            .and_then(|()| remove_named(&state, DOWNLOAD_PREFIX, download).map_err(files_error))
            .and_then(|()| pass(&state.join(download)));
    }
    if let Err(error) = removed {
        return (
            left_retired(written.launcher_missing, written.retired.clone()),
            Err(error),
        );
    }
    let retired_path = state.join(&written.retired);
    let retired = if written.launcher_missing {
        RetiredCopy::NotMoved
    } else {
        match fs::remove_file(&retired_path) {
            Ok(()) => RetiredCopy::Removed,
            Err(error) if is_absent(&error) => RetiredCopy::Removed,
            Err(_) => RetiredCopy::Left {
                name: written.retired.clone(),
            },
        }
    };
    let finished = pass(&retired_path).and_then(|()| {
        journal
            .remove()
            .and_then(|()| remove_if_there(&pending_path(journal.path())))
            .and_then(|()| remove_if_there(&pending_path(&state.join(RECEIPT_FILE))))
            .map_err(files_error)
    });
    (retired, finished)
}

/// Writes `receipt` as the installation `folder`'s own, by `write`: the commit point. A write
/// that failed is the commit too when it left the new receipt in place, as one can after its
/// rename, in putting the rename on disk; otherwise it is the update's failure.
pub(super) fn commit_receipt(
    receipt: &Receipt,
    folder: &InstallFolder,
    write: &WriteReceiptFn<'_>,
) -> Result<(), UpdateError> {
    match write(receipt, folder) {
        Ok(()) => Ok(()),
        Err(_) if receipt_committed(receipt, folder) => Ok(()),
        Err(error) => Err(files_error(error)),
    }
}

/// Passes [`FaultPoint::UpdateCleanup`] at `faults`, a failure there reported as one at `at`, what
/// the step before it removed.
fn pass_cleanup(faults: &dyn FaultPoints, at: &Path) -> Result<(), UpdateError> {
    faults
        .pass(FaultPoint::UpdateCleanup)
        .map_err(|source| UpdateError::Io {
            path: at.to_path_buf(),
            source,
        })
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

/// What taking an update back did, and left of it in the installation.
#[derive(Debug)]
pub(super) struct TakenBack {
    /// The folders the swap made for new names that stay where they are, as something was put in
    /// them since, or something other than a folder stands in their place.
    pub(super) kept: Vec<String>,
    /// Whether the installed launcher copy was put back in place from its retired name, so that
    /// the file at the installation's top is another than before the rollback.
    pub(super) launcher_restored: bool,
    /// The new launcher copy's retired name, in the state folder, when it was moved aside and its
    /// one removal failed, as a launcher running from it holds it on Windows.
    pub(super) left: Option<String>,
}

/// Takes back the update whose journal is `journal`, in the installation `folder` for
/// `platform`, from wherever it stopped, under the installation's lock: the journal is moved to
/// rolling back, then, each move made only once neither its source, nor a folder above it, nor a
/// folder above its target is a link, and every step counting a name already where it belongs as
/// done:
///
/// 1. the launcher copy: a new one in place, the staging's copy gone, goes aside under a retired
///    name of its own, its version's, so that the staging never holds a program that may run;
///    then the installed one comes back from its retired name. Without an installed one, the new
///    one goes aside alone;
/// 2. each new name the swap put in place, last first, back into the staging: one in place and
///    gone from the staging, and, for one that replaced an installed name, only while the backup
///    holds that one, since otherwise what is in place is the installed file;
/// 3. each folder the swap made, innermost first, while it is a folder that holds nothing;
/// 4. each installed name moved aside, last first, back into place;
/// 5. the staging, with what it holds; the backup, while it holds nothing but folders; the
///    download folder, when there is one; the new copy's retired file, tried once, outside the
///    retries a scanner's hold gets, since a launcher running from it holds it on Windows, which
///    leaves it for the next start; the pending files of the journal and of the receipt; and the
///    journal, last.
///
/// [`FaultPoint::UpdateRollback`] is passed after each move, once after the folders, and after
/// each of the last step's removals but the pending files'. A name in neither of its places, or
/// in both where only one may be, a file or a link left in the backup, or a link on a move's way
/// stops it as [`UpdateError::Inconsistent`]; whatever stops it, what is left, its journal among
/// it, is the next start's to take back. The receipt is never touched: the installed one stands.
pub(super) fn roll_back(
    folder: &InstallFolder,
    journal: &mut JournalFile,
    platform: Platform,
    faults: &dyn FaultPoints,
) -> Result<TakenBack, UpdateError> {
    if journal.journal().phase != UpdatePhase::RollingBack {
        journal.rolling_back()?;
    }
    let taking = TakingBack::of(folder, journal, platform, faults)?;
    let launcher_restored = taking.launcher()?;
    taking.published()?;
    let kept = taking.made_folders()?;
    taking.backed_up()?;
    let left = taking.leftovers()?;
    Ok(TakenBack {
        kept,
        launcher_restored,
        left,
    })
}

/// What a rollback works from: where the installation, its state, the update's staging and its
/// backup are, the journal that names every move, the retired name a new launcher copy in place
/// takes, and where the rollback passes its fault points.
struct TakingBack<'a> {
    /// The installation's top.
    root: &'a Path,
    /// The installation's state folder.
    state: PathBuf,
    /// The update's staging, which the journal names.
    staging: PathBuf,
    /// The update's backup of the installed names it moved aside.
    aside: PathBuf,
    /// The update's journal.
    journal: &'a JournalFile,
    /// Where a new launcher copy in place goes aside to.
    new_copy: PathBuf,
    /// Where the rollback passes its fault points.
    faults: &'a dyn FaultPoints,
}

impl<'a> TakingBack<'a> {
    /// The rollback of the update whose journal is `journal` in `folder`, for `platform`.
    fn of(
        folder: &'a InstallFolder,
        journal: &'a JournalFile,
        platform: Platform,
        faults: &'a dyn FaultPoints,
    ) -> Result<TakingBack<'a>, UpdateError> {
        let written = journal.journal();
        let state = folder.state_dir();
        let new_copy = retired_name(&written.to, &written.staging, platform)
            .ok_or_else(|| inconsistent(format!("{:?} is no update's staging", written.staging)))?;
        Ok(TakingBack {
            root: folder.root(),
            staging: state.join(&written.staging),
            aside: state.join(UPDATE_BACKUP),
            new_copy: state.join(new_copy),
            state,
            journal,
            faults,
        })
    }

    /// Puts the installed launcher copy back in its place, as [`roll_back`]'s first step says, and
    /// says whether it moved it there from its retired name.
    fn launcher(&self) -> Result<bool, UpdateError> {
        let written = self.journal.journal();
        let installed = self.root.join(&written.launcher);
        let in_place = is_there(&installed)?;
        let staged = is_there(&self.staging.join(&written.launcher))?;
        if written.launcher_missing {
            if in_place && !staged {
                self.move_back(&installed, &self.new_copy)?;
            }
            return Ok(false);
        }
        let retired = self.state.join(&written.retired);
        if !is_there(&retired)? {
            // Never moved aside, or back already.
            return if in_place {
                Ok(false)
            } else {
                Err(inconsistent(format!(
                    "the launcher copy {} is neither in place nor retired",
                    written.launcher
                )))
            };
        }
        if in_place {
            if staged {
                return Err(inconsistent(format!(
                    "{} stands in place while the installed copy is retired and the new one \
                     staged",
                    written.launcher
                )));
            }
            self.move_back(&installed, &self.new_copy)?;
        }
        self.move_back(&retired, &installed)?;
        Ok(true)
    }

    /// Moves each new name the swap put in place back into the staging, as [`roll_back`]'s
    /// second step says.
    fn published(&self) -> Result<(), UpdateError> {
        let written = self.journal.journal();
        let replaced: BTreeSet<&str> = written.backup.iter().map(String::as_str).collect();
        for text in written.publish.iter().rev() {
            let name = journal_name(text)?;
            let target = name.path_under(self.root);
            let staged = name.path_under(&self.staging);
            if !is_there(&target)? || is_there(&staged)? {
                continue;
            }
            if replaced.contains(text.as_str()) && !is_there(&name.path_under(&self.aside))? {
                continue;
            }
            if !is_there(&self.staging)? {
                return Err(inconsistent(format!(
                    "{text} stands in place, and the staging it came from is gone"
                )));
            }
            make_folders_above(&staged)?;
            self.move_back(&target, &staged)?;
        }
        Ok(())
    }

    /// Removes each folder the swap made, innermost first, while it is a folder, reached through
    /// no link, that holds nothing; gives those that stay.
    fn made_folders(&self) -> Result<Vec<String>, UpdateError> {
        let mut kept = Vec::new();
        for text in self.journal.journal().made_folders.iter().rev() {
            let path = journal_name(text)?.path_under(self.root);
            match no_links(&path) {
                Ok(()) => {}
                Err(FilesError::Link { .. }) => {
                    kept.push(text.clone());
                    continue;
                }
                Err(error) => return Err(files_error(error)),
            }
            let io = |source: io::Error| UpdateError::Io {
                path: path.clone(),
                source,
            };
            match fs::symlink_metadata(&path) {
                Ok(entry) if entry.is_dir() => {}
                Ok(_) => {
                    kept.push(text.clone());
                    continue;
                }
                Err(error) if is_absent(&error) => continue,
                Err(source) => return Err(io(source)),
            }
            if fs::read_dir(&path).map_err(io)?.next().is_some() {
                kept.push(text.clone());
                continue;
            }
            retry_locked(|| fs::remove_dir(&path)).map_err(io)?;
        }
        self.pass(self.root)?;
        Ok(kept)
    }

    /// Moves each installed name the swap moved aside back into place, last first: one in the
    /// backup and not in place.
    fn backed_up(&self) -> Result<(), UpdateError> {
        for text in self.journal.journal().backup.iter().rev() {
            let name = journal_name(text)?;
            let aside = name.path_under(&self.aside);
            let target = name.path_under(self.root);
            match (is_there(&aside)?, is_there(&target)?) {
                (true, false) => {
                    make_folders_above(&target)?;
                    self.move_back(&aside, &target)?;
                }
                (true, true) => {
                    return Err(inconsistent(format!(
                        "{text} stands both in place and in the backup"
                    )));
                }
                (false, true) => {}
                (false, false) => {
                    return Err(inconsistent(format!(
                        "{text} is neither in place nor in the backup"
                    )));
                }
            }
        }
        Ok(())
    }

    /// Removes what the update left in the state folder, as [`roll_back`]'s last step says, and
    /// gives the new launcher copy's retired name when it stays.
    fn leftovers(&self) -> Result<Option<String>, UpdateError> {
        let written = self.journal.journal();
        remove_made_by_update(&self.state, UPDATE_STAGING_PREFIX, &written.staging)?;
        self.pass(&self.staging)?;
        remove_backup(&self.aside)?;
        self.pass(&self.aside)?;
        if let Some(download) = &written.download {
            remove_made_by_update(&self.state, DOWNLOAD_PREFIX, download)?;
            self.pass(&self.state.join(download))?;
        }
        // Tried once: a copy that stays, as a launcher running from it holds it, is left for the
        // next start, and no journal needs to name it.
        let left = match fs::remove_file(&self.new_copy) {
            Err(error) if !is_absent(&error) => self
                .new_copy
                .file_name()
                .and_then(|name| name.to_str())
                .map(str::to_owned),
            _ => None,
        };
        self.pass(&self.new_copy)?;
        remove_if_there(&pending_path(self.journal.path())).map_err(rollback_files)?;
        remove_if_there(&pending_path(&self.state.join(RECEIPT_FILE))).map_err(rollback_files)?;
        self.journal.remove().map_err(rollback_files)?;
        self.pass(self.journal.path())?;
        Ok(left)
    }

    /// Moves `from` back to `to`, by a rename that never replaces anything, once neither `from`,
    /// nor a folder above it, nor a folder above `to` is a link; then passes. A link, or anything
    /// at `to`, is [`UpdateError::Inconsistent`].
    fn move_back(&self, from: &Path, to: &Path) -> Result<(), UpdateError> {
        match checked_move(from, to) {
            Ok(()) => self.pass(to),
            Err(UpdateError::Links { path }) => {
                Err(inconsistent(format!("a link stands at {}", path.display())))
            }
            Err(UpdateError::Io { source, .. })
                if source.kind() == io::ErrorKind::AlreadyExists =>
            {
                Err(inconsistent(format!("{} stands already", to.display())))
            }
            Err(other) => Err(other),
        }
    }

    /// Passes [`FaultPoint::UpdateRollback`], a failure there reported as one at `at`, what the
    /// step before it took back.
    fn pass(&self, at: &Path) -> Result<(), UpdateError> {
        pass_rollback(self.faults, at).map_err(files_error)
    }
}

/// The name `text` a journal lists, as a name under the installation's top.
fn journal_name(text: &str) -> Result<RelPath, UpdateError> {
    RelPath::parse(text, &PAYLOAD_NAMES)
        .map_err(|refused| inconsistent(format!("the journal names {text:?}: {refused}")))
}

/// The refusal to go on taking an update back, for what was found.
fn inconsistent(detail: String) -> UpdateError {
    UpdateError::Inconsistent {
        detail: detail.into(),
    }
}

/// The rollback's failure for what a file operation reported: a link on its way is
/// [`UpdateError::Inconsistent`], since a rollback never goes through one.
fn rollback_files(error: FilesError) -> UpdateError {
    match error {
        FilesError::Link { path } => inconsistent(format!("a link stands at {}", path.display())),
        other => UpdateError::Files(other),
    }
}

/// Whether anything is at `path`, looked at without following a link.
pub(super) fn is_there(path: &Path) -> Result<bool, UpdateError> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(true),
        Err(error) if is_absent(&error) => Ok(false),
        Err(source) => Err(UpdateError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// Makes the folders above `path` that are not there, once none of them is a link.
fn make_folders_above(path: &Path) -> Result<(), UpdateError> {
    let Some(folder) = path.parent() else {
        return Ok(());
    };
    no_links(folder).map_err(rollback_files)?;
    fs::create_dir_all(folder).map_err(|source| UpdateError::Io {
        path: folder.to_path_buf(),
        source,
    })
}

/// Removes the folder `name` that an update made in the state folder `state` with `prefix`, with
/// whatever it holds, counting one already gone as removed; a link on the way is a rollback's
/// [`UpdateError::Inconsistent`].
fn remove_made_by_update(state: &Path, prefix: &str, name: &str) -> Result<(), UpdateError> {
    remove_named(state, prefix, name).map_err(rollback_files)
}

/// Removes the folder `name` that a job made in the state folder `state` with `prefix`, with
/// whatever it holds, once neither it nor a folder above it is a link, counting one already gone
/// as removed.
pub(crate) fn remove_named(state: &Path, prefix: &str, name: &str) -> Result<(), FilesError> {
    match Staging::reopen(state, prefix, name) {
        Ok(staging) => remove_staging(staging),
        Err(FilesError::Io { source, .. }) if is_absent(&source) => Ok(()),
        Err(error) => Err(error),
    }
}

/// Removes the backup folder `aside` while it holds nothing but folders, counting one already
/// gone as removed. A file or a link left in it is the installation's and stays, kept for whoever
/// looks: it stops the rollback as [`UpdateError::Inconsistent`].
fn remove_backup(aside: &Path) -> Result<(), UpdateError> {
    no_links(aside).map_err(rollback_files)?;
    let mut folders = vec![aside.to_path_buf()];
    while let Some(folder) = folders.pop() {
        let io = |source: io::Error| UpdateError::Io {
            path: folder.clone(),
            source,
        };
        let entries = match fs::read_dir(&folder) {
            Ok(entries) => entries,
            Err(error) if is_absent(&error) && folder == aside => return Ok(()),
            Err(source) => return Err(io(source)),
        };
        for entry in entries {
            let entry = entry.map_err(io)?;
            if entry.file_type().map_err(io)?.is_dir() {
                folders.push(entry.path());
            } else {
                return Err(inconsistent(format!(
                    "the backup still holds {}",
                    entry.path().display()
                )));
            }
        }
    }
    remove_folder(aside).map_err(rollback_files)
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
pub(crate) fn remove_folder(path: &Path) -> Result<(), FilesError> {
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

/// Whether the receipt `new` is in place as `folder`'s receipt, as after a write of it that
/// failed: read back, once a file is seen at its name, as the installation's receipt is read, and
/// the same receipt.
pub(super) fn receipt_committed(new: &Receipt, folder: &InstallFolder) -> bool {
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
