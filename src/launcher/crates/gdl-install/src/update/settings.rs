//! What an update asks of the world beyond its folder, gathered so that its tests can stand in for
//! each: the named points where a test may stop it, the volume's measure, the game's version probe
//! and the closed-game probe, the setting of the disc's icon into the Windows executable, the
//! shell's notice of a new icon, the receipt's write, and how often a hold looks again.

use std::path::Path;
use std::time::Duration;

use gdl_files::{Cancel, FilesError};

use crate::destination::FolderChecks;
use crate::faults::{FaultPoints, shipped_faults};
use crate::folder::InstallFolder;
use crate::hold::HOLD_POLL;
use crate::icon::refresh_shell_icon;
#[cfg(windows)]
use crate::icon::{IconError, Personalised, personalise};
use crate::probe::{
    GameStartError, GameUseError, VersionProbe, probe_game_version_with, require_game_closed,
};
use crate::receipt::Receipt;

/// Runs a game with `--version` until the cancel asks it to stop, as [`probe_game_version_with`]
/// does.
pub(crate) type ProbeFn<'a> = dyn Fn(&Path, &Cancel) -> Result<VersionProbe, GameStartError> + 'a;

/// Sets the disc's icon, a TPL's bytes, into the staged Windows executable, as [`personalise`]
/// does.
#[cfg(windows)]
pub(crate) type PersonaliseFn<'a> = dyn Fn(&Path, &[u8]) -> Result<Personalised, IconError> + 'a;

/// Writes a receipt as an installation's own, as [`Receipt::write`] does.
pub(crate) type WriteReceiptFn<'a> =
    dyn Fn(&Receipt, &InstallFolder) -> Result<(), FilesError> + 'a;

/// What an update asks of the world beyond its folder.
pub(crate) struct UpdateSettings<'a> {
    /// Where the job passes its fault points.
    pub(crate) faults: &'a dyn FaultPoints,
    /// What the system is asked of the folder: its volume's measure among it.
    pub(crate) folders: &'a FolderChecks<'a>,
    /// How the staged game, and the installed one, are probed.
    pub(crate) probe: &'a ProbeFn<'a>,
    /// How the game is found closed, as [`require_game_closed`] finds it.
    pub(crate) game_closed: &'a dyn Fn(&Path) -> Result<(), GameUseError>,
    /// How the disc's icon is set into the staged executable.
    #[cfg(windows)]
    pub(crate) personalise: &'a PersonaliseFn<'a>,
    /// How the shell is told that the installed executable carries a new icon.
    pub(crate) notify_shell: &'a dyn Fn(&Path),
    /// How the new receipt is written.
    pub(crate) write_receipt: &'a WriteReceiptFn<'a>,
    /// How often a hold looks again whether what holds the job has ended.
    pub(crate) hold_poll: Duration,
}

impl UpdateSettings<'static> {
    /// What the launcher asks: no faults but, in a build with the test hooks, the pass the
    /// environment names, the system's own volume measure, the game's own probes, the icon set
    /// into the executable itself, the shell told, the receipt written as it writes one, and holds
    /// that look again every [`HOLD_POLL`].
    pub(crate) fn shipped() -> UpdateSettings<'static> {
        UpdateSettings {
            faults: shipped_faults(),
            folders: &FolderChecks::SHIPPED,
            probe: &probe_game_version_with,
            game_closed: &require_game_closed,
            #[cfg(windows)]
            personalise: &personalise,
            notify_shell: &refresh_shell_icon,
            write_receipt: &Receipt::write,
            hold_poll: HOLD_POLL,
        }
    }
}
