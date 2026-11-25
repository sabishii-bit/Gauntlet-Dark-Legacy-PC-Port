//! Kills at every step of an update, of its own rollback, and of the recoveries that follow one.
//! The update, or the recovery, runs in a child of this test program, which ends itself at the
//! pass of a fault point the test names, as a crash or a kill stops a job: nothing is unwound, and
//! what the job held, its lock among it, is let go only as the system lets go of what a program
//! that ended held. The test then recovers the folder as the next start does, and holds it to
//! what that recovery must leave: the installation as it was, or as the update leaves it. Where
//! the kill leaves the new launcher copy in place with every move made, that copy, a copy of this
//! program, recovers the folder itself, as the player's next start runs it. After every child and
//! every recovery, no entry in the test's folder has one of the alpha installer's names. The
//! children run one at a time.

use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::PathBuf;
use std::process;

use gdl_files::{Cancel, Job, NoProgress};

use super::tests::{INSTALLED, NEXT, answering, recover_by, release_of, standing_in, version};
use super::{UpdateFailure, UpdateOutcome, UpdateRequest, UpdateSource, update_with};
use crate::faults::{FailAt, FaultPoint, FaultPoints, Faults, KillAt};
use crate::sections::Sections;

/// The variable that names the pass a child of this test program is killed at, as
/// `<point>:<pass>`: the point by its name, the pass counted from 1. Unset, the child's job runs to
/// its end.
const KILL: &str = "GDL_UPDATE_KILL";

/// The variable that names the folder holding the launcher file a child updates from.
const KILL_ROOT: &str = "GDL_UPDATE_KILL_ROOT";

/// The variable that names the installation a child updates or recovers. Unset, as in a run of the
/// tests, [`killed_update`] does nothing.
const KILL_DESTINATION: &str = "GDL_UPDATE_KILL_DESTINATION";

/// The variable that names a pass a child's job fails at, as [`KILL`] names one.
const FAIL: &str = "GDL_UPDATE_FAIL";

/// The variable that, set, makes a child's job a recovery of the installation rather than an
/// update of it.
const RECOVER: &str = "GDL_UPDATE_RECOVER";

/// The variable that, set, makes a child update from a release of the feed's whose launcher file
/// is the one it is given, rather than from that file as the launcher that runs.
const RELEASE: &str = "GDL_UPDATE_RELEASE";

/// The launcher file a child updates from, in the folder [`KILL_ROOT`] names.
const SOURCE: &str = "next-launcher";

/// The exit code of a child whose job ended before the pass it was to be killed at, having written
/// what it ended in: none that a run that passed, a test that failed, a name that matched no test
/// or a kill gives.
const FINISHED_EXIT_CODE: i32 = 72;

/// What starts the line a child writes what its job ended in on.
const OUTCOME: &str = "outcome: ";

/// What a child writes of an update that stands.
const UPDATED: &str = "updated";

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

/// The folder the variable `name` names.
fn given(name: &str) -> PathBuf {
    env::var_os(name)
        .map(PathBuf::from)
        .unwrap_or_else(|| panic!("{name} names no folder"))
}

/// What an update ended in, as a child writes it: [`UPDATED`] for one that stands; otherwise the
/// failure's rule and what it left, or the outcome as it is.
fn update_outcome(result: &Result<UpdateOutcome, UpdateFailure>) -> String {
    match result {
        Ok(UpdateOutcome::Updated(_)) => UPDATED.to_owned(),
        Ok(other) => format!("{other:?}"),
        Err(failure) => format!("{}: {:?}", failure.rule(), failure.changes),
    }
}

/// Writes `outcome` on a line of its own, past the test harness's capture of what a test prints
/// and after the test's name the harness has begun a line with, and ends the child with
/// [`FINISHED_EXIT_CODE`].
fn finish(outcome: &str) -> ! {
    let mut out = io::stdout().lock();
    writeln!(out, "\n{OUTCOME}{outcome}")
        .and_then(|()| out.flush())
        .expect("a child's outcome is written");
    process::exit(FINISHED_EXIT_CODE)
}

/// A child's part of the kill tests, which does nothing unless [`KILL_DESTINATION`] names an
/// installation: updates it from the launcher file in the folder [`KILL_ROOT`] names, as the
/// launcher of that file's release, or, when [`RELEASE`] is set, as the launcher of the installed
/// release, from a release whose launcher file it is; or recovers it, when [`RECOVER`] is set, as
/// the launcher of the next release. The job has the tests' stand-ins, fails at the pass [`FAIL`]
/// names when it names one, and is killed at the pass [`KILL`] names when it names one. A job that
/// ends before that pass writes what it ended in and ends the child with [`FINISHED_EXIT_CODE`],
/// so that the child ends with the kill's own code only when the kill came.
#[test]
fn killed_update() {
    let Some(destination) = env::var_os(KILL_DESTINATION).map(PathBuf::from) else {
        return;
    };
    let killed = env::var(KILL).ok().map(|name| {
        let (point, at) = named_pass(&name);
        KillAt::new(point, at)
    });
    let failed = env::var(FAIL).ok().map(|name| {
        let (point, at) = named_pass(&name);
        FailAt::new(point, at)
    });
    let stand_ins: Vec<&dyn FaultPoints> = failed
        .iter()
        .map(|failed| failed as &dyn FaultPoints)
        .chain(killed.iter().map(|killed| killed as &dyn FaultPoints))
        .collect();
    let faults = Faults(&stand_ins);
    let settings = standing_in(&faults, &answering);
    let cancel = Cancel::default();
    let job = Job {
        cancel: &cancel,
        progress: &NoProgress,
    };
    if env::var_os(RECOVER).is_some() {
        let recovered = recover_by(&destination, NEXT, &job, &settings);
        finish(&format!("{recovered:?}"));
    }
    let file = given(KILL_ROOT).join(SOURCE);
    let updated = if env::var_os(RELEASE).is_some() {
        let (release, files) = release_of(NEXT, &fs::read(&file).unwrap());
        let running = version(INSTALLED);
        let request = UpdateRequest {
            root: &destination,
            running: &running,
            source: UpdateSource::Release {
                files: &files,
                release: &release,
            },
        };
        update_with(request, &job, &settings)
    } else {
        let mut sections = Sections::of_path(&file)
            .unwrap()
            .expect("the launcher file carries sections");
        let running = version(NEXT);
        let request = UpdateRequest {
            root: &destination,
            running: &running,
            source: UpdateSource::Bundled(&mut sections),
        };
        update_with(request, &job, &settings)
    };
    finish(&update_outcome(&updated));
}

mod tests {
    use std::cell::Cell;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::process::{Command, Stdio};
    use std::thread;
    use std::time::{Duration, Instant};

    #[cfg(windows)]
    use gdl_files::MAX_INSTALL_ROOT_UTF16;
    #[cfg(windows)]
    use gdl_fixtures::long_root;
    use gdl_fixtures::{InstallFixture, RuntimeFiles, append_sections};
    use serde_json::Value;
    use tempfile::TempDir;

    use super::{
        FAIL, FINISHED_EXIT_CODE, KILL, KILL_DESTINATION, KILL_ROOT, OUTCOME, RECOVER, RELEASE,
        SOURCE, UPDATED, pass_name,
    };
    use crate::faults::{FaultPoint, KILLED_EXIT_CODE, NoFaults};
    use crate::folder::{InstallFolder, JOURNAL_DIR, LockError, STATE_DIR};
    use crate::install::Changes;
    use crate::install::tests::assert_no_alpha_name;
    use crate::recover::{RecoverError, Recovered, UpdateRecovery};
    use crate::sections::{RUNTIME_SECTION, Sections};
    use crate::test_platform::this_platform;
    use crate::update::UPDATE_JOURNAL;
    use crate::update::tests::{
        Contents, INSTALLED, NEXT, add_players_files, answering, bundled_as, contents_of,
        downloads, journal_phase, launcher_copy, nothing_recovered, recover_simply, rolled_back,
        stagings, standing_in, update_simply, updated,
    };

    /// The test a child of this program runs alone.
    const KILLED_UPDATE: &str = "update::kill::killed_update";

    /// The folder of a test's own that the installation is laid out in.
    const INSTALLATION: &str = "install";

    /// How many installed files an update to [`next_runtime`] moves aside: every one it replaces
    /// or removes.
    const BACKED_UP: u32 = 7;

    /// How many new files it puts in place: every one it replaces or adds.
    const PUBLISHED: u32 = 7;

    /// How often the installation's lock is looked at again while a killed child's is not yet let
    /// go.
    const LOCK_RETRY_INTERVAL: Duration = Duration::from_millis(100);

    /// How long the lock a killed child held is waited for: Windows lets go of the locks of a
    /// program that ended some time after it ended.
    const LOCK_RELEASE_WAIT: Duration = Duration::from_secs(10);

    /// A pass of a fault point: the point and the pass, counted from 1.
    type Pass = (FaultPoint, u32);

    /// What a child of this test program does to the installation.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum ChildJob {
        /// Updates it from the launcher file beside it, as the launcher that file is.
        Update,
        /// Updates it from a release of the feed's whose launcher file that file is.
        Release,
        /// Recovers it, as the next start does.
        Recover,
    }

    /// This test program.
    fn this_program() -> PathBuf {
        std::env::current_exe().unwrap()
    }

    /// The runtime of the release the kill tests update to, after the installed one: it replaces,
    /// adds and removes installed files.
    fn next_runtime() -> RuntimeFiles {
        RuntimeFiles::standard(this_platform().as_str(), INSTALLED).newer(NEXT)
    }

    /// A launcher file the kill tests update from, carrying [`next_runtime`].
    struct Source(Vec<u8>);

    impl Source {
        /// A stand-in program of this system's kind, as the update tests update from.
        fn stand_in() -> Source {
            let dir = tempfile::tempdir().unwrap();
            let (path, _) = bundled_as(dir.path(), SOURCE, &next_runtime());
            Source(fs::read(path).unwrap())
        }

        /// This test program, so that the launcher copy an update from it puts in place runs this
        /// program's tests too.
        fn runnable() -> Source {
            let mut program = fs::read(this_program()).unwrap();
            append_sections(&mut program, &[(RUNTIME_SECTION, &next_runtime().zip())], 0);
            Source(program)
        }
    }

    /// An installation of the release before [`next_runtime`]'s, with the player's files beside
    /// it, that a child updates or recovers; in a folder of the test's own that holds the launcher
    /// file the update is from; and what the installation held before.
    struct Killed {
        /// The folder of the test's own.
        dir: TempDir,
        /// The installation.
        fixture: InstallFixture,
        /// What the installation held before anything was done to it.
        before: Contents,
    }

    impl Killed {
        /// An installation, to be updated from `source`, in a folder below the test's own.
        fn new(source: &Source) -> Killed {
            let dir = tempfile::tempdir().unwrap();
            let root = dir.path().join(INSTALLATION);
            Killed::at(dir, &root, source)
        }

        /// An installation, to be updated from `source`, whose path is as long as an
        /// installation's may be.
        #[cfg(windows)]
        fn longest(source: &Source) -> Killed {
            let dir = tempfile::tempdir().unwrap();
            let root = long_root(dir.path(), MAX_INSTALL_ROOT_UTF16).unwrap();
            Killed::at(dir, &root, source)
        }

        /// An installation at `root`, to be updated from `source`, which is written into `dir`.
        fn at(dir: TempDir, root: &Path, source: &Source) -> Killed {
            let runtime = RuntimeFiles::standard(this_platform().as_str(), INSTALLED);
            let fixture = InstallFixture::write(root, runtime).unwrap();
            add_players_files(root);
            fs::write(dir.path().join(SOURCE), &source.0).unwrap();
            let before = contents_of(root);
            Killed {
                dir,
                fixture,
                before,
            }
        }

        /// The installation's folder.
        fn root(&self) -> &Path {
            &self.fixture.root
        }

        /// The installation's state folder.
        fn state(&self) -> PathBuf {
            self.root().join(STATE_DIR)
        }

        /// The launcher copy at the installation's top.
        fn root_copy(&self) -> PathBuf {
            self.root().join(launcher_copy())
        }

        /// What the update's journal records; `None` when there is none.
        fn journal(&self) -> Option<Value> {
            let bytes = fs::read(self.state().join(JOURNAL_DIR).join(UPDATE_JOURNAL)).ok()?;
            Some(serde_json::from_slice(&bytes).unwrap())
        }

        /// Runs `program`, this test program or a copy of it, as a child that does `job` to the
        /// installation, killed at `kill` and failing first at `fail` when they are given, and
        /// asserts that the child left no alpha installer's name. Gives `None` when the kill ended
        /// it, and otherwise what its job ended in.
        fn run(
            &self,
            program: &Path,
            context: &str,
            job: ChildJob,
            kill: Option<Pass>,
            fail: Option<Pass>,
        ) -> Option<String> {
            let mut child = Command::new(program);
            child
                .args(["--exact", KILLED_UPDATE, "--test-threads", "1"])
                .env(KILL_ROOT, self.dir.path())
                .env(KILL_DESTINATION, self.root())
                .env_remove(RECOVER)
                .env_remove(RELEASE)
                .stdin(Stdio::null());
            for (name, pass) in [(KILL, kill), (FAIL, fail)] {
                match pass {
                    Some((point, at)) => child.env(name, pass_name(point, at)),
                    None => child.env_remove(name),
                };
            }
            match job {
                ChildJob::Update => {}
                ChildJob::Release => {
                    child.env(RELEASE, "1");
                }
                ChildJob::Recover => {
                    child.env(RECOVER, "1");
                }
            }
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                use windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;

                child.creation_flags(CREATE_NO_WINDOW);
            }
            let output = {
                let _one = crate::one_test_child();
                child.output().unwrap()
            };
            let stdout = String::from_utf8_lossy(&output.stdout);
            let written = || format!("{stdout}{}", String::from_utf8_lossy(&output.stderr));
            self.holds_no_alpha_name(&format!("{context}, the child ended"));
            match output.status.code() {
                Some(KILLED_EXIT_CODE) => None,
                Some(FINISHED_EXIT_CODE) => {
                    let outcome = stdout.lines().find_map(|line| line.strip_prefix(OUTCOME));
                    let outcome = outcome.unwrap_or_else(|| panic!("{context}: {}", written()));
                    Some(outcome.to_owned())
                }
                _ => panic!("{context}: {}: {}", output.status, written()),
            }
        }

        /// Runs `program` as [`run`](Self::run) does, and asserts that the kill at `kill` ended
        /// it.
        fn kill(
            &self,
            program: &Path,
            context: &str,
            job: ChildJob,
            kill: Pass,
            fail: Option<Pass>,
        ) {
            if let Some(outcome) = self.run(program, context, job, Some(kill), fail) {
                panic!(
                    "{context}: the job ran past {}: {outcome}",
                    pass_name(kill.0, kill.1)
                );
            }
        }

        /// Recovers the installation in this process as the next start does, with the stand-ins,
        /// once a killed child's lock is let go, and gives what the recovery did; and asserts that
        /// the recovery left no alpha installer's name.
        fn recover(&self, context: &str) -> Recovered {
            let folder = InstallFolder::at(self.root()).unwrap();
            let begun = Instant::now();
            loop {
                match folder.lock() {
                    Ok(lock) => {
                        drop(lock);
                        break;
                    }
                    Err(LockError::Busy) if begun.elapsed() < LOCK_RELEASE_WAIT => {
                        thread::sleep(LOCK_RETRY_INTERVAL);
                    }
                    Err(error) => panic!("{context}: {error}"),
                }
            }
            let recovered = recover_simply(self.root(), NEXT, &standing_in(&NoFaults, &answering))
                .unwrap_or_else(|error| panic!("{context}: {error}"));
            self.holds_no_alpha_name(&format!("{context}, recovered"));
            recovered
        }

        /// Asserts that no entry under the folder of the test's own, the installation and the
        /// launcher file it is updated from included, has one of the alpha installer's names, at
        /// any depth.
        #[track_caller]
        fn holds_no_alpha_name(&self, context: &str) {
            let walked = assert_no_alpha_name(self.dir.path(), context);
            // The launcher file is always there, so that a walk that met nothing cannot pass.
            assert!(
                walked.contains(&PathBuf::from(SOURCE)),
                "{context}: {walked:?}"
            );
        }

        /// What the next start's recovery gives once a kill stopped an update's rollback: the
        /// update taken back, with the installed launcher copy put back in place when the kill
        /// left it under its retired name; or nothing, once the rollback removed the journal.
        fn left_rolling_back(&self, context: &str) -> Recovered {
            let Some(journal) = self.journal() else {
                return nothing_recovered();
            };
            assert_eq!(journal["phase"], "rollingBack", "{context}");
            let retired = journal["retired"]
                .as_str()
                .expect("a journal names the installed launcher copy's retired name");
            rolled_back(self.state().join(retired).exists(), &[])
        }

        /// Updates the installation in this process from the launcher file beside it, nothing
        /// stopping the update, and gives what the installation then holds.
        fn updated(&self) -> Contents {
            let mut source = Sections::of_path(&self.dir.path().join(SOURCE))
                .unwrap()
                .expect("the launcher file carries sections");
            updated(update_simply(
                &self.fixture,
                &mut source,
                NEXT,
                &standing_in(&NoFaults, &answering),
            ));
            contents_of(self.root())
        }

        /// Asserts that the installation holds what it held before.
        #[track_caller]
        fn holds_what_it_held(&self, context: &str) {
            assert_eq!(contents_of(self.root()), self.before, "{context}");
        }

        /// Asserts that the installation holds `after`, what an update nothing stopped leaves.
        #[track_caller]
        fn holds(&self, after: &Contents, context: &str) {
            assert_eq!(contents_of(self.root()), *after, "{context}");
        }
    }

    /// What a recovery that did `update` and nothing else gives.
    fn recovered(update: UpdateRecovery) -> Recovered {
        Recovered {
            update,
            ..nothing_recovered()
        }
    }

    /// What a child that recovered the installation writes of a recovery that gave `recovered`.
    fn recovery_outcome(recovered: Recovered) -> String {
        format!("{:?}", Ok::<Recovered, RecoverError>(recovered))
    }

    /// Kills, as `job`, an update of a fresh installation from `source` at each pass of `point` in
    /// turn, until one runs past them all and the update stands; judges what each kill left with
    /// `judge`. Gives how many passes there were.
    fn kill_at_every_pass(
        source: &Source,
        job: ChildJob,
        point: FaultPoint,
        judge: &dyn Fn(&Killed, &str),
    ) -> u32 {
        let mut at = 0;
        loop {
            at += 1;
            let context = format!("{job:?} killed at {}", pass_name(point, at));
            let killed = Killed::new(source);
            if let Some(outcome) =
                killed.run(&this_program(), &context, job, Some((point, at)), None)
            {
                assert_eq!(outcome, UPDATED, "{context}");
                return at - 1;
            }
            judge(&killed, &context);
        }
    }

    /// Kills, as `job`, an update from `source` at every pass of its staging in turn: a kill
    /// before the journal is written leaves what the next start's sweep removes, a release's
    /// download folder from the first pass, as it was fetched before it; the kill once it is
    /// written leaves the staged update, which the next start takes back. Either way the
    /// installation is as it was.
    fn kill_at_every_staging_pass(source: &Source, job: ChildJob) {
        let journalled = Cell::new(false);
        let judge = |killed: &Killed, context: &str| {
            let root = killed.root();
            let expected = match journal_phase(root) {
                Some(phase) => {
                    assert_eq!(phase, "staged", "{context}");
                    assert!(!journalled.replace(true), "{context}");
                    rolled_back(false, &[])
                }
                None => {
                    assert!(!journalled.get(), "{context}");
                    let fetched = downloads(root);
                    assert_eq!(
                        fetched.len(),
                        usize::from(job == ChildJob::Release),
                        "{context}: {fetched:?}"
                    );
                    let mut swept = [fetched, stagings(root)].concat();
                    swept.sort();
                    Recovered {
                        swept,
                        ..nothing_recovered()
                    }
                }
            };
            assert_eq!(killed.recover(context), expected, "{context}");
            killed.holds_what_it_held(context);
        };
        kill_at_every_pass(source, job, FaultPoint::UpdateStage, &judge);
        assert!(journalled.get());
    }

    /// Kills an update of a fresh installation from `source` at `kill`, which leaves its journal
    /// in `phase`, then recovers it as the next start does, which must give `expected`; and gives
    /// the installation.
    fn killed_and_recovered(
        source: &Source,
        kill: Pass,
        phase: &str,
        expected: &Recovered,
    ) -> Killed {
        let context = pass_name(kill.0, kill.1);
        let killed = Killed::new(source);
        killed.kill(&this_program(), &context, ChildJob::Update, kill, None);
        assert_eq!(
            journal_phase(killed.root()).as_deref(),
            Some(phase),
            "{context}"
        );
        assert_eq!(killed.recover(&context), *expected, "{context}");
        killed
    }

    #[test]
    fn every_step_of_an_update_survives_a_kill() {
        let source = Source::stand_in();
        let after = Killed::new(&source).updated();
        kill_at_every_staging_pass(&source, ChildJob::Update);

        // Until every move is made, a kill is taken back: the journal recording the swap, each
        // installed file moved aside and each new one put in place, then the installed launcher
        // copy moved aside, which goes back in place.
        let taken_back = rolled_back(false, &[]);
        let context = "update.commit:1";
        killed_and_recovered(
            &source,
            (FaultPoint::UpdateCommit, 1),
            "swapping",
            &taken_back,
        )
        .holds_what_it_held(context);
        let judge = |killed: &Killed, context: &str| {
            assert_eq!(
                journal_phase(killed.root()).as_deref(),
                Some("swapping"),
                "{context}"
            );
            assert_eq!(killed.recover(context), taken_back, "{context}");
            killed.holds_what_it_held(context);
        };
        let moves = kill_at_every_pass(&source, ChildJob::Update, FaultPoint::UpdateMove, &judge);
        assert_eq!(moves, BACKED_UP + PUBLISHED);
        let killed = killed_and_recovered(
            &source,
            (FaultPoint::LauncherReplace, 1),
            "swapping",
            &rolled_back(true, &[]),
        );
        killed.holds_what_it_held("launcher.replace:1");
        assert_eq!(
            fs::read(killed.root_copy()).unwrap(),
            killed.fixture.launcher
        );

        // Once every move is made, the new launcher copy in place, the update is finished, its
        // receipt written by the recovery when the kill came before the update wrote it.
        let finished = recovered(UpdateRecovery::Finished);
        for kill in [
            (FaultPoint::LauncherReplace, 2),
            (FaultPoint::UpdateCommit, 2),
        ] {
            killed_and_recovered(&source, kill, "swapping", &finished)
                .holds(&after, &pass_name(kill.0, kill.1));
        }

        // Once the journal records the update committed, its cleanup is finished, from wherever
        // the kill stopped it.
        let cleaned_up = recovered(UpdateRecovery::CleanedUp);
        killed_and_recovered(
            &source,
            (FaultPoint::UpdateCommit, 3),
            "committed",
            &cleaned_up,
        )
        .holds(&after, "update.commit:3");
        let judge = |killed: &Killed, context: &str| {
            assert_eq!(
                journal_phase(killed.root()).as_deref(),
                Some("committed"),
                "{context}"
            );
            assert_eq!(killed.recover(context), cleaned_up, "{context}");
            killed.holds(&after, context);
        };
        let cleaning =
            kill_at_every_pass(&source, ChildJob::Update, FaultPoint::UpdateCleanup, &judge);
        assert!(cleaning > 0);
    }

    #[test]
    fn the_new_copy_finishes_the_update_that_put_it_in_place() {
        let source = Source::runnable();
        let after = Killed::new(&source).updated();
        let context = "launcher.replace:2, recovered by the new launcher copy";
        let killed = Killed::new(&source);
        killed.kill(
            &this_program(),
            context,
            ChildJob::Update,
            (FaultPoint::LauncherReplace, 2),
            None,
        );
        let copy = killed.root_copy();
        assert_eq!(fs::read(&copy).unwrap(), source.0);

        // The player's next start runs the new copy, which finishes the update with every move
        // made rather than take it back, so that it never moves itself.
        let outcome = killed.run(&copy, context, ChildJob::Recover, None, None);
        assert_eq!(
            outcome.as_deref(),
            Some(recovery_outcome(recovered(UpdateRecovery::Finished)).as_str()),
            "{context}"
        );
        killed.holds(&after, context);
    }

    #[test]
    fn a_kill_while_a_recovery_takes_an_update_back_is_recovered() {
        let source = Source::stand_in();
        // Every installed file moved aside and two new ones in place.
        let first = (FaultPoint::UpdateMove, BACKED_UP + 2);
        let mut at = 0;
        loop {
            at += 1;
            let context = format!(
                "{}, then a recovery's {}",
                pass_name(first.0, first.1),
                pass_name(FaultPoint::UpdateRollback, at)
            );
            let killed = Killed::new(&source);
            killed.kill(&this_program(), &context, ChildJob::Update, first, None);
            let kill = (FaultPoint::UpdateRollback, at);
            let outcome = killed.run(
                &this_program(),
                &context,
                ChildJob::Recover,
                Some(kill),
                None,
            );
            if let Some(outcome) = outcome {
                assert_eq!(
                    outcome,
                    recovery_outcome(rolled_back(false, &[])),
                    "{context}"
                );
                killed.holds_what_it_held(&context);
                break;
            }
            let expected = killed.left_rolling_back(&context);
            assert_eq!(killed.recover(&context), expected, "{context}");
            killed.holds_what_it_held(&context);
        }
        assert!(at > 1);
    }

    #[test]
    fn a_kill_while_the_job_takes_itself_back_is_recovered() {
        let source = Source::stand_in();
        // The update fails as the new launcher copy is put in place, every move made: its rollback
        // moves the new copy aside, the installed one back and every file back.
        let fail = (FaultPoint::LauncherReplace, 2);
        let mut at = 0;
        loop {
            at += 1;
            let context = format!(
                "{} failed, then {}",
                pass_name(fail.0, fail.1),
                pass_name(FaultPoint::UpdateRollback, at)
            );
            let killed = Killed::new(&source);
            let kill = (FaultPoint::UpdateRollback, at);
            let outcome = killed.run(
                &this_program(),
                &context,
                ChildJob::Update,
                Some(kill),
                Some(fail),
            );
            if let Some(outcome) = outcome {
                assert_eq!(
                    outcome,
                    format!("io: {:?}", Changes::RolledBack),
                    "{context}"
                );
                killed.holds_what_it_held(&context);
                break;
            }
            let expected = killed.left_rolling_back(&context);
            assert_eq!(killed.recover(&context), expected, "{context}");
            killed.holds_what_it_held(&context);
        }
        assert!(at > 1);
    }

    #[test]
    fn a_kill_while_a_recovery_finishes_an_update_is_recovered() {
        let stand_in = Source::stand_in();
        let runnable = Source::runnable();
        // The receipt written, and, before it, the new launcher copy put in place, which then
        // recovers the folder itself as the player's next start runs it.
        for (source, first) in [
            (&stand_in, (FaultPoint::UpdateCommit, 2)),
            (&runnable, (FaultPoint::LauncherReplace, 2)),
        ] {
            let after = Killed::new(source).updated();
            for second in [
                (FaultPoint::UpdateCommit, 3),
                (FaultPoint::UpdateCleanup, 1),
            ] {
                let context = format!(
                    "{}, then a recovery's {}",
                    pass_name(first.0, first.1),
                    pass_name(second.0, second.1)
                );
                let killed = Killed::new(source);
                killed.kill(&this_program(), &context, ChildJob::Update, first, None);
                let recovering = if first.0 == FaultPoint::LauncherReplace {
                    killed.root_copy()
                } else {
                    this_program()
                };
                killed.kill(&recovering, &context, ChildJob::Recover, second, None);
                assert_eq!(
                    journal_phase(killed.root()).as_deref(),
                    Some("committed"),
                    "{context}"
                );
                assert_eq!(
                    killed.recover(&context),
                    recovered(UpdateRecovery::CleanedUp),
                    "{context}"
                );
                killed.holds(&after, &context);
            }
        }
    }

    #[test]
    fn a_kill_while_a_release_is_fetched_or_staged_is_swept_or_taken_back() {
        kill_at_every_staging_pass(&Source::stand_in(), ChildJob::Release);
    }

    #[test]
    fn a_kill_while_a_release_is_swapped_taken_back_or_cleaned_up_is_recovered() {
        let source = Source::stand_in();
        let after = Killed::new(&source).updated();
        let job = ChildJob::Release;

        // A move made, the journal naming the download folder: the next start takes the update
        // back from it, the download folder with it.
        let context = "a release's update.move:1";
        let killed = Killed::new(&source);
        killed.kill(
            &this_program(),
            context,
            job,
            (FaultPoint::UpdateMove, 1),
            None,
        );
        assert_eq!(
            journal_phase(killed.root()).as_deref(),
            Some("swapping"),
            "{context}"
        );
        assert_eq!(downloads(killed.root()).len(), 1, "{context}");
        assert_eq!(
            killed.recover(context),
            rolled_back(false, &[]),
            "{context}"
        );
        killed.holds_what_it_held(context);

        // Whether the download folder the journal names is still there, once a kill left the
        // journal standing.
        let download_left = |killed: &Killed| {
            let journal = killed.journal()?;
            let download = journal["download"]
                .as_str()
                .expect("a release's journal names its download folder");
            Some(killed.state().join(download).exists())
        };

        // The update's own rollback once a move failed, killed at each of its passes in turn,
        // the download folder's removal among them: the next start finishes taking it back.
        let fail = (FaultPoint::UpdateMove, 1);
        let mut taken_past_download = false;
        let mut at = 0;
        loop {
            at += 1;
            let context = format!(
                "a release's {} failed, then {}",
                pass_name(fail.0, fail.1),
                pass_name(FaultPoint::UpdateRollback, at)
            );
            let killed = Killed::new(&source);
            let kill = (FaultPoint::UpdateRollback, at);
            let outcome = killed.run(&this_program(), &context, job, Some(kill), Some(fail));
            if let Some(outcome) = outcome {
                assert_eq!(
                    outcome,
                    format!("io: {:?}", Changes::RolledBack),
                    "{context}"
                );
                killed.holds_what_it_held(&context);
                break;
            }
            taken_past_download |= download_left(&killed) == Some(false);
            let expected = killed.left_rolling_back(&context);
            assert_eq!(killed.recover(&context), expected, "{context}");
            killed.holds_what_it_held(&context);
        }
        assert!(at > 1);
        assert!(taken_past_download);

        // Its cleanup killed at each of its passes in turn: the next start finishes it, also
        // once the download folder the journal names is gone.
        let download_gone = Cell::new(false);
        let judge = |killed: &Killed, context: &str| {
            assert_eq!(
                journal_phase(killed.root()).as_deref(),
                Some("committed"),
                "{context}"
            );
            if download_left(killed) == Some(false) {
                download_gone.set(true);
            }
            assert_eq!(
                killed.recover(context),
                recovered(UpdateRecovery::CleanedUp),
                "{context}"
            );
            killed.holds(&after, context);
        };
        let cleaning = kill_at_every_pass(&source, job, FaultPoint::UpdateCleanup, &judge);
        assert!(cleaning > 0);
        assert!(download_gone.get());
    }

    #[cfg(windows)]
    #[test]
    fn a_long_root_update_survives_a_kill() {
        let source = Source::stand_in();
        let after = Killed::new(&source).updated();
        let context = "update.move:3 under the longest root";
        let killed = Killed::longest(&source);
        killed.kill(
            &this_program(),
            context,
            ChildJob::Update,
            (FaultPoint::UpdateMove, 3),
            None,
        );
        assert_eq!(journal_phase(killed.root()).as_deref(), Some("swapping"));
        assert_eq!(killed.recover(context), rolled_back(false, &[]));
        killed.holds_what_it_held(context);

        // Nothing stopping it, the same update goes through there.
        assert_eq!(killed.updated(), after);
    }
}
