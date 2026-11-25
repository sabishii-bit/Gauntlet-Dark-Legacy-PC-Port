//! What the headless crate's test programs share: the headless program cargo built, and how they
//! run it.

use std::ffi::OsStr;
use std::io;
use std::path::Path;
use std::process::{Command, Output, Stdio};
use std::thread;
use std::time::Instant;

use gdl_files::{RETRY_INTERVAL, RETRY_WINDOW};
use serde_json::Value;

/// The headless program, as cargo built it for these tests.
pub const PROGRAM: &str = env!("CARGO_BIN_EXE_gdl-launcher-headless");

/// The version its build gave it.
pub const VERSION: &str = env!("GDL_VERSION");

/// The creation flag that runs a console program without a console window of its own, so that a
/// test never opens one.
#[cfg(windows)]
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;

/// Runs `command`, which starts `program`, with nothing to read and without a console window, to
/// its end, and gives back what it wrote and how it ended. A program another test starts holds
/// every file this process has open for a moment, so on Linux a program just written may be
/// refused as busy, and is run again then, every [`RETRY_INTERVAL`] for up to [`RETRY_WINDOW`],
/// as the launcher's probe runs one again.
pub fn run_to_end(mut command: Command, program: &Path) -> Output {
    command.stdin(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(CREATE_NO_WINDOW);
    }
    let begun = Instant::now();
    loop {
        match command.output() {
            Err(error)
                if error.kind() == io::ErrorKind::ExecutableFileBusy
                    && begun.elapsed() < RETRY_WINDOW =>
            {
                thread::sleep(RETRY_INTERVAL);
            }
            result => {
                return result.unwrap_or_else(|error| panic!("{}: {error}", program.display()));
            }
        }
    }
}

/// Inspects the folder at `folder` through the headless program, as JSON, which it reports
/// whatever the folder holds: it ends with status 0 and writes nothing to its error stream.
#[track_caller]
pub fn inspect_json(folder: &Path) -> Value {
    let mut command = Command::new(PROGRAM);
    command.args([
        OsStr::new("--inspect"),
        folder.as_os_str(),
        OsStr::new("--json"),
    ]);
    let output = run_to_end(command, Path::new(PROGRAM));
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert!(output.stderr.is_empty(), "{output:?}");
    serde_json::from_slice(&output.stdout).unwrap()
}
