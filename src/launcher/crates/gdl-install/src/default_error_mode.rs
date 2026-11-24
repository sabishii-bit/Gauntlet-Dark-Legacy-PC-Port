//! A test of this test program run alone in a child started in the system's default error mode,
//! where critical errors show a dialog, as a launcher started from Explorer is: the shells and
//! runners the tests run under fail critical errors already, and a program inherits its error mode
//! from what starts it, so only such a child shows that a job sets the mode itself.

use std::os::windows::process::CommandExt;
use std::process::{Command, Stdio};

use windows_sys::Win32::System::Diagnostics::Debug::SEM_FAILCRITICALERRORS;
use windows_sys::Win32::System::Threading::{CREATE_DEFAULT_ERROR_MODE, CREATE_NO_WINDOW};

use crate::platform::{error_mode, refuse_fault_dialogs};

/// The exit code of a child whose every check of the error mode held.
pub(crate) const ERROR_MODE_CHECKED: i32 = 75;

/// The arguments that make this test program run the test `name` alone.
pub(crate) fn only_test(name: &str) -> [&str; 4] {
    ["--exact", name, "--test-threads", "1"]
}

/// Runs the test `name` alone in a child of this test program started in the system's default
/// error mode and without a window, with the variable `child` set so that the test plays its part,
/// and checks that the child ended with [`ERROR_MODE_CHECKED`].
pub(crate) fn run_in_default_error_mode(name: &str, child: &str) {
    let output = Command::new(std::env::current_exe().unwrap())
        .args(only_test(name))
        .env(child, "1")
        .creation_flags(CREATE_DEFAULT_ERROR_MODE | CREATE_NO_WINDOW)
        .stdin(Stdio::null())
        .output()
        .unwrap();
    assert_eq!(
        output.status.code(),
        Some(ERROR_MODE_CHECKED),
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

/// In the child's part of such a test, the error mode the child started in, which is checked to
/// show critical errors, with fault dialogs refused from then on so that a fault still shows none;
/// `None` when the variable `child` is unset, as in a run of the tests, where that part does
/// nothing.
pub(crate) fn started_in_default_error_mode(child: &str) -> Option<u32> {
    std::env::var_os(child)?;
    refuse_fault_dialogs();
    let started = error_mode();
    assert_eq!(started & SEM_FAILCRITICALERRORS, 0, "{started:#x}");
    Some(started)
}
