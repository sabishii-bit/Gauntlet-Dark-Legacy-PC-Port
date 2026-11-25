//! The named points a job passes each time it has made a step that must survive its end, gathered
//! in one place so that every job passes the same ones, and the stand-ins its tests put there to
//! fail, stop, panic or kill the job at a pass and see what it leaves. A build with the test hooks
//! also kills itself at the pass a variable of its environment names, so that a rehearsal of the
//! built program sees what each kill leaves.

#[cfg(test)]
use std::cell::Cell;
#[cfg(feature = "test-hooks")]
use std::ffi::OsStr;
use std::io;
#[cfg(feature = "test-hooks")]
use std::sync::OnceLock;
#[cfg(feature = "test-hooks")]
use std::sync::atomic::{AtomicU32, Ordering};

/// A point a job passes each time it has made a step that must survive its end: where a test
/// fails, stops or kills the job to see what it leaves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FaultPoint {
    /// The install's state folder, lock or first journal was made.
    Prepare,
    /// A phase of staging was done.
    Stage,
    /// The journal moved to publishing, a root was put in place, every root was recorded, or the
    /// receipt was written.
    Publish,
    /// The journal moved to committed, or the staging went.
    Cleanup,
    /// A root went back into the staging, or a step of taking the install back was done: the
    /// staging, the state files, the journal or the state folder went.
    Rollback,
    /// A step of an update's staging was done: a release's files downloaded, its source
    /// verified, the installed files hashed again, the new files, the launcher copy or the icon
    /// staged, or its journal written.
    UpdateStage,
    /// An installed file moved aside into the update's backup, or a new one into place.
    UpdateMove,
    /// The installed launcher copy moved aside, or the new one into its place.
    LauncherReplace,
    /// The update's journal moved to swapping, its receipt was written, or its journal moved to
    /// committed.
    UpdateCommit,
    /// A step of an update's cleanup was done: its staging, its backup or its download went, or
    /// the retired launcher copy's removal was tried.
    UpdateCleanup,
    /// A step of taking an update back was done.
    UpdateRollback,
}

#[cfg(any(test, feature = "test-hooks"))]
impl FaultPoint {
    /// Every point a job passes.
    pub(crate) const ALL: [FaultPoint; 11] = [
        FaultPoint::Prepare,
        FaultPoint::Stage,
        FaultPoint::Publish,
        FaultPoint::Cleanup,
        FaultPoint::Rollback,
        FaultPoint::UpdateStage,
        FaultPoint::UpdateMove,
        FaultPoint::LauncherReplace,
        FaultPoint::UpdateCommit,
        FaultPoint::UpdateCleanup,
        FaultPoint::UpdateRollback,
    ];

    /// The point's name, as a test names the point it stops a job at.
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            FaultPoint::Prepare => "install.prepare",
            FaultPoint::Stage => "install.stage",
            FaultPoint::Publish => "install.publish",
            FaultPoint::Cleanup => "install.cleanup",
            FaultPoint::Rollback => "install.rollback",
            FaultPoint::UpdateStage => "update.stage",
            FaultPoint::UpdateMove => "update.move",
            FaultPoint::LauncherReplace => "launcher.replace",
            FaultPoint::UpdateCommit => "update.commit",
            FaultPoint::UpdateCleanup => "update.cleanup",
            FaultPoint::UpdateRollback => "update.rollback",
        }
    }

    /// The point whose name, as [`as_str`](Self::as_str) gives it, is `name` in the same letter
    /// case, as a test names the point it stops a job at; `None` for a name no point has.
    pub(crate) fn named(name: &str) -> Option<FaultPoint> {
        FaultPoint::ALL
            .into_iter()
            .find(|point| point.as_str() == name)
    }
}

/// The exit code of a test program that [`KillAt`] ended, and of a build with the test hooks that
/// [`FAULT_VARIABLE`] ended: none that a run that passed, a test that failed or a name that
/// matched no test gives.
#[cfg(any(test, feature = "test-hooks"))]
pub(crate) const KILLED_EXIT_CODE: i32 = 71;

/// What parts a pass's point from its count where [`FAULT_VARIABLE`] names one.
#[cfg(any(test, feature = "test-hooks"))]
const PASS_SEPARATOR: char = ':';

/// The pass `name` names, `<point>:<pass>`: a point by its name, as [`FaultPoint::as_str`] gives
/// it, and a pass counted from 1, in ASCII digits alone; `None` for anything else.
#[cfg(any(test, feature = "test-hooks"))]
fn named_pass(name: &str) -> Option<(FaultPoint, u32)> {
    let (point, at) = name.split_once(PASS_SEPARATOR)?;
    let point = FaultPoint::named(point)?;
    if at.is_empty() || !at.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    let at: u32 = at.parse().ok()?;
    (at != 0).then_some((point, at))
}

/// Where a job passes its [`FaultPoint`]s: an error from a pass fails the job there, as the
/// step after it would have failed.
pub(crate) trait FaultPoints {
    /// The job passes `point`.
    fn pass(&self, point: FaultPoint) -> io::Result<()>;
}

/// The points of a job nothing stops: every pass goes on.
#[cfg(any(test, not(feature = "test-hooks")))]
pub(crate) struct NoFaults;

#[cfg(any(test, not(feature = "test-hooks")))]
impl FaultPoints for NoFaults {
    fn pass(&self, _: FaultPoint) -> io::Result<()> {
        Ok(())
    }
}

/// The variable a build with the test hooks reads the pass it kills itself at from, as
/// `<point>:<pass>`: the point by its name, such as `update.move`, and the pass counted from 1.
#[cfg(feature = "test-hooks")]
pub const FAULT_VARIABLE: &str = "GDL_LAUNCHER_FAULT";

/// The pass a build with the test hooks kills itself at, as [`FAULT_VARIABLE`] names it when it
/// is first looked at, and how many times that pass's point has been passed since.
#[cfg(feature = "test-hooks")]
pub(crate) struct EnvFaults {
    /// The pass named, read once; `None` when the variable names none.
    named: OnceLock<Option<(FaultPoint, u32)>>,
    /// How many times the named point has been passed.
    passed: AtomicU32,
}

#[cfg(feature = "test-hooks")]
impl FaultPoints for EnvFaults {
    /// Ends the process with [`KILLED_EXIT_CODE`] at the named pass, as a crash or a kill stops a
    /// job: nothing is unwound, and what the job holds, its lock among it, is let go only as the
    /// system lets go of what a program that ended held. Every other pass goes on, and so does
    /// every pass while the variable is unset, empty or names no pass.
    fn pass(&self, point: FaultPoint) -> io::Result<()> {
        let named = self.named.get_or_init(|| {
            judged(std::env::var_os(FAULT_VARIABLE).as_deref())
                .ok()
                .flatten()
        });
        if let Some((named, at)) = *named
            && named == point
            && self.passed.fetch_add(1, Ordering::SeqCst).saturating_add(1) == at
        {
            std::process::exit(KILLED_EXIT_CODE);
        }
        Ok(())
    }
}

/// The fault points a build with the test hooks passes: the one [`FAULT_VARIABLE`] names.
#[cfg(feature = "test-hooks")]
pub(crate) static ENV_FAULTS: EnvFaults = EnvFaults {
    named: OnceLock::new(),
    passed: AtomicU32::new(0),
};

/// What `value`, the value of [`FAULT_VARIABLE`], names: no pass when it is unset or empty, a pass
/// when it names one, or the value as text when it names none.
#[cfg(feature = "test-hooks")]
fn judged(value: Option<&OsStr>) -> Result<Option<(FaultPoint, u32)>, String> {
    match value {
        None => Ok(None),
        Some(value) if value.is_empty() => Ok(None),
        Some(value) => value
            .to_str()
            .and_then(named_pass)
            .map(Some)
            .ok_or_else(|| value.to_string_lossy().into_owned()),
    }
}

/// Whether [`FAULT_VARIABLE`] is unset, empty or names a pass a build with the test hooks can kill
/// itself at; `Err` with what it holds otherwise, which such a build refuses before it runs
/// anything, since a pass it treats as unset would kill nothing.
#[cfg(feature = "test-hooks")]
pub fn check_fault_variable() -> Result<(), String> {
    judged(std::env::var_os(FAULT_VARIABLE).as_deref()).map(|_| ())
}

/// The fault points the launcher's own settings pass: in a build with the test hooks those
/// [`FAULT_VARIABLE`] names, and none otherwise.
pub(crate) fn shipped_faults() -> &'static dyn FaultPoints {
    #[cfg(feature = "test-hooks")]
    {
        &ENV_FAULTS
    }
    #[cfg(not(feature = "test-hooks"))]
    {
        &NoFaults
    }
}

/// What a pass that a test fails answers.
#[cfg(test)]
pub(crate) const INJECTED: &str = "injected";

/// The passes of one point a test waits for, counted from 1: how a stand-in finds the pass it acts
/// at.
#[cfg(test)]
struct Passes {
    /// The point counted.
    point: FaultPoint,
    /// The pass waited for.
    at: u32,
    /// How many times the point has been passed so far.
    passed: Cell<u32>,
}

#[cfg(test)]
impl Passes {
    fn new(point: FaultPoint, at: u32) -> Passes {
        Passes {
            point,
            at,
            passed: Cell::new(0),
        }
    }

    /// Counts a pass of `point`, and says whether it is the one waited for.
    fn reached(&self, point: FaultPoint) -> bool {
        if point != self.point {
            return false;
        }
        let passed = self.passed.get() + 1;
        self.passed.set(passed);
        passed == self.at
    }
}

/// Fails the `at`-th pass of a point with [`INJECTED`], as the step after it would have failed.
#[cfg(test)]
pub(crate) struct FailAt(Passes);

#[cfg(test)]
impl FailAt {
    pub(crate) fn new(point: FaultPoint, at: u32) -> FailAt {
        FailAt(Passes::new(point, at))
    }
}

#[cfg(test)]
impl FaultPoints for FailAt {
    fn pass(&self, point: FaultPoint) -> io::Result<()> {
        if self.0.reached(point) {
            Err(io::Error::other(INJECTED))
        } else {
            Ok(())
        }
    }
}

/// Runs `act` at the `at`-th pass of a point and answers what it answers: how a test changes the
/// folder while the job runs.
#[cfg(test)]
pub(crate) struct OnPass<'a> {
    passes: Passes,
    act: &'a dyn Fn() -> io::Result<()>,
}

#[cfg(test)]
impl<'a> OnPass<'a> {
    pub(crate) fn new(point: FaultPoint, at: u32, act: &'a dyn Fn() -> io::Result<()>) -> Self {
        OnPass {
            passes: Passes::new(point, at),
            act,
        }
    }
}

#[cfg(test)]
impl FaultPoints for OnPass<'_> {
    fn pass(&self, point: FaultPoint) -> io::Result<()> {
        if self.passes.reached(point) {
            (self.act)()
        } else {
            Ok(())
        }
    }
}

/// Panics at the `at`-th pass of a point, as a fault in the launcher would unwind the job there.
#[cfg(test)]
pub(crate) struct PanicAt(Passes);

#[cfg(test)]
impl PanicAt {
    pub(crate) fn new(point: FaultPoint, at: u32) -> PanicAt {
        PanicAt(Passes::new(point, at))
    }
}

#[cfg(test)]
impl FaultPoints for PanicAt {
    fn pass(&self, point: FaultPoint) -> io::Result<()> {
        if self.0.reached(point) {
            panic!("the test panics at pass {} of {point:?}", self.0.at);
        }
        Ok(())
    }
}

/// Ends the process with [`KILLED_EXIT_CODE`] at the `at`-th pass of a point, as a crash or a kill
/// stops the job there: nothing is unwound, and what the job holds, its lock among it, is closed
/// and let go only as the system lets go of what a program that ended held.
#[cfg(test)]
pub(crate) struct KillAt(Passes);

#[cfg(test)]
impl KillAt {
    pub(crate) fn new(point: FaultPoint, at: u32) -> KillAt {
        KillAt(Passes::new(point, at))
    }
}

#[cfg(test)]
impl FaultPoints for KillAt {
    fn pass(&self, point: FaultPoint) -> io::Result<()> {
        if self.0.reached(point) {
            std::process::exit(KILLED_EXIT_CODE);
        }
        Ok(())
    }
}

/// Several stand-ins at once: every pass is told to each, in order, so that each counts it, and
/// the first failure among them is the pass's.
#[cfg(test)]
pub(crate) struct Faults<'a>(pub(crate) &'a [&'a dyn FaultPoints]);

#[cfg(test)]
impl FaultPoints for Faults<'_> {
    fn pass(&self, point: FaultPoint) -> io::Result<()> {
        let mut passed = Ok(());
        for faults in self.0 {
            let answer = faults.pass(point);
            if passed.is_ok() {
                passed = answer;
            }
        }
        passed
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::io;
    use std::panic::{AssertUnwindSafe, catch_unwind};
    #[cfg(feature = "test-hooks")]
    use std::process::{Command, Stdio};

    #[cfg(feature = "test-hooks")]
    use super::{FAULT_VARIABLE, KILLED_EXIT_CODE, check_fault_variable, shipped_faults};
    use super::{
        FailAt, FaultPoint, FaultPoints, Faults, INJECTED, KillAt, NoFaults, OnPass, PanicAt,
        named_pass,
    };

    /// The variable that makes a child of this test program running [`CHILD_TEST`] pass every
    /// point the launcher's own settings pass. Unset, as in a run of the tests, that test does
    /// nothing.
    #[cfg(feature = "test-hooks")]
    const FAULT_CHILD: &str = "GDL_FAULT_VARIABLE_CHILD";

    /// The test such a child runs alone.
    #[cfg(feature = "test-hooks")]
    const CHILD_TEST: &str = "faults::tests::a_child_passes_every_point_twice";

    /// How many times the child passes each point, in the order a job's points are listed.
    #[cfg(feature = "test-hooks")]
    const CHILD_PASSES: u32 = 2;

    /// The exit code of a child that passed every point as often as it does, its variable taken.
    #[cfg(feature = "test-hooks")]
    const PASSED_EVERY_POINT: i32 = 76;

    /// The exit code of a child that passed every point as often as it does, its variable refused.
    #[cfg(feature = "test-hooks")]
    const VARIABLE_REFUSED: i32 = 77;

    #[test]
    fn a_named_pass_reads_back() {
        assert_eq!(
            named_pass("update.move:3"),
            Some((FaultPoint::UpdateMove, 3))
        );
        for point in FaultPoint::ALL {
            let name = format!("{}:1", point.as_str());
            assert_eq!(named_pass(&name), Some((point, 1)), "{name:?}");
        }
        assert_eq!(
            named_pass("install.publish:4294967295"),
            Some((FaultPoint::Publish, u32::MAX))
        );
        // No pass, nought, a point no job passes, another letter case, a count that is no
        // decimal of ASCII digits alone or past what a count holds, and a second count.
        for name in [
            "update.move:0",
            "update.move:00",
            "update.move",
            "update.move:",
            ":3",
            "nowhere:1",
            "Update.Move:3",
            "update.move:x",
            "update.move:-1",
            "update.move:+1",
            "update.move: 3",
            "update.move:3 ",
            "update.move:\u{0663}",
            "update.move:4294967296",
            "update.move:3:4",
        ] {
            assert_eq!(named_pass(name), None, "{name:?}");
        }
    }

    /// The child's part of [`the_fault_variable_is_checked`], which does nothing unless
    /// [`FAULT_CHILD`] is set: it checks the variable as a build with the test hooks checks it
    /// first, then passes every point [`CHILD_PASSES`] times through the launcher's own fault
    /// points, and exits with [`PASSED_EVERY_POINT`] or [`VARIABLE_REFUSED`] unless one killed it.
    #[cfg(feature = "test-hooks")]
    #[test]
    fn a_child_passes_every_point_twice() {
        if std::env::var_os(FAULT_CHILD).is_none() {
            return;
        }
        let checked = check_fault_variable();
        let faults = shipped_faults();
        for point in FaultPoint::ALL {
            for _ in 0..CHILD_PASSES {
                faults.pass(point).unwrap();
            }
        }
        std::process::exit(if checked.is_ok() {
            PASSED_EVERY_POINT
        } else {
            VARIABLE_REFUSED
        });
    }

    #[cfg(feature = "test-hooks")]
    #[test]
    fn the_fault_variable_is_checked() {
        let _one = crate::one_test_child();
        // The pass named kills the child as it is reached, whatever came before it; a pass past
        // the child's last kills nothing; and a value that names no pass is refused and kills
        // nothing, as an unset variable and an empty one kill nothing.
        let cases: &[(Option<&str>, i32)] = &[
            (None, PASSED_EVERY_POINT),
            (Some(""), PASSED_EVERY_POINT),
            (Some("update.move:1"), KILLED_EXIT_CODE),
            (Some("update.move:2"), KILLED_EXIT_CODE),
            (Some("update.move:3"), PASSED_EVERY_POINT),
            (Some("install.prepare:2"), KILLED_EXIT_CODE),
            (Some("update.rollback:2"), KILLED_EXIT_CODE),
            (Some("update.move:0"), VARIABLE_REFUSED),
            (Some("update.move"), VARIABLE_REFUSED),
            (Some("nowhere:1"), VARIABLE_REFUSED),
            (Some("update.move:x"), VARIABLE_REFUSED),
        ];
        for &(value, expected) in cases {
            let mut command = Command::new(std::env::current_exe().unwrap());
            command
                .args(["--exact", CHILD_TEST, "--test-threads", "1"])
                .env(FAULT_CHILD, "1")
                .stdin(Stdio::null());
            match value {
                Some(value) => command.env(FAULT_VARIABLE, value),
                None => command.env_remove(FAULT_VARIABLE),
            };
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                use windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;

                command.creation_flags(CREATE_NO_WINDOW);
            }
            let output = command.output().unwrap();
            assert_eq!(
                output.status.code(),
                Some(expected),
                "{value:?}: {}{}",
                String::from_utf8_lossy(&output.stdout),
                String::from_utf8_lossy(&output.stderr)
            );
        }
    }

    #[test]
    fn every_point_is_named_once() {
        let names = FaultPoint::ALL.map(FaultPoint::as_str);
        assert_eq!(
            names,
            [
                "install.prepare",
                "install.stage",
                "install.publish",
                "install.cleanup",
                "install.rollback",
                "update.stage",
                "update.move",
                "launcher.replace",
                "update.commit",
                "update.cleanup",
                "update.rollback",
            ]
        );
    }

    #[test]
    fn every_point_is_found_by_its_name_alone() {
        for point in FaultPoint::ALL {
            assert_eq!(FaultPoint::named(point.as_str()), Some(point), "{point:?}");
        }
        for name in [
            "",
            "nowhere",
            "update",
            "Update.Move",
            "update.move:3",
            " update.move",
        ] {
            assert_eq!(FaultPoint::named(name), None, "{name:?}");
        }
    }

    #[test]
    fn a_job_nothing_stops_passes_every_point() {
        for point in FaultPoint::ALL {
            assert!(NoFaults.pass(point).is_ok(), "{point:?}");
        }
    }

    #[test]
    fn a_kill_waits_for_its_pass_of_its_point() {
        // The process ends at the kill's pass, which a test of its own cannot outlive: the kill
        // tests run it in a child. Every pass before it, and every other point's, goes on.
        let kill = KillAt::new(FaultPoint::Cleanup, 3);
        for _ in 0..2 {
            for point in FaultPoint::ALL {
                assert!(kill.pass(point).is_ok(), "{point:?}");
            }
        }
        for point in FaultPoint::ALL {
            if point != FaultPoint::Cleanup {
                assert!(kill.pass(point).is_ok(), "{point:?}");
            }
        }
    }

    #[test]
    fn a_failure_comes_at_its_pass_of_its_point_alone() {
        let fail = FailAt::new(FaultPoint::Publish, 2);
        // The other points' passes are not counted.
        for point in FaultPoint::ALL {
            if point != FaultPoint::Publish {
                assert!(fail.pass(point).is_ok(), "{point:?}");
            }
        }
        assert!(fail.pass(FaultPoint::Publish).is_ok());
        let refused = fail.pass(FaultPoint::Publish).unwrap_err();
        assert_eq!(refused.to_string(), INJECTED);
        // Only that pass fails.
        assert!(fail.pass(FaultPoint::Publish).is_ok());
    }

    #[test]
    fn an_act_runs_at_its_pass_and_answers_for_it() {
        let acted = Cell::new(0);
        let act = || {
            acted.set(acted.get() + 1);
            Err(io::Error::other("the act refuses"))
        };
        let on = OnPass::new(FaultPoint::Stage, 3, &act);
        for _ in 0..2 {
            assert!(on.pass(FaultPoint::Stage).is_ok());
        }
        assert!(on.pass(FaultPoint::Cleanup).is_ok());
        assert_eq!(acted.get(), 0);
        let refused = on.pass(FaultPoint::Stage).unwrap_err();
        assert_eq!(refused.to_string(), "the act refuses");
        assert!(on.pass(FaultPoint::Stage).is_ok());
        assert_eq!(acted.get(), 1);
    }

    #[test]
    fn a_panic_comes_at_its_pass() {
        let panic = PanicAt::new(FaultPoint::Rollback, 2);
        assert!(panic.pass(FaultPoint::Rollback).is_ok());
        assert!(panic.pass(FaultPoint::Prepare).is_ok());
        let unwound = catch_unwind(AssertUnwindSafe(|| panic.pass(FaultPoint::Rollback)));
        assert!(unwound.is_err());
    }

    #[test]
    fn every_stand_in_hears_every_pass_and_the_first_failure_answers() {
        let acted = Cell::new(0);
        let act = || {
            acted.set(acted.get() + 1);
            Ok(())
        };
        let first = FailAt::new(FaultPoint::Publish, 1);
        let second = FailAt::new(FaultPoint::Publish, 2);
        let on = OnPass::new(FaultPoint::Publish, 2, &act);
        let faults = Faults(&[&first, &second, &on]);
        assert_eq!(
            faults.pass(FaultPoint::Publish).unwrap_err().to_string(),
            INJECTED
        );
        // The pass the first failed was counted by the others too.
        assert!(faults.pass(FaultPoint::Publish).is_err());
        assert_eq!(acted.get(), 1);
        assert!(faults.pass(FaultPoint::Publish).is_ok());
    }
}
