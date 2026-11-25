//! The named points a job passes each time it has made a step that must survive its end, gathered
//! in one place so that every job passes the same ones, and the stand-ins its tests put there to
//! fail, stop, panic or kill the job at a pass and see what it leaves.

#[cfg(test)]
use std::cell::Cell;
use std::io;
#[cfg(test)]
use std::sync::Mutex;

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

#[cfg(test)]
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

/// The exit code of a test program that [`KillAt`] ended: none that a run that passed, a test that
/// failed or a name that matched no test gives.
#[cfg(test)]
pub(crate) const KILLED_EXIT_CODE: i32 = 71;

/// Held while a child of the test program runs a job that [`KillAt`] may end, so that the kill
/// tests' children, an install's and an update's alike, run one at a time.
#[cfg(test)]
pub(crate) static ONE_KILLED_CHILD: Mutex<()> = Mutex::new(());

/// Where a job passes its [`FaultPoint`]s: an error from a pass fails the job there, as the
/// step after it would have failed.
pub(crate) trait FaultPoints {
    /// The job passes `point`.
    fn pass(&self, point: FaultPoint) -> io::Result<()>;
}

/// The points of a job nothing stops: every pass goes on.
pub(crate) struct NoFaults;

impl FaultPoints for NoFaults {
    fn pass(&self, _: FaultPoint) -> io::Result<()> {
        Ok(())
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

    use super::{
        FailAt, FaultPoint, FaultPoints, Faults, INJECTED, KillAt, NoFaults, OnPass, PanicAt,
    };

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
