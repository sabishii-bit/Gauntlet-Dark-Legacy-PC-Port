//! The headless program as the release scripts and developers run it: the version it reports, its
//! check of the runtime its file carries, and its report of a folder.

use std::env::consts::EXE_SUFFIX;
use std::ffi::OsStr;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use gdl_fixtures::{InstallFixture, RuntimeFiles, append_sections_at, make_alpha_markers};
use gdl_install::{RUNTIME_SECTION, probe_game_version};
use serde_json::{Value, json};

/// The headless program, as cargo built it for these tests.
const PROGRAM: &str = env!("CARGO_BIN_EXE_gdl-launcher-headless");

/// The version its build gave it.
const VERSION: &str = env!("GDL_VERSION");

/// A version that no launcher is: older than the first.
const OTHER_VERSION: &str = "0.0.1";

/// The line that answers arguments the program does not take.
const USAGE: &str = "usage: gdl-launcher-headless --check-payload [--payload <zip>] | \
                     --inspect <dir> [--json] | --version\n";

/// How long a program just written is tried again for while Linux finds it still open to write,
/// as long as the launcher retries a file operation that something else holds up.
const BUSY_WINDOW: Duration = Duration::from_secs(8);

/// How long to wait between those tries.
const BUSY_PAUSE: Duration = Duration::from_millis(200);

/// The creation flag that runs a console program without a console window of its own, so that a
/// test never opens one.
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

/// The platform this program was built for, as a release names it; `None`, the reason printed, on
/// a system no release is built for.
fn this_platform() -> Option<&'static str> {
    let platform = if cfg!(all(windows, target_arch = "x86_64")) {
        Some("windows-x64")
    } else if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        Some("linux-x64")
    } else {
        None
    };
    if platform.is_none() {
        eprintln!("skipped: no release is built for this system");
    }
    platform
}

/// Runs `program` with `args` and nothing to read, without a console window, to its end, and gives
/// back what it wrote and how it ended. A program another test starts holds every file this
/// process has open for a moment, so on Linux a program just written may be refused as busy, and
/// is run again then.
fn run<A: AsRef<OsStr>>(program: &Path, args: &[A]) -> Output {
    let mut command = Command::new(program);
    command.args(args).stdin(Stdio::null());
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
                    && begun.elapsed() < BUSY_WINDOW =>
            {
                thread::sleep(BUSY_PAUSE);
            }
            result => {
                return result.unwrap_or_else(|error| panic!("{}: {error}", program.display()));
            }
        }
    }
}

/// Runs the headless program cargo built with `args`.
fn run_program<A: AsRef<OsStr>>(args: &[A]) -> Output {
    run(Path::new(PROGRAM), args)
}

/// What a program wrote to one of its streams, as text.
fn text(bytes: &[u8]) -> &str {
    std::str::from_utf8(bytes).unwrap()
}

/// A copy in `dir` of the headless program with `runtime` as its runtime section, appended as a
/// release appends one to the launcher file, after `change` is made to what is appended.
fn launcher_file(dir: &Path, runtime: &[u8], change: impl FnOnce(&mut [u8])) -> PathBuf {
    let copy = dir.join(format!("gdl-launcher-headless{EXE_SUFFIX}"));
    fs::copy(PROGRAM, &copy).unwrap();
    let program_end = fs::metadata(&copy).unwrap().len();
    let mut appended = Vec::new();
    append_sections_at(&mut appended, program_end, &[(RUNTIME_SECTION, runtime)], 0);
    change(&mut appended);
    let mut file = OpenOptions::new().append(true).open(&copy).unwrap();
    file.write_all(&appended).unwrap();
    copy
}

/// Inspects the folder at `folder` through the headless program, as JSON, which it reports
/// whatever the folder holds.
#[track_caller]
fn inspect_json(folder: &Path) -> Value {
    let args = [
        OsStr::new("--inspect"),
        folder.as_os_str(),
        OsStr::new("--json"),
    ];
    let output = run_program(&args);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stderr), "");
    serde_json::from_slice(&output.stdout).unwrap()
}

#[test]
fn version_prints_the_version_its_build_gave_it() {
    let output = run_program(&["--version"]);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stdout), format!("{VERSION}\n"));
    assert_eq!(text(&output.stderr), "");
    if option_env!("TAURI_CONFIG").is_none() {
        let file = include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../VERSION"));
        assert_eq!(VERSION, file.trim_ascii());
    }
}

#[test]
fn the_version_probe_reads_a_real_program() {
    // The probe runs the game as the launcher will before it changes an installation: with
    // `--version`, which the headless program answers the way the game does.
    let probe = probe_game_version(Path::new(PROGRAM)).unwrap();
    assert!(probe.succeeded(), "{probe:?}");
    assert_eq!(probe.first_line.as_deref(), Some(VERSION), "{probe:?}");
}

#[test]
fn a_sectioned_copy_verifies() {
    let Some(platform) = this_platform() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let runtime = RuntimeFiles::standard(platform, VERSION).zip();
    let launcher = launcher_file(dir.path(), &runtime, |_| {});
    let output = run(&launcher, &["--check-payload"]);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(
        text(&output.stdout),
        format!("Verified {VERSION} ({platform})\nFeatures: none\n")
    );
    assert_eq!(text(&output.stderr), "");
}

#[test]
fn a_tampered_section_is_refused() {
    let Some(platform) = this_platform() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let runtime = RuntimeFiles::standard(platform, VERSION).zip();
    // A byte of the runtime changed after the table recorded its digest.
    let launcher = launcher_file(dir.path(), &runtime, |appended| appended[0] ^= 0x01);
    let output = run(&launcher, &["--check-payload"]);
    assert_eq!(output.status.code(), Some(1), "{output:?}");
    let err = text(&output.stderr);
    assert!(
        err.starts_with("error: payload.damaged (sectionDigest)"),
        "{err}"
    );
    assert_eq!(text(&output.stdout), "");
}

#[test]
fn a_runtime_of_another_version_is_refused() {
    let Some(platform) = this_platform() else {
        return;
    };
    assert_ne!(OTHER_VERSION, VERSION);
    let dir = tempfile::tempdir().unwrap();
    let runtime = RuntimeFiles::standard(platform, OTHER_VERSION).zip();
    let launcher = launcher_file(dir.path(), &runtime, |_| {});
    let output = run(&launcher, &["--check-payload"]);
    assert_eq!(output.status.code(), Some(1), "{output:?}");
    assert_eq!(
        text(&output.stderr),
        format!(
            "error: payload.damaged (otherVersion): The runtime's version {OTHER_VERSION:?} is \
             not the launcher's version {VERSION:?}\n"
        )
    );
    assert_eq!(text(&output.stdout), "");
}

#[test]
fn an_unsectioned_build_is_refused() {
    // Elsewhere than on the systems a release is built for, the program may be of a kind the
    // sections' reader does not know.
    if this_platform().is_none() {
        return;
    }
    // The program as cargo built it carries no runtime.
    let output = run_program(&["--check-payload"]);
    assert_eq!(output.status.code(), Some(1), "{output:?}");
    assert_eq!(
        text(&output.stderr),
        "error: payload.damaged (noSections): the file carries no sections\n"
    );
    assert_eq!(text(&output.stdout), "");
}

#[test]
fn usage_errors_exit_2() {
    let cases: &[&[&str]] = &[
        // No headless mode, which this program answers with its usage.
        &[],
        &["--allow-root"],
        // Modes in forms they do not take.
        &["--version", "--json"],
        &["--version", "--check-payload"],
        &["--check-payload", "--payload"],
        &["--inspect"],
    ];
    for &args in cases {
        let output = run_program(args);
        assert_eq!(output.status.code(), Some(2), "{args:?}: {output:?}");
        assert_eq!(text(&output.stderr), USAGE, "{args:?}");
        assert_eq!(text(&output.stdout), "", "{args:?}");
    }
}

#[test]
fn inspect_reports_json() {
    let Some(platform) = this_platform() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();

    // An installation of this program's release.
    let runtime = RuntimeFiles::standard(platform, VERSION);
    let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
    let report = inspect_json(&fixture.root);
    assert_eq!(report["schema"], 1);
    assert_eq!(report["path"], fixture.root.display().to_string());
    assert_eq!(report["state"], "installed");
    assert_eq!(report["code"], Value::Null);
    let installation = &report["installation"];
    assert_eq!(installation["version"], VERSION);
    assert_eq!(installation["platform"], platform);
    assert_eq!(installation["launcher"], VERSION);
    assert_eq!(installation["relation"], "same");
    assert_eq!(installation["access"], "writable");

    // An empty folder.
    let empty = dir.path().join("empty");
    fs::create_dir(&empty).unwrap();
    let report = inspect_json(&empty);
    assert_eq!(report["state"], "empty");
    assert_eq!(report["installation"], Value::Null);

    // A folder the alpha installer installed into, with the names it is known by.
    let alpha = dir.path().join("alpha");
    fs::create_dir(&alpha).unwrap();
    make_alpha_markers(&alpha, &["installation.json", ".gdl-update.lock"]).unwrap();
    let report = inspect_json(&alpha);
    assert_eq!(report["state"], "alphaInstall");
    assert_eq!(report["code"], "folder.alphaInstall");
    assert_eq!(
        report["alphaMarkers"],
        json!(["installation.json", ".gdl-update.lock"])
    );
}
