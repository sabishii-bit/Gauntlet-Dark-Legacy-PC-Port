//! The launcher's modes without a window and, on Linux, the entry program that starts the window.
//! Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]

use std::ffi::OsString;
use std::io;
use std::process::ExitCode;

/// The optional features this build carries, which `--check-payload` reports.
const FEATURES: &[&str] = &[];

fn main() -> ExitCode {
    // Opened before anything else, so a later rename of this file cannot change what is read.
    let mut own = gdl_session::headless::open_own_file();
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    let build = gdl_session::headless::Build {
        version: env!("GDL_VERSION"),
        features: FEATURES,
    };
    let (mut out, mut err) = (io::stdout().lock(), io::stderr().lock());
    let code = gdl_session::headless::run(&args, &build, &mut own, &mut out, &mut err)
        .unwrap_or_else(|| gdl_session::headless::usage(&mut err));
    ExitCode::from(code)
}
