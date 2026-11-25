//! Recovering an update that a crash, a power cut or a kill stopped, from its journal alone, under
//! the installation's lock: an update staged or rolling back is taken back as the job takes a
//! failure back; one swapping is finished when its receipt stands, or when every move it lists is
//! found made and the launcher copy and the release record at the installation's top are the ones
//! its receipt records, and is taken back otherwise; and one committed is cleaned up. Every file
//! an update moves was checked before its first move, so an update whose every move is made has
//! put the new release in place whole, and only its receipt is left to write. Finishing it then
//! leaves the new launcher copy, which may be what runs the recovery, where it stands.

use std::fs::{self, File};
use std::path::Path;

use gdl_files::{BLOCK_LEN, FilesError, PAYLOAD_NAMES, RelPath, fill_from, is_absent};
use gdl_media::Sha256Digest;
use gdl_release::Platform;
use sha2::{Digest, Sha256};

use super::journal::{JournalFile, UPDATE_BACKUP, UpdateJournal, UpdatePhase};
use super::settings::UpdateSettings;
use super::swap::{clean_up, commit_receipt, is_there, receipt_committed, roll_back};
use super::{RetiredCopy, UpdateError};
use crate::faults::{FaultPoint, FaultPoints};
use crate::folder::{InstallFolder, JOURNAL_DIR, RECEIPT_FILE};
use crate::payload::BUILD_INFO;
use crate::recover::{RecoverError, UpdateRecovery};

/// What recovering an update did, and the names in the state folder it left for the next
/// start's sweep: a retired launcher copy that a launcher running from it holds.
type Done = (UpdateRecovery, Vec<String>);

/// Recovers the update that `journal`, read from the installation `folder`'s journal folder and
/// held to what an update writes, records, asking the world beyond the folder through `settings`,
/// as the module says.
pub(crate) fn recover_update(
    folder: &InstallFolder,
    journal: UpdateJournal,
    settings: &UpdateSettings<'_>,
) -> Result<Done, RecoverError> {
    let platform = journal.receipt.checked_platform().map_err(|detail| {
        RecoverError::UpdateJournalUnreadable {
            detail: detail.into(),
        }
    })?;
    let mut file = JournalFile::resume(&folder.state_dir().join(JOURNAL_DIR), journal);
    let faults = settings.faults;
    match file.journal().phase {
        UpdatePhase::Staged | UpdatePhase::RollingBack => {
            take_back(folder, &mut file, platform, faults)
        }
        UpdatePhase::Swapping if every_move_made(folder, file.journal())? => {
            roll_forward(folder, &mut file, platform, settings)
        }
        UpdatePhase::Swapping => take_back(folder, &mut file, platform, faults),
        UpdatePhase::Committed => Ok(cleaned(
            clean_up(folder, &file, faults),
            UpdateRecovery::CleanedUp,
        )),
    }
}

/// Takes the update `file` records back, as the job takes a failure back from the swap on.
fn take_back(
    folder: &InstallFolder,
    file: &mut JournalFile,
    platform: Platform,
    faults: &dyn FaultPoints,
) -> Result<Done, RecoverError> {
    let taken = roll_back(folder, file, platform, faults).map_err(recovery_error)?;
    let rolled_back = UpdateRecovery::RolledBack {
        kept: taken.kept,
        launcher_restored: taken.launcher_restored,
    };
    Ok((rolled_back, taken.left.into_iter().collect()))
}

/// Finishes the update `file` records, every move of which is made: its receipt written when it
/// is not in place, judged by what it left when its write fails as the job's is, then the journal
/// moved to committed, then the cleanup. The shell is told of the executable when the update put a
/// new one in place.
///
/// [`FaultPoint::UpdateCommit`] is passed as the job passes it, so that each pass stands for the
/// same step in either: the first for the journal that records the swap, there already; the second
/// for the receipt, written now or found in place; the third for the journal moved to committed.
fn roll_forward(
    folder: &InstallFolder,
    file: &mut JournalFile,
    platform: Platform,
    settings: &UpdateSettings<'_>,
) -> Result<Done, RecoverError> {
    pass_commit(settings.faults, file.path())?;
    let receipt = file.journal().receipt.clone();
    if !receipt_committed(&receipt, folder) {
        commit_receipt(&receipt, folder, settings.write_receipt).map_err(recovery_error)?;
    }
    pass_commit(settings.faults, &folder.state_dir().join(RECEIPT_FILE))?;
    file.committed().map_err(recovery_error)?;
    pass_commit(settings.faults, file.path())?;
    let cleanup = clean_up(folder, file, settings.faults);
    let executable = platform.executable();
    if file.journal().publish.iter().any(|name| name == executable) {
        (settings.notify_shell)(&folder.root().join(executable));
    }
    Ok(cleaned(cleanup, UpdateRecovery::Finished))
}

/// What a recovery whose update stands did, once its cleanup gave `cleanup`: `done` when the
/// cleanup finished, with a retired launcher copy it could not remove left for the sweep, and the
/// cleanup left for the next start otherwise, its journal naming everything it is to remove.
fn cleaned(cleanup: (RetiredCopy, Result<(), UpdateError>), done: UpdateRecovery) -> Done {
    match cleanup {
        (RetiredCopy::Left { name }, Ok(())) => (done, vec![name]),
        (_, Ok(())) => (done, Vec::new()),
        (_, Err(error)) => {
            let left = UpdateRecovery::CleanupLeft {
                detail: error.to_string().into(),
            };
            (left, Vec::new())
        }
    }
}

/// Whether every move the swapping `journal` lists is made in the installation `folder`, and the
/// launcher copy and the release record at its top are the ones the journal's receipt records;
/// or whether that receipt is in place already, which is written only once every move is made.
/// The moves are made when every installed name it moves aside is in the backup, every new name
/// is in place and gone from the staging, every name it removes is gone from the top, the new
/// launcher copy is in place and gone from the staging, and the installed one is retired unless
/// none was there.
fn every_move_made(folder: &InstallFolder, journal: &UpdateJournal) -> Result<bool, RecoverError> {
    if receipt_committed(&journal.receipt, folder) {
        return Ok(true);
    }
    let root = folder.root();
    let state = folder.state_dir();
    let staging = state.join(&journal.staging);
    let aside = state.join(UPDATE_BACKUP);
    let there = |path: &Path| is_there(path).map_err(recovery_error);
    for text in &journal.backup {
        let name = journal_name(text)?;
        let removed = !journal.publish.contains(text);
        if !there(&name.path_under(&aside))? || (removed && there(&name.path_under(root))?) {
            return Ok(false);
        }
    }
    for text in &journal.publish {
        let name = journal_name(text)?;
        if !there(&name.path_under(root))? || there(&name.path_under(&staging))? {
            return Ok(false);
        }
    }
    let launcher = root.join(&journal.launcher);
    if !there(&launcher)? || there(&staging.join(&journal.launcher))? {
        return Ok(false);
    }
    if !journal.launcher_missing && !there(&state.join(&journal.retired))? {
        return Ok(false);
    }
    let receipt = &journal.receipt;
    Ok(holds_digest(&launcher, &receipt.launcher.sha256)?
        && holds_digest(&root.join(BUILD_INFO), &receipt.build_info_sha256)?)
}

/// Whether a regular file is at `path`, not a link, and holds the bytes whose SHA-256 digest is
/// `expected`, in lower-case hexadecimal.
fn holds_digest(path: &Path, expected: &str) -> Result<bool, RecoverError> {
    let io = |source| RecoverError::Io {
        path: path.to_path_buf(),
        source,
    };
    match fs::symlink_metadata(path) {
        Ok(entry) if entry.is_file() => {}
        Ok(_) => return Ok(false),
        Err(error) if is_absent(&error) => return Ok(false),
        Err(source) => return Err(io(source)),
    }
    let mut file = File::open(path).map_err(io)?;
    let mut hasher = Sha256::new();
    let mut block = vec![0; BLOCK_LEN];
    loop {
        let read = fill_from(&mut file, &mut block).map_err(io)?;
        hasher.update(&block[..read]);
        if read < block.len() {
            break;
        }
    }
    Ok(Sha256Digest::from_hex(expected) == Some(Sha256Digest(hasher.finalize().into())))
}

/// The name `text` an update's journal lists, as a name under the installation's top: one the
/// journal's reading held to the payload's rules.
fn journal_name(text: &str) -> Result<RelPath, RecoverError> {
    RelPath::parse(text, &PAYLOAD_NAMES).map_err(|refused| RecoverError::UpdateInconsistent {
        detail: format!("the journal names {text:?}: {refused}").into(),
    })
}

/// Passes [`FaultPoint::UpdateCommit`] at `faults`, a failure there reported as one at `at`, what
/// the step before it wrote.
fn pass_commit(faults: &dyn FaultPoints, at: &Path) -> Result<(), RecoverError> {
    faults
        .pass(FaultPoint::UpdateCommit)
        .map_err(|source| RecoverError::Io {
            path: at.to_path_buf(),
            source,
        })
}

/// The recovery's failure for what stopped finishing or taking back an update: a file
/// operation's failure and the system's refusal as they are, a link on the way as the link, and
/// anything else as what keeps the update from being finished or taken back.
fn recovery_error(error: UpdateError) -> RecoverError {
    match error {
        UpdateError::Inconsistent { detail } => RecoverError::UpdateInconsistent { detail },
        UpdateError::Files(error) => RecoverError::Files(error),
        UpdateError::Io { path, source } => RecoverError::Io { path, source },
        UpdateError::Links { path } => RecoverError::Files(FilesError::Link { path }),
        other => RecoverError::UpdateInconsistent {
            detail: other.to_string().into(),
        },
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::fs::{self, File};
    use std::io;
    use std::panic::{AssertUnwindSafe, catch_unwind};
    use std::path::{Path, PathBuf};
    use std::sync::Mutex;
    use std::time::Duration;

    use gdl_files::{Cancel, FilesError, Hold, Job, retry_locked};
    use gdl_fixtures::{InstallFixture, pretty_json};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::super::tests::{
        Contents, Heard, INSTALLED, NEXT, OnceJournalled, StartedLater, Told, answering, as_it_is,
        bundled, bundled_as, contents_of, executable, failed, journal_phase, launcher_copy,
        nothing_recovered, players_installation, receipt_at, recover_by, recover_simply,
        retired_copies, rolled_back, running_for, stagings, standing_in, state_names, update_by,
        update_simply, updated, version,
    };
    use super::super::{
        CHECK_INSTALLED, FINISH_CLEANUP, RECOVER_UPDATE, UPDATE_BACKUP, UPDATE_JOURNAL,
        UpdateError, UpdateSettings, VERIFY_DOWNLOAD,
    };
    use super::recovery_error;
    use crate::faults::{FailAt, FaultPoint, FaultPoints, Faults, NoFaults, PanicAt};
    use crate::folder::{InstallFolder, JOURNAL_DIR, RECEIPT_FILE, STATE_DIR};
    use crate::install::{Changes, RecoveryOutcome};
    use crate::payload::BUILD_INFO;
    use crate::probe::GameUseError;
    use crate::receipt::Receipt;
    use crate::recover::{RecoverError, Recovered, UpdateRecovery};
    use crate::sections::Sections;

    /// The update's journal in the installation at `root`.
    fn journal_path(root: &Path) -> PathBuf {
        root.join(STATE_DIR).join(JOURNAL_DIR).join(UPDATE_JOURNAL)
    }

    /// Runs the update of `fixture` from `source` with `faults` until a pass of theirs panics, as
    /// a fault in the launcher unwinds the job there: what the job made stays for the next start,
    /// and its lock is let go.
    fn panicked(fixture: &InstallFixture, source: &mut Sections<File>, faults: &dyn FaultPoints) {
        let settings = standing_in(faults, &answering);
        let unwound = catch_unwind(AssertUnwindSafe(|| {
            update_simply(fixture, source, NEXT, &settings)
        }));
        assert!(unwound.is_err(), "{unwound:?}");
    }

    /// An installation whose update to the newer release stopped once its journal recorded it
    /// staged, with what it staged kept, and its folder as it was before the update.
    fn left_staged() -> (TempDir, InstallFixture, Contents) {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let before = contents_of(&fixture.root);
        let journalled = OnceJournalled {
            point: FaultPoint::UpdateStage,
            journal: journal_path(&fixture.root),
            act: &|| panic!("the test panics once the journal is written"),
            done: Cell::new(false),
        };
        panicked(&fixture, &mut source, &journalled);
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("staged"));
        (dir, fixture, before)
    }

    /// An installation whose update to the newer release stopped as the new launcher copy was put
    /// in place: every move made, the receipt not yet written, the journal swapping; and its
    /// folder as it was before the update.
    fn left_swapped() -> (TempDir, InstallFixture, Contents) {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let before = contents_of(&fixture.root);
        panicked(
            &fixture,
            &mut source,
            &PanicAt::new(FaultPoint::LauncherReplace, 2),
        );
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("swapping"));
        assert_eq!(receipt_at(&fixture.root)["version"], json!(INSTALLED));
        (dir, fixture, before)
    }

    /// What an installation holds after an uninterrupted update to the newer release, as
    /// [`contents_of`] keeps it.
    fn as_after() -> Contents {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        contents_of(&fixture.root)
    }

    /// A job whose progress `heard` follows and whose cancel is `cancel`.
    fn heard_by<'a>(cancel: &'a Cancel, heard: &'a Heard<'_>) -> Job<'a> {
        Job {
            cancel,
            progress: heard,
        }
    }

    #[test]
    fn a_staged_update_is_taken_back() {
        let (_dir, fixture, before) = left_staged();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        assert_eq!(contents_of(&fixture.root), before);

        // Taken back from where a rollback a crash cut short left it: the staging gone, the
        // journal still there.
        let (_dir, fixture, before) = left_staged();
        let staging = fixture
            .root
            .join(STATE_DIR)
            .join(&stagings(&fixture.root)[0]);
        retry_locked(|| fs::remove_dir_all(&staging)).unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        assert_eq!(contents_of(&fixture.root), before);
    }

    #[test]
    fn a_swap_with_every_move_done_rolls_forward() {
        let expected = as_after();
        let (_dir, fixture, _) = left_swapped();
        let new_copy = fs::read(fixture.root.join(launcher_copy())).unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(
            recovered.unwrap(),
            Recovered {
                update: UpdateRecovery::Finished,
                ..nothing_recovered()
            }
        );
        // The receipt the journal carries is written, and the tree is the update's whole, the
        // new launcher copy left where the update put it.
        assert_eq!(receipt_at(&fixture.root)["version"], json!(NEXT));
        assert_eq!(
            fs::read(fixture.root.join(launcher_copy())).unwrap(),
            new_copy
        );
        assert_eq!(contents_of(&fixture.root), expected);
    }

    #[test]
    fn a_swap_whose_receipt_stands_rolls_forward_without_writing_it_again() {
        let expected = as_after();
        let (_dir, fixture, _) = left_swapped();
        // Killed once the receipt was written, before the journal recorded it committed.
        write_journals_receipt(&fixture.root);
        let unwritten = |_: &Receipt, _: &InstallFolder| -> Result<(), FilesError> {
            panic!("the receipt in place is written again")
        };
        let settings = UpdateSettings {
            write_receipt: &unwritten,
            ..standing_in(&NoFaults, &answering)
        };
        let recovered = recover_simply(&fixture.root, NEXT, &settings);
        assert_eq!(recovered.unwrap().update, UpdateRecovery::Finished);
        assert_eq!(contents_of(&fixture.root), expected);
    }

    /// Writes the receipt the update's journal in the installation at `root` carries, as an
    /// update killed once it wrote its receipt leaves it.
    fn write_journals_receipt(root: &Path) {
        let journal: Value =
            serde_json::from_slice(&fs::read(journal_path(root)).unwrap()).unwrap();
        let receipt: Receipt = serde_json::from_value(journal["receipt"].clone()).unwrap();
        receipt.write(&InstallFolder::at(root).unwrap()).unwrap();
    }

    #[test]
    fn a_roll_forward_passes_the_commit_as_the_job_does() {
        // Each pass stands for the job's own step, whether the job wrote the receipt before it
        // was stopped or the recovery writes it: the swap recorded, the receipt in place, the
        // journal committed.
        for receipt_stands in [false, true] {
            for (at, phase) in [(1, "swapping"), (2, "swapping"), (3, "committed")] {
                let context = format!("pass {at}, the receipt standing: {receipt_stands}");
                let (_dir, fixture, _) = left_swapped();
                if receipt_stands {
                    write_journals_receipt(&fixture.root);
                }
                let fail = FailAt::new(FaultPoint::UpdateCommit, at);
                let recovered =
                    recover_simply(&fixture.root, NEXT, &standing_in(&fail, &answering));
                match recovered {
                    Err(error @ RecoverError::Io { .. }) => assert_eq!(error.rule(), "io"),
                    other => panic!("{context}: {other:?}"),
                }
                assert_eq!(
                    journal_phase(&fixture.root).as_deref(),
                    Some(phase),
                    "{context}"
                );
                let receipt = if at == 1 && !receipt_stands {
                    INSTALLED
                } else {
                    NEXT
                };
                assert_eq!(
                    receipt_at(&fixture.root)["version"],
                    json!(receipt),
                    "{context}"
                );
            }
        }
    }

    #[test]
    fn a_swap_with_a_digest_that_disagrees_rolls_back() {
        for changed in [BUILD_INFO, launcher_copy()] {
            let (_dir, fixture, before) = left_swapped();
            let path = fixture.root.join(changed);
            let mut bytes = fs::read(&path).unwrap();
            bytes.extend_from_slice(b" changed");
            fs::write(&path, bytes).unwrap();
            let recovered =
                recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
            assert_eq!(recovered.unwrap(), rolled_back(true, &[]), "{changed}");
            assert_eq!(contents_of(&fixture.root), before, "{changed}");
            assert!(retired_copies(&fixture.root, NEXT).is_empty(), "{changed}");
        }
    }

    #[test]
    fn a_swap_with_a_move_not_made_rolls_back() {
        // Every move made, but a removed file put back in its place by hand, out of the backup:
        // the update is not whole, so it is taken back, and that file stays where it is.
        let (_dir, fixture, before) = left_swapped();
        let held = fixture
            .root
            .join(STATE_DIR)
            .join(UPDATE_BACKUP)
            .join("shaders")
            .join("test.spv");
        let removed = fixture.root.join("shaders").join("test.spv");
        let bytes = fs::read(&held).unwrap();
        retry_locked(|| fs::rename(&held, &removed)).unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(recovered.unwrap(), rolled_back(true, &[]));
        assert_eq!(contents_of(&fixture.root), before);
        assert_eq!(fs::read(&removed).unwrap(), bytes);
    }

    #[test]
    fn a_half_done_move_is_inconsistent() {
        // Three installed files moved aside, the first of them found in its place too, as a move
        // that copied rather than moved would leave it.
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        panicked(
            &fixture,
            &mut source,
            &PanicAt::new(FaultPoint::UpdateMove, 3),
        );
        let aside = fixture.root.join(STATE_DIR).join(UPDATE_BACKUP);
        fs::copy(aside.join("VERSION"), fixture.root.join("VERSION")).unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        match recovered {
            Err(error @ RecoverError::UpdateInconsistent { .. }) => {
                assert_eq!(error.rule(), "inconsistent");
                assert!(error.to_string().contains("VERSION"), "{error}");
            }
            other => panic!("{other:?}"),
        }
        // The journal and both copies stay for whoever looks.
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("rollingBack"));
        assert!(aside.join("VERSION").is_file());
        assert!(fixture.root.join("VERSION").is_file());
        assert_eq!(stagings(&fixture.root).len(), 1);
    }

    #[test]
    fn a_rollback_retires_the_new_launcher_copy() {
        // The job had decided to take the update back, and was stopped before its first move: the
        // new launcher copy, which a launcher may run from, stands in place.
        let (_dir, fixture, before) = left_swapped();
        let path = journal_path(&fixture.root);
        let mut journal: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        journal["phase"] = json!("rollingBack");
        fs::write(&path, pretty_json(&journal)).unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(recovered.unwrap(), rolled_back(true, &[]));
        // The installed copy is back, no staging is left, and the new copy went aside under a
        // retired name of its own, then went.
        assert_eq!(contents_of(&fixture.root), before);
        assert!(stagings(&fixture.root).is_empty());
        assert!(retired_copies(&fixture.root, NEXT).is_empty());
        assert!(retired_copies(&fixture.root, INSTALLED).is_empty());
    }

    #[test]
    fn a_launcher_copy_gone_before_its_move_is_taken_back() {
        // Stopped once every runtime file moved, its launcher copy gone since, as a scanner may
        // quarantine a program: nothing is left to put back of it, and the rest is taken back.
        let (dir, fixture) = players_installation();
        let copy = fixture.root.join(launcher_copy());
        let old = fs::read(&copy).unwrap();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let before = contents_of(&fixture.root);
        panicked(
            &fixture,
            &mut source,
            &PanicAt::new(FaultPoint::UpdateMove, 14),
        );
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("swapping"));
        retry_locked(|| fs::remove_file(&copy)).unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        assert!(!copy.exists());
        fs::write(&copy, &old).unwrap();
        assert_eq!(contents_of(&fixture.root), before);
    }

    #[test]
    fn a_missing_launcher_copy_put_in_place_is_finished() {
        // An installation without its launcher copy, whose update stopped as the new one was put
        // in place: every move is made, and the update is finished with that copy where it is.
        let (dir, fixture) = players_installation();
        let copy = fixture.root.join(launcher_copy());
        retry_locked(|| fs::remove_file(&copy)).unwrap();
        let (source_path, mut source) =
            bundled_as(dir.path(), "next-launcher", &fixture.runtime.newer(NEXT));
        panicked(
            &fixture,
            &mut source,
            &PanicAt::new(FaultPoint::LauncherReplace, 2),
        );
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("swapping"));
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(
            recovered.unwrap(),
            Recovered {
                update: UpdateRecovery::Finished,
                ..nothing_recovered()
            }
        );
        assert_eq!(fs::read(&copy).unwrap(), fs::read(&source_path).unwrap());
        assert_eq!(receipt_at(&fixture.root)["version"], json!(NEXT));
        assert_eq!(state_names(&fixture.root), [RECEIPT_FILE]);
    }

    #[test]
    fn a_launcher_copy_put_back_by_hand_is_kept() {
        // An update of an installation without its launcher copy, stopped once staged, and taken
        // back until its staging went; then a copy put in place by hand. That copy is not the
        // update's, and stays as it is.
        let (dir, fixture) = players_installation();
        let copy = fixture.root.join(launcher_copy());
        retry_locked(|| fs::remove_file(&copy)).unwrap();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let before = contents_of(&fixture.root);
        let journalled = OnceJournalled {
            point: FaultPoint::UpdateStage,
            journal: journal_path(&fixture.root),
            act: &|| panic!("the test panics once the journal is written"),
            done: Cell::new(false),
        };
        panicked(&fixture, &mut source, &journalled);
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("staged"));
        let staging = fixture
            .root
            .join(STATE_DIR)
            .join(&stagings(&fixture.root)[0]);
        retry_locked(|| fs::remove_dir_all(&staging)).unwrap();
        fs::write(&copy, b"a launcher of the player's").unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        assert_eq!(fs::read(&copy).unwrap(), b"a launcher of the player's");
        assert!(retired_copies(&fixture.root, NEXT).is_empty());
        // Nothing else is left of the update.
        retry_locked(|| fs::remove_file(&copy)).unwrap();
        assert_eq!(contents_of(&fixture.root), before);
    }

    #[test]
    fn a_running_game_holds_the_recovery_until_it_closes() {
        let (_dir, fixture, before) = left_staged();
        let ignore = |_: Hold| {};
        let heard = Heard::new(&ignore);
        let cancel = Cancel::default();
        let game = StartedLater::new(0, running_for());
        let game_closed = |path: &Path| game.closed(path);
        let settings = UpdateSettings {
            game_closed: &game_closed,
            ..standing_in(&NoFaults, &answering)
        };
        let recovered = recover_by(&fixture.root, NEXT, &heard_by(&cancel, &heard), &settings);
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        // The hold comes before anything is changed; then each part is told as it is reached.
        assert_eq!(
            heard.told(),
            [
                Told::Held(Hold::GameRunning),
                Told::Resumed,
                Told::Phase(RECOVER_UPDATE.0),
                Told::Phase(FINISH_CLEANUP.0),
            ]
        );
        assert_eq!(contents_of(&fixture.root), before);
    }

    #[test]
    fn a_game_running_from_the_backup_holds_the_recovery() {
        // The job moved the executable aside, then its rollback stopped at once.
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let before = contents_of(&fixture.root);
        let moved = FailAt::new(FaultPoint::UpdateMove, 5);
        let stopped = FailAt::new(FaultPoint::UpdateRollback, 1);
        let faults = Faults(&[&moved, &stopped]);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&faults, &answering),
        ));
        assert_eq!(failure.changes, Changes::Kept);
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("rollingBack"));
        let installed = fixture.root.join(executable());
        let aside = fixture
            .root
            .join(STATE_DIR)
            .join(UPDATE_BACKUP)
            .join(executable());
        assert!(!installed.exists());
        assert!(aside.is_file());

        // The game started from the top before the move runs on from the backup.
        let asked = Mutex::new(Vec::new());
        let game = StartedLater::new(0, running_for());
        let game_closed = |path: &Path| {
            asked.lock().unwrap().push(path.to_path_buf());
            if path == installed {
                Err(GameUseError::Missing)
            } else {
                game.closed(path)
            }
        };
        let ignore = |_: Hold| {};
        let heard = Heard::new(&ignore);
        let cancel = Cancel::default();
        let settings = UpdateSettings {
            game_closed: &game_closed,
            ..standing_in(&NoFaults, &answering)
        };
        let recovered = recover_by(&fixture.root, NEXT, &heard_by(&cancel, &heard), &settings);
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        assert_eq!(
            heard.holds(),
            [Told::Held(Hold::GameRunning), Told::Resumed]
        );
        let asked = asked.into_inner().unwrap();
        assert_eq!(asked.first(), Some(&installed));
        assert!(asked[1..].iter().all(|path| *path == aside), "{asked:?}");
        assert_eq!(contents_of(&fixture.root), before);
    }

    #[test]
    fn a_cancel_during_the_recoverys_hold_changes_nothing() {
        let (_dir, fixture, _) = left_staged();
        let before = as_it_is(&fixture.root);
        let cancel = Cancel::default();
        let stop = |_: Hold| cancel.cancel();
        let heard = Heard::new(&stop);
        let game = StartedLater::new(0, Duration::from_secs(3600));
        let game_closed = |path: &Path| game.closed(path);
        let settings = UpdateSettings {
            game_closed: &game_closed,
            ..standing_in(&NoFaults, &answering)
        };
        let recovered = recover_by(&fixture.root, NEXT, &heard_by(&cancel, &heard), &settings);
        match recovered {
            Err(error @ RecoverError::Cancelled) => assert_eq!(error.rule(), "cancelled"),
            other => panic!("{other:?}"),
        }
        assert_eq!(heard.holds(), [Told::Held(Hold::GameRunning)]);
        assert_eq!(as_it_is(&fixture.root), before);
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("staged"));
    }

    #[test]
    fn an_update_holds_for_a_game_while_it_recovers_first() {
        let (dir, fixture, _) = left_staged();
        let mut source = bundled(dir.path(), &fixture.runtime.newer(NEXT));
        let ignore = |_: Hold| {};
        let heard = Heard::new(&ignore);
        let cancel = Cancel::default();
        let game = StartedLater::new(0, running_for());
        let game_closed = |path: &Path| game.closed(path);
        let settings = UpdateSettings {
            game_closed: &game_closed,
            ..standing_in(&NoFaults, &answering)
        };
        let done = updated(update_by(
            &fixture,
            &mut source,
            NEXT,
            &heard_by(&cancel, &heard),
            &settings,
        ));
        assert_eq!(done.to, version(NEXT));
        assert_eq!(done.recovered, rolled_back(false, &[]));
        // The recovery held the update before it read its source, and told no phase of its own.
        let told = heard.told();
        assert_eq!(
            told[..4],
            [
                Told::Phase(CHECK_INSTALLED.0),
                Told::Held(Hold::GameRunning),
                Told::Resumed,
                Told::Phase(VERIFY_DOWNLOAD.0),
            ],
            "{told:?}"
        );
        assert_eq!(
            heard.holds(),
            [Told::Held(Hold::GameRunning), Told::Resumed]
        );
        assert_eq!(state_names(&fixture.root), [RECEIPT_FILE]);
    }

    #[test]
    fn an_expansion_journal_is_left_and_refuses_an_update() {
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let state = fixture.root.join(STATE_DIR);
        fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
        let journal = json!({"schema": 1, "minLauncher": INSTALLED, "phase": "extracting"});
        fs::write(
            state.join(JOURNAL_DIR).join("expansion-n64.json"),
            pretty_json(&journal),
        )
        .unwrap();
        // Beside it, a staging an update left, which the sweep would otherwise remove.
        fs::create_dir(state.join("stage-AbC123xy")).unwrap();
        let before = as_it_is(&fixture.root);
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        match recovered {
            Err(error @ RecoverError::OtherJob { .. }) => {
                assert_eq!(error.rule(), "otherJob");
                assert!(error.to_string().ends_with("expansion-n64.json"), "{error}");
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(as_it_is(&fixture.root), before);
        let failure = failed(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&NoFaults, &answering),
        ));
        assert_eq!(failure.rule(), "otherJob", "{failure:?}");
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(as_it_is(&fixture.root), before);
    }

    #[test]
    fn a_journal_that_claims_a_players_file_is_left() {
        // A staged update's journal changed to move the player's save aside, as a hand or a
        // fault might: the recovery reads no such journal, and neither it nor the save is moved.
        let (_dir, fixture, _) = left_staged();
        let path = journal_path(&fixture.root);
        let mut journal: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        journal["backup"]
            .as_array_mut()
            .unwrap()
            .push(json!("saves/hero.json"));
        fs::write(&path, pretty_json(&journal)).unwrap();
        let save = fixture.root.join("saves").join("hero.json");
        let saved = fs::read(&save).unwrap();
        let before = as_it_is(&fixture.root);
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        match recovered {
            Err(error @ RecoverError::UpdateJournalUnreadable { .. }) => {
                assert_eq!(error.rule(), "journalUnreadable");
                assert!(error.to_string().contains("saves/hero.json"), "{error}");
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(as_it_is(&fixture.root), before);
        assert_eq!(fs::read(&save).unwrap(), saved);
    }

    #[test]
    fn an_unreadable_install_journal_no_longer_blocks_the_update_recovery() {
        // An install journal that cannot be read refuses the install's part alone: the update is
        // taken back beside it, and the journal stays as it was.
        let (_dir, fixture, _) = left_staged();
        let install = fixture
            .root
            .join(STATE_DIR)
            .join(JOURNAL_DIR)
            .join(crate::install::INSTALL_JOURNAL);
        fs::write(&install, b"not JSON\n").unwrap();
        let recovered = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        match recovered {
            Err(RecoverError::Install(error)) => assert_eq!(error.rule(), "journalUnreadable"),
            other => panic!("{other:?}"),
        }
        assert_eq!(journal_phase(&fixture.root), None);
        assert!(stagings(&fixture.root).is_empty());
        assert_eq!(receipt_at(&fixture.root)["version"], json!(INSTALLED));
        assert_eq!(
            fs::read(fixture.root.join("VERSION")).unwrap(),
            b"1.0.0-beta\n"
        );
        assert_eq!(fs::read(&install).unwrap(), b"not JSON\n");
    }

    #[test]
    fn recovering_twice_does_nothing_more() {
        let (_dir, fixture, before) = left_staged();
        let first = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        let first = first.unwrap();
        assert_eq!(first.install, RecoveryOutcome::Nothing);
        assert_eq!(contents_of(&fixture.root), before);
        let settled = as_it_is(&fixture.root);
        let again = recover_simply(&fixture.root, NEXT, &standing_in(&NoFaults, &answering));
        assert_eq!(again.unwrap(), nothing_recovered());
        assert_eq!(as_it_is(&fixture.root), settled);
    }

    #[test]
    fn a_recovery_by_an_older_launcher_leaves_a_newer_update() {
        // The receipt the update wrote records the new release, so the launcher of the old one
        // changes nothing of the cleanup it left.
        let (dir, fixture) = players_installation();
        let mut source = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let fail = FailAt::new(FaultPoint::UpdateCleanup, 1);
        updated(update_simply(
            &fixture,
            &mut source,
            NEXT,
            &standing_in(&fail, &answering),
        ));
        let before = as_it_is(&fixture.root);
        let recovered = recover_simply(
            &fixture.root,
            INSTALLED,
            &standing_in(&NoFaults, &answering),
        );
        match recovered {
            Err(RecoverError::OlderLauncher { running, installed }) => {
                assert_eq!((&*running, &*installed), (INSTALLED, NEXT));
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(as_it_is(&fixture.root), before);
        assert_eq!(journal_phase(&fixture.root).as_deref(), Some("committed"));
    }

    #[test]
    fn recovery_errors_of_an_update_name_their_rules() {
        let path = PathBuf::from("journal");
        for (error, rule) in [
            (
                UpdateError::Inconsistent { detail: "x".into() },
                "inconsistent",
            ),
            (UpdateError::Links { path: path.clone() }, "link"),
            (
                UpdateError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                },
                "io",
            ),
            (UpdateError::JournalTooLarge { limit: 1 }, "inconsistent"),
        ] {
            let error = recovery_error(error);
            assert_eq!(error.rule(), rule, "{error:?}");
        }
        // A detail is told once, not wrapped in another's words.
        let error = recovery_error(UpdateError::Inconsistent {
            detail: "the backup still holds x".into(),
        });
        assert_eq!(
            error.to_string(),
            "The interrupted update cannot be finished or taken back: the backup still holds x"
        );
    }

    #[cfg(windows)]
    #[test]
    fn the_system_is_kept_awake_while_recovering_an_update() {
        use gdl_files::{PhaseId, Progress, RelPath};
        use windows_sys::Win32::System::Power::{ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

        use crate::platform;

        /// A progress that reads the thread's requests as the update's part begins.
        #[derive(Default)]
        struct Awake {
            state: Mutex<Option<u32>>,
        }

        impl Progress for Awake {
            fn phase(&self, phase: PhaseId) {
                if phase == RECOVER_UPDATE {
                    *self.state.lock().unwrap() = Some(platform::execution_state());
                }
            }

            fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
        }

        let (_dir, fixture, _) = left_staged();
        let before = platform::execution_state();
        let awake = Awake::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &awake,
        };
        let settings = standing_in(&NoFaults, &answering);
        let recovered = recover_by(&fixture.root, NEXT, &job, &settings);
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        assert_eq!(
            *awake.state.lock().unwrap(),
            Some(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
        );
        assert_eq!(platform::execution_state(), before);
    }

    /// The variable that makes a run of this test program recover an update from the system's
    /// default error mode, in `an_update_recovery_fails_critical_errors_rather_than_showing_them`.
    /// Unset, as in a run of the tests, that part does nothing.
    #[cfg(windows)]
    const ERROR_MODE_CHILD: &str = "GDL_UPDATE_RECOVERY_ERROR_MODE_CHILD";

    /// The test that a child run of this program, started in the default error mode, runs alone.
    #[cfg(windows)]
    const RECOVERING_TEST: &str =
        "update::recover::tests::recovering_an_update_from_the_default_error_mode";

    /// The recovery is made in a child started in the system's default error mode, which shows
    /// critical errors, as a launcher started from Explorer is.
    #[cfg(windows)]
    #[test]
    fn an_update_recovery_fails_critical_errors_rather_than_showing_them() {
        crate::default_error_mode::run_in_default_error_mode(RECOVERING_TEST, ERROR_MODE_CHILD);
    }

    /// The child's part of `an_update_recovery_fails_critical_errors_rather_than_showing_them`.
    #[cfg(windows)]
    #[test]
    fn recovering_an_update_from_the_default_error_mode() {
        use gdl_files::{PhaseId, Progress, RelPath};
        use windows_sys::Win32::System::Diagnostics::Debug::SEM_FAILCRITICALERRORS;

        use crate::default_error_mode::{ERROR_MODE_CHECKED, started_in_default_error_mode};
        use crate::platform::error_mode;

        /// A progress that reads the process's error mode as the update's part begins.
        #[derive(Default)]
        struct Mode {
            at_phase: Mutex<Option<u32>>,
        }

        impl Progress for Mode {
            fn phase(&self, phase: PhaseId) {
                if phase == RECOVER_UPDATE {
                    *self.at_phase.lock().unwrap() = Some(error_mode());
                }
            }

            fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
        }

        let Some(started) = started_in_default_error_mode(ERROR_MODE_CHILD) else {
            return;
        };
        let (dir, fixture, _) = left_staged();
        let mode = Mode::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &mode,
        };
        let settings = standing_in(&NoFaults, &answering);
        let recovered = recover_by(&fixture.root, NEXT, &job, &settings);
        assert_eq!(recovered.unwrap(), rolled_back(false, &[]));
        // Critical errors fail calls from before the recovery's first look on, and the rest of
        // the mode is as it was.
        let failing = started | SEM_FAILCRITICALERRORS;
        assert_eq!(*mode.at_phase.lock().unwrap(), Some(failing));
        assert_eq!(error_mode(), failing);
        // Removed before the exit, which runs no destructor.
        drop(fixture);
        dir.close().unwrap();
        std::process::exit(ERROR_MODE_CHECKED);
    }
}
