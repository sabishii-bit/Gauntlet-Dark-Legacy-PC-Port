//! What an install asks of the world beyond its folder, gathered so that its tests can stand in for
//! each: the game's version probe, the setting of the disc's icon into the Windows executable, the
//! shell's notice of a new icon, and the named points where a test may stop the job.

#[cfg(test)]
use std::cell::Cell;
use std::io;
use std::path::Path;

use gdl_files::Cancel;

use crate::icon::refresh_shell_icon;
#[cfg(windows)]
use crate::icon::{IconError, Personalised, personalise};
use crate::probe::{GameStartError, VersionProbe, probe_game_version_with};

/// A point an install passes, each time it has made a step that must survive the job's end: where
/// a test fails, stops or kills the job to see what it leaves.
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
}

/// Where an install passes its [`FaultPoint`]s: an error from a pass fails the job there, as the
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

/// Runs the staged game with `--version` until the cancel asks it to stop, as
/// [`probe_game_version_with`] does.
pub(crate) type ProbeFn = dyn Fn(&Path, &Cancel) -> Result<VersionProbe, GameStartError>;

/// Sets the disc's icon, a TPL's bytes, into the staged Windows executable, as [`personalise`]
/// does.
#[cfg(windows)]
pub(crate) type PersonaliseFn = dyn Fn(&Path, &[u8]) -> Result<Personalised, IconError>;

/// What an install asks of the world beyond its folder.
pub(crate) struct InstallSettings<'a> {
    /// Where the job passes its fault points.
    pub(crate) faults: &'a dyn FaultPoints,
    /// How the staged game is probed.
    pub(crate) probe: &'a ProbeFn,
    /// How the disc's icon is set into the staged executable.
    #[cfg(windows)]
    pub(crate) personalise: &'a PersonaliseFn,
    /// How the shell is told that the installed executable carries a new icon.
    pub(crate) notify_shell: &'a dyn Fn(&Path),
}

impl InstallSettings<'static> {
    /// What the launcher asks: no faults, the game's own probe, the icon set into the executable
    /// itself and the shell told.
    pub(crate) const SHIPPED: InstallSettings<'static> = InstallSettings {
        faults: &NoFaults,
        probe: &probe_game_version_with,
        #[cfg(windows)]
        personalise: &personalise,
        notify_shell: &refresh_shell_icon,
    };
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::io;
    use std::panic::{AssertUnwindSafe, catch_unwind};

    use super::{FailAt, FaultPoint, FaultPoints, Faults, INJECTED, NoFaults, OnPass, PanicAt};

    /// Every point an install passes.
    const POINTS: [FaultPoint; 5] = [
        FaultPoint::Prepare,
        FaultPoint::Stage,
        FaultPoint::Publish,
        FaultPoint::Cleanup,
        FaultPoint::Rollback,
    ];

    #[test]
    fn a_job_nothing_stops_passes_every_point() {
        for point in POINTS {
            assert!(NoFaults.pass(point).is_ok(), "{point:?}");
        }
    }

    #[test]
    fn a_failure_comes_at_its_pass_of_its_point_alone() {
        let fail = FailAt::new(FaultPoint::Publish, 2);
        // The other points' passes are not counted.
        for point in POINTS {
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
