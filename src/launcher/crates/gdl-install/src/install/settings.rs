//! What an install asks of the world beyond its folder, gathered so that its tests can stand in for
//! each: what judging the folder again asks of the system, the game's version probe, the setting of
//! the disc's icon into the Windows executable, the shell's notice of a new icon, and the named
//! points where a test may stop the job.

use std::path::Path;

use gdl_files::Cancel;

use crate::destination::FolderChecks;
use crate::faults::{FaultPoints, NoFaults};
use crate::icon::refresh_shell_icon;
#[cfg(windows)]
use crate::icon::{IconError, Personalised, personalise};
use crate::probe::{GameStartError, VersionProbe, probe_game_version_with};

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
    /// What judging the folder again asks of the system: its probe and its volume's measure among
    /// it.
    pub(crate) folders: &'a FolderChecks<'a>,
    /// How the staged game is probed.
    pub(crate) probe: &'a ProbeFn,
    /// How the disc's icon is set into the staged executable.
    #[cfg(windows)]
    pub(crate) personalise: &'a PersonaliseFn,
    /// How the shell is told that the installed executable carries a new icon.
    pub(crate) notify_shell: &'a dyn Fn(&Path),
}

impl InstallSettings<'static> {
    /// What the launcher asks: no faults, the folder judged as a plan judges it, the game's own
    /// probe, the icon set into the executable itself and the shell told.
    pub(crate) const SHIPPED: InstallSettings<'static> = InstallSettings {
        faults: &NoFaults,
        folders: &FolderChecks::SHIPPED,
        probe: &probe_game_version_with,
        #[cfg(windows)]
        personalise: &personalise,
        notify_shell: &refresh_shell_icon,
    };
}
