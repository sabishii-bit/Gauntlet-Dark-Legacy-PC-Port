//! Kills at every step of a fresh install, and of the recovery that follows one. The install, or
//! the recovery, runs in a child of this test program, which ends itself at the pass of a fault
//! point the test names, as a crash or a kill stops a job: nothing is unwound, and what the job
//! held, its lock among it, is let go only as the system lets go of what a program that ended
//! held. The test then recovers the folder as the next start does, and holds it to what that
//! recovery must leave. What each kill and each recovery leaves is held apart from the alpha
//! installer's names at any depth. The children run one at a time.

use std::env;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant};

use gdl_files::{Cancel, Job, NoProgress};
use gdl_media::GameCubeImage;
use gdl_release::Version;

use super::recover::recover_install_with;
use super::settings::InstallSettings;
use super::tests::{answering, standing_in};
use super::{
    FolderRecord, RecoveryError, RecoveryOutcome, Unrecorded, install_game_with, plan_install,
};
use crate::faults::{FailAt, FaultPoint, FaultPoints, Faults, KillAt};
use crate::folder::LockError;
use crate::sections::Sections;

/// The variable that names the pass a child of this test program is killed at, as
/// `<point>:<pass>`: the point by its name, the pass counted from 1. Unset, as in a run of the
/// tests, [`killed_install`] does nothing.
const KILL: &str = "GDL_INSTALL_KILL";

/// The variable that names the folder holding the image and the launcher file a killed install
/// installs from.
const KILL_ROOT: &str = "GDL_INSTALL_KILL_ROOT";

/// The variable that names the folder a killed install installs into, or a killed recovery
/// recovers.
const KILL_DESTINATION: &str = "GDL_INSTALL_KILL_DESTINATION";

/// The variable that names a pass the killed job fails at, as [`KILL`] names one.
const FAIL: &str = "GDL_INSTALL_FAIL";

/// The variable that, set, makes the killed job a recovery of the folder rather than an install
/// into it.
const RECOVER: &str = "GDL_INSTALL_RECOVER";

/// The image a killed install installs from, in the folder [`KILL_ROOT`] names.
const IMAGE: &str = "game.iso";

/// The launcher file it installs from, beside the image.
const LAUNCHER: &str = "launcher.bin";

/// The launcher's version, which the tests install and recover as.
const VERSION: &str = "1.0.0-beta";

/// How often a recovery is tried again while the lock a killed child held is not yet let go.
const LOCK_RETRY_INTERVAL: Duration = Duration::from_millis(100);

/// How long the lock a killed child held is waited for: Windows lets go of the locks of a program
/// that ended some time after it ended.
const LOCK_RELEASE_WAIT: Duration = Duration::from_secs(10);

/// The name of the `at`-th pass of `point`, as [`KILL`] and [`FAIL`] name a pass.
fn pass_name(point: FaultPoint, at: u32) -> String {
    format!("{}:{at}", point.as_str())
}

/// The pass that `name` names, as [`KILL`] and [`FAIL`] name a pass.
fn named_pass(name: &str) -> (FaultPoint, u32) {
    let (point, at) = name
        .rsplit_once(':')
        .unwrap_or_else(|| panic!("{name:?} names no pass"));
    let point =
        FaultPoint::named(point).unwrap_or_else(|| panic!("{point:?} is no point a job passes"));
    let at = at
        .parse()
        .unwrap_or_else(|_| panic!("{name:?} counts no pass"));
    (point, at)
}

/// The folder the variable `name` names, which every child is given.
fn given(name: &str) -> PathBuf {
    env::var_os(name)
        .map(PathBuf::from)
        .unwrap_or_else(|| panic!("{name} names no folder"))
}

/// Recovers the install in `destination` as the next start does, asking the world beyond the
/// folder through `settings` and telling `record`; tried again while the installation's lock is
/// held, until a killed child's lock is let go or [`LOCK_RELEASE_WAIT`] has passed.
fn recovered(
    destination: &Path,
    record: &mut dyn FolderRecord,
    settings: &InstallSettings<'_>,
) -> Result<RecoveryOutcome, RecoveryError> {
    let running = Version::parse(VERSION).expect("the launcher's version is a version");
    let cancel = Cancel::default();
    let job = Job {
        cancel: &cancel,
        progress: &NoProgress,
    };
    let begun = Instant::now();
    loop {
        match recover_install_with(destination, &running, record, &job, settings) {
            Err(RecoveryError::Lock(LockError::Busy)) if begun.elapsed() < LOCK_RELEASE_WAIT => {
                thread::sleep(LOCK_RETRY_INTERVAL);
            }
            outcome => return outcome,
        }
    }
}

/// A child's part of the kill tests, which does nothing unless [`KILL`] names a pass: plans and
/// installs into the folder [`KILL_DESTINATION`] names from what [`KILL_ROOT`] holds, or recovers
/// that folder when [`RECOVER`] is set, with the tests' stand-ins, failing at the pass [`FAIL`]
/// names when it names one, and killed at the pass [`KILL`] names. A job that runs past that pass
/// fails the test, so that the child ends with the kill's own code only when the kill came.
#[test]
fn killed_install() {
    let Some(kill) = env::var_os(KILL) else {
        return;
    };
    let kill = kill.into_string().expect("a pass is named in Unicode");
    let (point, at) = named_pass(&kill);
    let killed = KillAt::new(point, at);
    let failed = env::var(FAIL).ok().map(|name| {
        let (point, at) = named_pass(&name);
        FailAt::new(point, at)
    });
    let mut stand_ins: Vec<&dyn FaultPoints> = Vec::new();
    if let Some(failed) = &failed {
        stand_ins.push(failed);
    }
    stand_ins.push(&killed);
    let faults = Faults(&stand_ins);
    let settings = standing_in(&faults, &answering);
    let destination = given(KILL_DESTINATION);
    if env::var_os(RECOVER).is_some() {
        let outcome = recovered(&destination, &mut Unrecorded, &settings);
        panic!("the recovery ran past {kill}: {outcome:?}");
    }
    let root = given(KILL_ROOT);
    let mut image = GameCubeImage::open(&root.join(IMAGE)).unwrap();
    let mut launcher = Sections::of_path(&root.join(LAUNCHER))
        .unwrap()
        .expect("the launcher file carries sections");
    let plan = plan_install(&mut image, &mut launcher, &destination, VERSION).unwrap();
    assert!(plan.is_ready(), "{plan:?}");
    let cancel = Cancel::default();
    let job = Job {
        cancel: &cancel,
        progress: &NoProgress,
    };
    let installed = install_game_with(
        &plan,
        &mut image,
        &mut launcher,
        &mut Unrecorded,
        &job,
        &settings,
    );
    panic!("the install ran past {kill}: {installed:?}");
}

mod tests {
    use std::cell::Cell;
    use std::collections::BTreeMap;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::process::{Command, Stdio};
    use std::sync::PoisonError;

    #[cfg(windows)]
    use gdl_files::MAX_INSTALL_ROOT_UTF16;
    #[cfg(windows)]
    use gdl_fixtures::long_root;
    use gdl_fixtures::{folder_names, icon_iso, tree_entries};
    use serde_json::Value;
    use tempfile::TempDir;

    use super::{
        FAIL, IMAGE, KILL, KILL_DESTINATION, KILL_ROOT, LAUNCHER, RECOVER, pass_name, recovered,
    };
    use crate::faults::{FaultPoint, KILLED_EXIT_CODE, NoFaults, ONE_KILLED_CHILD};
    use crate::folder::{LOCK_FILE, RECEIPT_FILE, STATE_DIR};
    use crate::install::settings::InstallSettings;
    use crate::install::tests::{
        AsItWas, Heard, Inputs, answering, assert_no_alpha_name, destination_in, heard_of,
        journal_of, roots, standing_in, uninterrupted,
    };
    use crate::install::{INSTALL_STAGING_PREFIX, RecoveryOutcome};

    /// The test a child of this program runs alone.
    const KILLED_INSTALL: &str = "install::kill::killed_install";

    /// A pass of a fault point: the point and the pass, counted from 1.
    type Pass = (FaultPoint, u32);

    /// What the kill tests install from: the icon disc and a launcher file of this system's kind
    /// carrying the standard runtime.
    struct Installing {
        /// The disc's image.
        disc: Vec<u8>,
        /// The disc and the launcher file, as the install tests install them.
        inputs: Inputs,
    }

    impl Installing {
        fn new() -> Installing {
            let disc = icon_iso();
            let inputs = Inputs::of(&disc);
            Installing { disc, inputs }
        }

        /// The tree an install that nothing stops leaves.
        fn whole(&self) -> BTreeMap<String, Option<String>> {
            uninterrupted(&self.inputs)
        }
    }

    /// A folder a killed job installs into, or recovers, in a folder of the test's own that holds
    /// the image and the launcher file the install installs from; and what was there before.
    struct Killed {
        /// The folder of the test's own.
        root: TempDir,
        /// The folder installed into.
        destination: PathBuf,
        /// Whether it was there before the install, holding a file of the player's.
        holds_a_file: bool,
        /// What was there before the install.
        as_it_was: AsItWas,
    }

    impl Killed {
        /// A folder for a killed install of `installing`: one the install makes, two below the
        /// folder of the test's own, or, when `holds_a_file`, one there already holding a file of
        /// the player's.
        fn new(installing: &Installing, holds_a_file: bool) -> Killed {
            let root = Killed::holding(installing);
            let destination = destination_in(root.path(), holds_a_file);
            Killed::at(root, destination, holds_a_file)
        }

        /// A folder for a killed install of `installing` whose path is as long as an install root
        /// may be, there already and empty.
        #[cfg(windows)]
        fn longest(installing: &Installing) -> Killed {
            let root = Killed::holding(installing);
            let destination = long_root(root.path(), MAX_INSTALL_ROOT_UTF16).unwrap();
            Killed::at(root, destination, false)
        }

        /// A folder of the test's own holding the image and the launcher file of `installing`.
        fn holding(installing: &Installing) -> TempDir {
            let root = tempfile::tempdir().unwrap();
            fs::write(root.path().join(IMAGE), &installing.disc).unwrap();
            fs::write(root.path().join(LAUNCHER), &installing.inputs.launcher).unwrap();
            root
        }

        /// The folder `destination` for a killed install from what `root` holds, as it is before
        /// the install.
        fn at(root: TempDir, destination: PathBuf, holds_a_file: bool) -> Killed {
            let as_it_was = AsItWas::of(root.path(), &destination);
            Killed {
                root,
                destination,
                holds_a_file,
                as_it_was,
            }
        }

        /// Runs the job of a child of this test program, killed at `kill`, failing first at
        /// `fail` when it is given, and recovering the folder rather than installing into it when
        /// `recover`; and asserts that the kill ended the child, leaving no alpha installer's name.
        fn kill(&self, context: &str, kill: Pass, fail: Option<Pass>, recover: bool) {
            let mut child = Command::new(std::env::current_exe().unwrap());
            child
                .args(["--exact", KILLED_INSTALL, "--test-threads", "1"])
                .env(KILL, pass_name(kill.0, kill.1))
                .env(KILL_ROOT, self.root.path())
                .env(KILL_DESTINATION, &self.destination)
                .env_remove(FAIL)
                .env_remove(RECOVER)
                .stdin(Stdio::null());
            if let Some((point, at)) = fail {
                child.env(FAIL, pass_name(point, at));
            }
            if recover {
                child.env(RECOVER, "1");
            }
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                use windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;

                child.creation_flags(CREATE_NO_WINDOW);
            }
            let output = {
                let _one = ONE_KILLED_CHILD
                    .lock()
                    .unwrap_or_else(PoisonError::into_inner);
                child.output().unwrap()
            };
            assert_eq!(
                output.status.code(),
                Some(KILLED_EXIT_CODE),
                "{context}: {}{}",
                String::from_utf8_lossy(&output.stdout),
                String::from_utf8_lossy(&output.stderr)
            );
            self.holds_no_alpha_name(&format!("{context}, killed"));
        }

        /// Recovers the folder as the next start does, with the stand-ins, and gives what the
        /// recovery did, what the record heard and how many times the shell was told of the
        /// installed executable; and asserts that the recovery left no alpha installer's name.
        fn recover(&self, context: &str) -> (RecoveryOutcome, Vec<(&'static str, PathBuf)>, u32) {
            let mut record = Heard::default();
            let told = Cell::new(0);
            let notify = |_: &Path| told.set(told.get() + 1);
            let settings = InstallSettings {
                notify_shell: &notify,
                ..standing_in(&NoFaults, &answering)
            };
            let outcome = recovered(&self.destination, &mut record, &settings)
                .unwrap_or_else(|error| panic!("{context}: {error}"));
            self.holds_no_alpha_name(&format!("{context}, recovered"));
            (outcome, record.calls, told.get())
        }

        /// Asserts that no entry under the folder of the test's own, the folder installed into and
        /// the folders above it that the install made included, has one of the alpha installer's
        /// names, at any depth.
        #[track_caller]
        fn holds_no_alpha_name(&self, context: &str) {
            let walked = assert_no_alpha_name(self.root.path(), context);
            // The image and the launcher file are always there, so that a walk that met nothing
            // cannot pass.
            for source in [IMAGE, LAUNCHER] {
                assert!(
                    walked.contains(&PathBuf::from(source)),
                    "{context}: {walked:?}"
                );
            }
        }

        /// Asserts that the folder holds what it held before the install, outside the launcher's
        /// names, and none of those: when the install made it, it is gone with the folders above
        /// it that the install made.
        #[track_caller]
        fn holds_what_it_held(&self, context: &str) {
            self.as_it_was.holds(context);
        }

        /// Asserts that the folder is left as a state folder given up without its journal leaves
        /// it: holding only what it held, or, when the install made it, there and empty, since
        /// nothing says who made it.
        #[track_caller]
        fn is_left_without_state(&self, context: &str) {
            if self.holds_a_file {
                self.as_it_was.holds(context);
            } else {
                let left = folder_names(&self.destination).unwrap();
                assert!(left.is_empty(), "{context}: {left:?}");
            }
        }

        /// What the install's journal says, which the kill must have left.
        #[track_caller]
        fn journal(&self, context: &str) -> Value {
            journal_of(&self.destination).unwrap_or_else(|| panic!("{context}: no journal"))
        }

        /// How many of the install's roots are in place.
        fn in_place(&self) -> usize {
            roots()
                .iter()
                .filter(|root| self.destination.join(root).exists())
                .count()
        }

        /// Whether the state folder holds the install's receipt.
        fn receipted(&self) -> bool {
            self.state().join(RECEIPT_FILE).exists()
        }

        /// Whether the state folder holds an install's staging.
        fn staged(&self) -> bool {
            folder_names(&self.state())
                .unwrap()
                .iter()
                .any(|name| name.starts_with(INSTALL_STAGING_PREFIX))
        }

        /// The state folder.
        fn state(&self) -> PathBuf {
            self.destination.join(STATE_DIR)
        }
    }

    /// How many roots the install puts in place.
    fn root_count() -> u32 {
        u32::try_from(roots().len()).unwrap()
    }

    /// How many recorded roots the journal lists.
    fn recorded(journal: &Value) -> usize {
        journal["published"]
            .as_array()
            .expect("a journal lists the roots in place")
            .len()
    }

    /// The outcome of a recovery that took everything back.
    fn rolled_back() -> RecoveryOutcome {
        RecoveryOutcome::RolledBack { kept: Vec::new() }
    }

    #[test]
    fn a_kill_while_preparing_is_recovered() {
        let installing = Installing::new();
        for at in 1..=3 {
            for holds_a_file in [false, true] {
                let context = format!("prepare {at}, the folder holding a file: {holds_a_file}");
                let killed = Killed::new(&installing, holds_a_file);
                killed.kill(&context, (FaultPoint::Prepare, at), None, false);
                match at {
                    1 => assert!(
                        folder_names(&killed.state()).unwrap().is_empty(),
                        "{context}"
                    ),
                    2 => assert_eq!(
                        folder_names(&killed.state()).unwrap(),
                        [LOCK_FILE],
                        "{context}"
                    ),
                    _ => assert_eq!(killed.journal(&context)["phase"], "staging", "{context}"),
                }
                let (outcome, heard, told) = killed.recover(&context);
                if at < 3 {
                    // The state folder made, then locked, with no journal yet: given up, the
                    // folder left as the install found it, or empty when it made it.
                    assert_eq!(outcome, RecoveryOutcome::AbandonedRemoved, "{context}");
                    killed.is_left_without_state(&context);
                } else {
                    // The journal names the folders the install made, which go too.
                    assert_eq!(outcome, rolled_back(), "{context}");
                    killed.holds_what_it_held(&context);
                }
                assert_eq!(
                    heard,
                    heard_of(&killed.destination, &["forget"]),
                    "{context}"
                );
                assert_eq!(told, 0, "{context}");
            }
        }
    }

    #[test]
    fn a_kill_while_staging_is_taken_back() {
        let installing = Installing::new();
        // Windows sets the disc's icon into the executable, a step of its own.
        let steps = if cfg!(windows) { 5 } else { 4 };
        for at in 1..=steps {
            let context = format!("stage {at}");
            let killed = Killed::new(&installing, false);
            killed.kill(&context, (FaultPoint::Stage, at), None, false);
            assert_eq!(killed.journal(&context)["phase"], "staging", "{context}");
            let (outcome, heard, told) = killed.recover(&context);
            assert_eq!(outcome, rolled_back(), "{context}");
            killed.holds_what_it_held(&context);
            assert_eq!(
                heard,
                heard_of(&killed.destination, &["forget"]),
                "{context}"
            );
            assert_eq!(told, 0, "{context}");
        }
    }

    #[test]
    fn a_kill_while_publishing_is_finished() {
        let installing = Installing::new();
        let whole = installing.whole();
        let roots = root_count();
        // The journal moved to publishing; each root put in place, its move not yet recorded;
        // every root recorded; the receipt written.
        for at in 1..=roots + 3 {
            let context = format!("publish {at}");
            let killed = Killed::new(&installing, false);
            killed.kill(&context, (FaultPoint::Publish, at), None, false);
            let journal = killed.journal(&context);
            assert_eq!(journal["phase"], "publishing", "{context}");
            let moved = (at - 1).min(roots);
            assert_eq!(killed.in_place(), moved as usize, "{context}");
            assert_eq!(
                recorded(&journal),
                at.saturating_sub(2).min(roots) as usize,
                "{context}"
            );
            assert_eq!(killed.receipted(), at == roots + 3, "{context}");

            let (outcome, heard, told) = killed.recover(&context);
            assert!(
                matches!(outcome, RecoveryOutcome::Finished { .. }),
                "{context}: {outcome:?}"
            );
            assert_eq!(tree_entries(&killed.destination), whole, "{context}");
            assert_eq!(
                heard,
                heard_of(&killed.destination, &["installed"]),
                "{context}"
            );
            assert_eq!(told, 1, "{context}");
        }
    }

    #[test]
    fn a_kill_while_cleaning_up_is_finished() {
        let installing = Installing::new();
        let whole = installing.whole();
        // The journal committed, then the staging gone.
        for at in 1..=2 {
            let context = format!("cleanup {at}");
            let killed = Killed::new(&installing, false);
            killed.kill(&context, (FaultPoint::Cleanup, at), None, false);
            assert_eq!(killed.journal(&context)["phase"], "committed", "{context}");
            assert!(killed.receipted(), "{context}");
            assert_eq!(killed.staged(), at == 1, "{context}");

            // The killed job never told the record or the shell: the recovery does.
            let (outcome, heard, told) = killed.recover(&context);
            assert_eq!(outcome, RecoveryOutcome::CleanedUp, "{context}");
            assert_eq!(tree_entries(&killed.destination), whole, "{context}");
            assert_eq!(
                heard,
                heard_of(&killed.destination, &["installed"]),
                "{context}"
            );
            assert_eq!(told, 1, "{context}");
        }
    }

    #[test]
    fn a_kill_while_a_recovery_takes_an_install_back_is_recovered() {
        // A staging install's rollback takes the staging, the state files, the journal and the
        // state folder with its lock.
        const JOURNAL_GONE: u32 = 3;
        const GIVEN_UP: u32 = 4;
        let installing = Installing::new();
        for at in 1..=GIVEN_UP {
            let context = format!("a recovery's rollback {at}");
            let killed = Killed::new(&installing, false);
            killed.kill(&context, (FaultPoint::Stage, 2), None, false);
            killed.kill(&context, (FaultPoint::Rollback, at), None, true);
            if at < JOURNAL_GONE {
                assert_eq!(killed.journal(&context)["phase"], "staging", "{context}");
            } else if at == JOURNAL_GONE {
                assert!(journal_of(&killed.destination).is_none(), "{context}");
                assert!(killed.state().is_dir(), "{context}");
            } else {
                assert!(!killed.state().exists(), "{context}");
            }

            let (outcome, heard, told) = killed.recover(&context);
            if at < JOURNAL_GONE {
                assert_eq!(outcome, rolled_back(), "{context}");
                killed.holds_what_it_held(&context);
            } else if at == JOURNAL_GONE {
                // With the journal gone, nothing names the folders the install made, which stay.
                assert_eq!(outcome, RecoveryOutcome::AbandonedRemoved, "{context}");
                killed.is_left_without_state(&context);
            } else {
                assert_eq!(outcome, RecoveryOutcome::Nothing, "{context}");
                killed.is_left_without_state(&context);
            }
            let calls: &[&'static str] = if at < GIVEN_UP { &["forget"] } else { &[] };
            assert_eq!(heard, heard_of(&killed.destination, calls), "{context}");
            assert_eq!(told, 0, "{context}");
        }
    }

    #[test]
    fn a_kill_while_the_job_takes_itself_back_is_recovered() {
        // The fourth root in place, its move not yet recorded, when the job fails: its rollback
        // moves the four back, then takes the staging, the state files, the journal and the
        // state folder.
        const MOVED: u32 = 4;
        const JOURNAL_GONE: u32 = MOVED + 3;
        const GIVEN_UP: u32 = MOVED + 4;
        let installing = Installing::new();
        for at in 1..=GIVEN_UP {
            let context = format!("the job's rollback {at}");
            let killed = Killed::new(&installing, false);
            let fail = (FaultPoint::Publish, MOVED + 1);
            killed.kill(&context, (FaultPoint::Rollback, at), Some(fail), false);
            assert_eq!(
                killed.in_place(),
                MOVED.saturating_sub(at) as usize,
                "{context}"
            );
            if at < JOURNAL_GONE {
                let journal = killed.journal(&context);
                assert_eq!(journal["phase"], "rollingBack", "{context}");
            } else if at == JOURNAL_GONE {
                assert!(journal_of(&killed.destination).is_none(), "{context}");
                assert!(killed.state().is_dir(), "{context}");
            } else {
                assert!(!killed.state().exists(), "{context}");
            }

            let (outcome, heard, told) = killed.recover(&context);
            if at < JOURNAL_GONE {
                assert_eq!(outcome, rolled_back(), "{context}");
                killed.holds_what_it_held(&context);
            } else if at == JOURNAL_GONE {
                // With the journal gone, nothing names the folders the install made, which stay.
                assert_eq!(outcome, RecoveryOutcome::AbandonedRemoved, "{context}");
                killed.is_left_without_state(&context);
            } else {
                assert_eq!(outcome, RecoveryOutcome::Nothing, "{context}");
                killed.is_left_without_state(&context);
            }
            let calls: &[&'static str] = if at < GIVEN_UP { &["forget"] } else { &[] };
            assert_eq!(heard, heard_of(&killed.destination, calls), "{context}");
            assert_eq!(told, 0, "{context}");
        }
    }

    #[test]
    fn a_kill_while_a_recovery_finishes_a_publish_is_recovered() {
        let installing = Installing::new();
        let whole = installing.whole();
        let context = "a recovery's publish";
        let killed = Killed::new(&installing, false);
        // Three roots in place, the third's move not yet recorded.
        killed.kill(context, (FaultPoint::Publish, 4), None, false);
        assert_eq!(killed.in_place(), 3);
        assert_eq!(recorded(&killed.journal(context)), 2);
        // The recovery records the third, then puts the fourth in place and is killed before it
        // records that.
        killed.kill(context, (FaultPoint::Publish, 2), None, true);
        let journal = killed.journal(context);
        assert_eq!(journal["phase"], "publishing");
        assert_eq!(killed.in_place(), 4);
        assert_eq!(recorded(&journal), 3);

        let (outcome, heard, told) = killed.recover(context);
        assert!(
            matches!(outcome, RecoveryOutcome::Finished { .. }),
            "{outcome:?}"
        );
        assert_eq!(tree_entries(&killed.destination), whole);
        assert_eq!(heard, heard_of(&killed.destination, &["installed"]));
        assert_eq!(told, 1);
    }

    #[cfg(windows)]
    #[test]
    fn a_kill_under_the_longest_root_is_finished() {
        let installing = Installing::new();
        let whole = installing.whole();
        let context = "publish 4 under the longest root";
        let killed = Killed::longest(&installing);
        killed.kill(context, (FaultPoint::Publish, 4), None, false);
        assert_eq!(killed.in_place(), 3);

        let (outcome, heard, told) = killed.recover(context);
        assert!(
            matches!(outcome, RecoveryOutcome::Finished { .. }),
            "{outcome:?}"
        );
        assert_eq!(tree_entries(&killed.destination), whole);
        assert_eq!(heard, heard_of(&killed.destination, &["installed"]));
        assert_eq!(told, 1);
    }
}
