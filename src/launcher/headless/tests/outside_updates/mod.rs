//! What the test programs that update an installation from a launcher file outside it share: such
//! a file, whose update leaves the game in place and so is done in moments, and the few bytes the
//! installation's own launcher copy holds, which none of their runs starts.

use std::fs;

use gdl_fixtures::{RuntimeFiles, append_sections};
use gdl_install::RUNTIME_SECTION;
use gdl_release::Platform;

use crate::common::PROGRAM;

/// What the installation's launcher copy holds where no test runs it: a few bytes, so that an
/// update hashes and retires no program.
pub const INSTALLED_COPY: &[u8] = b"the launcher copy installed before";

/// A launcher file for `platform` of the program's own carrying the standard runtime of `version`
/// as it is, its executable the runtime's own few bytes: an update from it to an installation of
/// another standard runtime moves the files that name the release and the launcher copy alone, and
/// leaves the game, which it neither stages nor probes.
pub fn launcher_keeping_the_game(platform: Platform, version: &str) -> Vec<u8> {
    let mut launcher = fs::read(PROGRAM).unwrap();
    let runtime = RuntimeFiles::standard(platform.as_str(), version);
    append_sections(&mut launcher, &[(RUNTIME_SECTION, &runtime.zip())], 0);
    launcher
}
