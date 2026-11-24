//! What an install asks of the world beyond its folder, gathered so that its tests can stand in for
//! each: the game's version probe, the setting of the disc's icon into the Windows executable, the
//! shell's notice of a new icon, and the named points where a test may stop the job.

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
    use super::{FaultPoint, FaultPoints, NoFaults};

    #[test]
    fn a_job_nothing_stops_passes_every_point() {
        for point in [
            FaultPoint::Prepare,
            FaultPoint::Stage,
            FaultPoint::Publish,
            FaultPoint::Cleanup,
        ] {
            assert!(NoFaults.pass(point).is_ok(), "{point:?}");
        }
    }
}
