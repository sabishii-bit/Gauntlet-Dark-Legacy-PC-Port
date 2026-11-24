//! The headless program as the release scripts and developers run it: the version it reports, its
//! check of the runtime its file carries, its reports of what a folder is to the launcher and to
//! an install, and its extraction of a Legends expansion's tree into a new folder.

use std::collections::BTreeMap;
use std::env::consts::EXE_SUFFIX;
use std::ffi::{OsStr, OsString};
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use gdl_fixtures::{
    DiscSet, Edc, EntryState, InstallFixture, Listed, RuntimeFiles, append_sections_at, backdate,
    listed, make_alpha_markers, snapshot, snapshot_outside, tree_listing, unhex,
};
use gdl_install::{RUNTIME_SECTION, probe_game_version};
use serde_json::{Value, json};
use tempfile::TempDir;

/// The headless program, as cargo built it for these tests.
const PROGRAM: &str = env!("CARGO_BIN_EXE_gdl-launcher-headless");

/// The version its build gave it.
const VERSION: &str = env!("GDL_VERSION");

/// A version that no launcher is: older than the first.
const OTHER_VERSION: &str = "0.0.1";

/// The line that answers arguments the program does not take.
const USAGE: &str = "usage: gdl-launcher-headless --check-payload [--payload <zip>] | \
                     --inspect <dir> [--json] | --check-folder <dir> [--json] | \
                     --extract <n64|dreamcast> <dump> --output <dir> [--strict script] | \
                     --version\n";

/// The N64 cases, which hold the image the tests extract and the tree the N64 extractor writes
/// for it.
const N64_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../tests/scripts/data/legends/n64-vectors.json"
));

/// The Dreamcast cases, which hold the tree the Dreamcast extractor writes for the full set.
const DREAMCAST_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../tests/scripts/data/legends/dreamcast-vectors.json"
));

/// The manifest the N64 extractor wrote for the default image, which places its files in it.
const N64_DEFAULT_MANIFEST: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../tests/scripts/data/legends/n64-default-manifest.json"
));

/// The disc manifest the Dreamcast extractor wrote for the full set.
const FULL_DISC_MANIFEST: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../tests/scripts/data/legends/dreamcast-full-disc-manifest.json"
));

/// What every extraction's staging is named after, whatever its expansion.
const STAGINGS: &str = ".gdl-extract-";

/// What no raw DEFLATE stream begins with: a last block of the type the format reserves.
const NO_STREAM: u8 = 0xFF;

/// How long something that another process holds up is tried again for, as long as the launcher
/// retries a file operation that something else holds up: running a program just written, which
/// Linux may find still open to write, and removing a staging a scanner holds on Windows.
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
/// back what it wrote and how it ended.
fn run<A: AsRef<OsStr>>(program: &Path, args: &[A]) -> Output {
    let mut command = Command::new(program);
    command.args(args);
    run_to_end(command, program)
}

/// Runs `command`, which starts `program`, with nothing to read and without a console window, to
/// its end, and gives back what it wrote and how it ended. A program another test starts holds
/// every file this process has open for a moment, so on Linux a program just written may be
/// refused as busy, and is run again then.
fn run_to_end(mut command: Command, program: &Path) -> Output {
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

/// Removes the folder at `path` and everything in it, trying again every [`BUSY_PAUSE`] for up to
/// [`BUSY_WINDOW`] while it fails, as something that holds part of it lets go. A folder already
/// gone counts as removed.
fn remove_once_let_go(path: &Path) -> io::Result<()> {
    let begun = Instant::now();
    loop {
        match fs::remove_dir_all(path) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
            Err(_) if begun.elapsed() < BUSY_WINDOW => thread::sleep(BUSY_PAUSE),
            result => return result,
        }
    }
}

/// Runs the headless program cargo built with `args`.
fn run_program<A: AsRef<OsStr>>(args: &[A]) -> Output {
    run(Path::new(PROGRAM), args)
}

/// Runs the headless program cargo built with `args`, from the folder `folder`, so that a relative
/// path among them is taken from there.
fn run_program_in<A: AsRef<OsStr>>(folder: &Path, args: &[A]) -> Output {
    let mut command = Command::new(PROGRAM);
    command.args(args).current_dir(folder);
    run_to_end(command, Path::new(PROGRAM))
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
        &["--check-payload", "--payload", "--json"],
        &["--inspect"],
        &["--inspect", ""],
        &["--check-folder"],
        &["--check-folder", ""],
        &["--check-folder", "folder", "--json", "--json"],
        &["--check-folder", "folder", "--payload", "a.zip"],
        &["--check-folder", "folder", "--inspect", "folder"],
        &["--extract"],
        &["--extract", "n64", "rom.z64"],
        &["--extract", "gamecube", "game.iso", "--output", "out"],
        &[
            "--extract",
            "n64",
            "rom.z64",
            "--output",
            "out",
            "--strict",
            "install",
        ],
        &["--extract", "n64", "rom.z64", "--output", "out."],
        &[
            "--extract",
            "dreamcast",
            "disc.cue",
            "--output",
            ".gdl-launcher",
        ],
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

/// Judges the folder at `folder` through the headless program as one to install into, as JSON
/// when `json` says so, and gives back the report, which it writes whatever the folder holds.
#[track_caller]
fn check_folder(folder: &Path, json: bool) -> String {
    let mut args = vec![OsStr::new("--check-folder"), folder.as_os_str()];
    if json {
        args.push(OsStr::new("--json"));
    }
    let output = run_program(&args);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stderr), "");
    text(&output.stdout).to_owned()
}

/// The `name: value` lines of a text report, split at their first `: `.
fn report_lines(report: &str) -> Vec<(String, String)> {
    assert!(report.ends_with('\n'), "{report:?}");
    report
        .lines()
        .map(|line| {
            let (name, value) = line.split_once(": ").expect("a name and a value");
            (name.to_owned(), value.to_owned())
        })
        .collect()
}

/// The codes of the warnings that a folder of the test's own may call for on Windows, where the
/// machine's temporary folder may lie in a known folder or a cloud provider's, or have a name that
/// is not ASCII. Elsewhere no folder is warned of.
const WARNING_CODES: [&str; 3] = [
    "folder.cloudSynced",
    "folder.protected",
    "folder.nonAsciiOldWindows",
];

/// Whether `value`, a text report's line, is a warning that a folder of the test's own may call
/// for, as [`WARNING_CODES`] lists them.
fn is_the_machines_warning(value: &str) -> bool {
    cfg!(windows)
        && value
            .split_once(' ')
            .is_some_and(|(code, _)| WARNING_CODES.contains(&code))
}

/// The value of the field `name` of a text report, which must give it once.
#[track_caller]
fn field<'a>(lines: &'a [(String, String)], name: &str) -> &'a str {
    let mut values = lines.iter().filter(|(field, _)| field == name);
    let (_, value) = values.next().expect("the field");
    assert!(values.next().is_none(), "{name} twice");
    value
}

#[test]
fn check_folder_reports_a_fresh_folder() {
    let dir = tempfile::tempdir().unwrap();
    let top = dir.path();
    let fresh = top.join("game");

    // Judged whole: probed in the folder above it, which takes entries, and measured there.
    let report = check_folder(&fresh, false);
    let lines = report_lines(&report);
    assert_eq!(field(&lines, "path"), fresh.display().to_string());
    assert_eq!(field(&lines, "exists"), "no");
    assert_eq!(field(&lines, "nearestExisting"), top.display().to_string());
    assert!(
        field(&lines, "availableBytes").parse::<u64>().is_ok(),
        "{report}"
    );
    assert!(
        field(&lines, "rootUtf16").parse::<usize>().is_ok(),
        "{report}"
    );
    assert_eq!(field(&lines, "writable"), "yes");
    // Windows' file systems ignore letter case, and the Linux tests' do not.
    let ignores_case = if cfg!(windows) { "yes" } else { "no" };
    assert_eq!(field(&lines, "ignoresCase"), ignores_case);
    assert_eq!(field(&lines, "noexec"), "no");
    for (name, value) in &lines {
        assert!(name != "blocker", "{report}");
        assert!(
            name != "warning" || is_the_machines_warning(value),
            "{report}"
        );
    }

    let parsed: Value = serde_json::from_str(&check_folder(&fresh, true)).unwrap();
    assert_eq!(parsed["schema"], 1);
    assert_eq!(parsed["path"], fresh.display().to_string());
    assert_eq!(parsed["exists"], false);
    assert_eq!(parsed["nearestExisting"], top.display().to_string());
    assert!(parsed["availableBytes"].is_u64(), "{parsed}");
    assert!(parsed["rootUtf16"].is_u64(), "{parsed}");
    assert_eq!(parsed["writable"], true);
    assert_eq!(parsed["ignoresCase"], cfg!(windows));
    assert_eq!(parsed["noexec"], false);
    assert_eq!(parsed["blockers"], json!([]));
    let warnings = parsed["warnings"].as_array().unwrap();
    if cfg!(windows) {
        for warning in warnings {
            let code = warning["code"].as_str().unwrap_or_default();
            assert!(WARNING_CODES.contains(&code), "{warning}");
        }
    } else {
        assert!(warnings.is_empty(), "{parsed}");
    }
    // What Windows shows of each entry on the path that is there, the nearest first.
    let shown: Vec<Value> = parsed["components"]
        .as_array()
        .unwrap()
        .iter()
        .map(|component| component["path"].clone())
        .collect();
    let ancestors: Vec<Value> = if cfg!(windows) {
        top.ancestors()
            .map(|entry| Value::from(entry.display().to_string()))
            .collect()
    } else {
        Vec::new()
    };
    assert_eq!(shown, ancestors);

    // The probe's entries are gone, and nothing was made.
    assert_eq!(fs::read_dir(top).unwrap().count(), 0, "the probe was left");
}

#[test]
fn check_folder_reports_an_alpha_folder_and_a_file() {
    let dir = tempfile::tempdir().unwrap();
    let top = dir.path();
    let alpha = top.join("alpha");
    fs::create_dir(&alpha).unwrap();
    make_alpha_markers(&alpha, &["installation.json", ".gdl-update.lock"]).unwrap();
    let file = top.join("file.txt");
    fs::write(&file, b"not a folder").unwrap();
    backdate(top).unwrap();
    let before = snapshot(top).unwrap();

    // Each is judged, then the judging ends before it probes the folder or asks its volume.
    for (folder, code, rule, detail) in [
        (
            &alpha,
            "folder.alphaInstall",
            "alphaInstall",
            "The folder holds an alpha installation: installation.json, .gdl-update.lock",
        ),
        (
            &file,
            "folder.notDirectory",
            "notDirectory",
            "Install destination is not a directory",
        ),
    ] {
        let named = folder.display();
        let report = check_folder(folder, false);
        let lines = report_lines(&report);
        assert_eq!(field(&lines, "path"), named.to_string());
        assert_eq!(field(&lines, "exists"), "yes", "{named}");
        assert_eq!(field(&lines, "nearestExisting"), named.to_string());
        for unasked in [
            "availableBytes",
            "rootUtf16",
            "writable",
            "ignoresCase",
            "noexec",
        ] {
            assert_eq!(field(&lines, unasked), "unknown", "{named}: {unasked}");
        }
        assert_eq!(
            field(&lines, "blocker"),
            format!("{code} ({rule}): {detail}"),
            "{named}"
        );

        let parsed: Value = serde_json::from_str(&check_folder(folder, true)).unwrap();
        assert_eq!(parsed["exists"], true, "{named}");
        assert_eq!(parsed["writable"], Value::Null, "{named}");
        assert_eq!(parsed["availableBytes"], Value::Null, "{named}");
        assert_eq!(
            parsed["blockers"],
            json!([{"code": code, "rule": rule, "detail": detail}]),
            "{named}"
        );
        assert_eq!(parsed["components"], json!([]), "{named}");
    }
    // Neither was written into, nor the folder that holds them.
    assert!(snapshot(top).unwrap() == before, "a folder changed");
}

/// The cases of `text`, a file of the shared vectors, read.
fn vectors(text: &str) -> Value {
    serde_json::from_str(text).expect("the vectors are JSON")
}

/// A string of the vectors.
fn string(value: &Value) -> &str {
    value.as_str().expect("a string")
}

/// The image the N64 vectors freeze as their default, and the name their extraction of it gives
/// its dump, which the tree's manifest records.
fn default_rom() -> (String, Vec<u8>) {
    let n64 = vectors(N64_VECTORS);
    let cases = n64["extractions"].as_array().expect("the extractions");
    let case = cases
        .iter()
        .find(|case| case["image"] == "default")
        .expect("an extraction of the default image");
    (
        string(&case["name"]).to_owned(),
        unhex(&n64["images"]["default"]),
    )
}

/// The arguments that extract the expansion `expansion` names from the dump at `dump` into
/// `output`, held as the extraction scripts hold a dump when `script` says so.
fn extract_args(expansion: &str, dump: &Path, output: &Path, script: bool) -> Vec<OsString> {
    let mut args: Vec<OsString> = vec![
        "--extract".into(),
        expansion.into(),
        dump.into(),
        "--output".into(),
        output.into(),
    ];
    if script {
        args.extend(["--strict".into(), "script".into()]);
    }
    args
}

/// The folders an extraction is tried in: one the outputs are made in, which holds a file of its
/// own before any extraction, and one the dumps lie in.
struct Folders {
    /// The folder they lie in, removed with them.
    _dir: TempDir,
    /// The folder the outputs are made in.
    parent: PathBuf,
    /// The folder the dumps lie in.
    dumps: PathBuf,
}

impl Folders {
    fn new() -> Folders {
        let dir = tempfile::tempdir().unwrap();
        let parent = dir.path().join("extracted");
        fs::create_dir(&parent).unwrap();
        fs::write(parent.join("beside.txt"), b"beside").unwrap();
        let dumps = dir.path().join("dumps");
        fs::create_dir(&dumps).unwrap();
        Folders {
            _dir: dir,
            parent,
            dumps,
        }
    }

    /// Writes `bytes` as the dump named `name`, and gives its path.
    fn dump(&self, name: &str, bytes: &[u8]) -> PathBuf {
        let path = self.dumps.join(name);
        fs::write(&path, bytes).unwrap();
        path
    }

    /// Writes the files of the disc `set` into the new folder `folder` of the dump folder, and
    /// gives the path of its sheet.
    fn disc(&self, folder: &str, set: &DiscSet) -> PathBuf {
        let folder = self.dumps.join(folder);
        fs::create_dir(&folder).unwrap();
        set.write(&folder).unwrap()
    }

    /// What a test compares after a refusal: the parent whole, its own entry included, and the
    /// dump folder whole, taken once every time of change is long before it, so that an entry made
    /// and removed again shows as well as one changed.
    fn whole(&self) -> Before {
        backdate(&self.parent).unwrap();
        backdate(&self.dumps).unwrap();
        Before {
            parent: snapshot(&self.parent).unwrap(),
            dumps: snapshot(&self.dumps).unwrap(),
        }
    }

    /// What a test compares after a success: what lies in the parent outside its stagings, and the
    /// dump folder whole, taken as [`whole`](Self::whole) takes them.
    fn outside(&self) -> Before {
        backdate(&self.parent).unwrap();
        backdate(&self.dumps).unwrap();
        Before {
            parent: snapshot_outside(&self.parent, STAGINGS).unwrap(),
            dumps: snapshot(&self.dumps).unwrap(),
        }
    }

    /// Checks, after a refusal that came before anything was made, that the parent and the dump
    /// folder are as [`whole`](Self::whole) found them.
    #[track_caller]
    fn assert_untouched(&self, before: &Before) {
        assert!(
            snapshot(&self.parent).unwrap() == before.parent,
            "the parent changed"
        );
        self.assert_dumps_unchanged(before);
    }

    /// Checks, after extractions into the outputs named `outputs`, that the parent holds no
    /// staging and is as [`outside`](Self::outside) found it outside the outputs, and that the
    /// dump folder is as it was. A staging that a scanner held something of as the program
    /// dropped it is removed once the hold ends, as that drop ignores a failed removal; only
    /// Windows' scanners hold files so, so a staging left on another system fails the check.
    #[track_caller]
    fn assert_only_made(&self, outputs: &[&str], before: &Before) {
        for entry in fs::read_dir(&self.parent).unwrap() {
            let entry = entry.unwrap();
            if !entry
                .file_name()
                .as_encoded_bytes()
                .starts_with(STAGINGS.as_bytes())
            {
                continue;
            }
            let left = entry.path();
            if cfg!(windows) {
                if let Err(error) = remove_once_let_go(&left) {
                    panic!("a staging was left: {}: {error}", left.display());
                }
            } else {
                panic!("a staging was left: {}", left.display());
            }
        }
        let mut outside = snapshot_outside(&self.parent, STAGINGS).unwrap();
        outside.retain(|path, _| !outputs.iter().any(|output| path.starts_with(output)));
        assert!(
            outside == before.parent,
            "the parent changed outside the outputs"
        );
        self.assert_dumps_unchanged(before);
    }

    /// Checks that the dump folder is as it was: a dump is only ever read.
    #[track_caller]
    fn assert_dumps_unchanged(&self, before: &Before) {
        assert!(
            snapshot(&self.dumps).unwrap() == before.dumps,
            "the dump folder changed"
        );
    }
}

/// What lay in a test's folders before an extraction.
struct Before {
    /// The parent: whole, or outside its stagings.
    parent: BTreeMap<PathBuf, EntryState>,
    /// The dump folder whole.
    dumps: BTreeMap<PathBuf, EntryState>,
}

#[test]
fn extract_writes_the_n64_golden_tree() {
    let folders = Folders::new();
    let (name, image) = default_rom();
    let dump = folders.dump(&name, &image);
    let before = folders.outside();
    // The output given as a relative path, taken from the folder the program runs in, which is
    // reported made absolute.
    let output = run_program_in(
        &folders.parent,
        &extract_args("n64", &dump, Path::new("n64"), true),
    );
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stderr), "");
    let made = folders.parent.join("n64");
    assert_eq!(
        text(&output.stdout),
        format!(
            "Extracted n64 (script) into {}: 2 files, 0 programs, 1 maps\n",
            made.display()
        )
    );
    let tree = listed(&vectors(N64_VECTORS)["trees"]["default"]);
    assert_eq!(tree_listing(&made), tree);
    folders.assert_only_made(&["n64"], &before);
}

#[test]
fn extract_writes_the_dreamcast_golden_trees_from_each_form() {
    let folders = Folders::new();
    let set = DiscSet::full(Edc::Zero);
    let sheet = folders.disc("cue", &set);
    let folder = sheet.parent().unwrap().to_path_buf();
    // The second data track's file, which holds the disc's files.
    let track = folder.join(&set.files[1].0);
    let gdi = folders.disc("gdi", &DiscSet::full_gdi(Edc::Zero));
    let dreamcast = vectors(DREAMCAST_VECTORS);
    let tree = listed(&dreamcast["sets"]["full"]["tree"]);
    let before = folders.outside();

    // The cue sheet, the folder that holds it, and a track's file that it names.
    for (form, dump) in [("sheet", &sheet), ("folder", &folder), ("track", &track)] {
        let made = folders.parent.join(form);
        let output = run_program(&extract_args("dreamcast", dump, &made, true));
        assert_eq!(output.status.code(), Some(0), "{form}: {output:?}");
        assert_eq!(text(&output.stderr), "", "{form}");
        assert_eq!(tree_listing(&made), tree, "{form}");
    }

    // The same sectors behind a GDI: every file is the cue set's but the disc manifest, which
    // records the GDI's own data tracks.
    let made = folders.parent.join("gdi");
    let output = run_program(&extract_args("dreamcast", &gdi, &made, true));
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    let disc_manifest = "disc-manifest.json";
    let others = |listing: Vec<Listed>| -> Vec<Listed> {
        let kept = listing.into_iter();
        kept.filter(|(path, _, _)| path != disc_manifest).collect()
    };
    assert_eq!(others(tree_listing(&made)), others(tree));
    let written: Value = serde_json::from_slice(&fs::read(made.join(disc_manifest)).unwrap())
        .expect("the disc manifest is JSON");
    let mut expected = vectors(FULL_DISC_MANIFEST);
    let tracks = dreamcast["sets"]["fullGdi"]["dataTracks"]
        .as_array()
        .unwrap();
    expected["data_tracks"] = tracks
        .iter()
        .map(|row| json!({"file": row[0], "index": row[1], "first_lba": row[2], "sectors": row[3]}))
        .collect();
    assert_eq!(written, expected);

    folders.assert_only_made(&["sheet", "folder", "track", "gdi"], &before);
}

#[test]
fn extract_refuses_and_leaves_the_folder_as_it_was() {
    let folders = Folders::new();
    // The N64 extractor's corrupt stream: the default image's first stored byte made one that no
    // raw DEFLATE stream begins with.
    let (name, mut image) = default_rom();
    let rom_offset = vectors(N64_DEFAULT_MANIFEST)["files"][0]["rom_offset"].as_u64();
    let first = usize::try_from(rom_offset.expect("the first file's place")).unwrap();
    image[first] = NO_STREAM;
    let rom = folders.dump(&name, &image);
    // A disc without its native table.
    let sheet = folders.disc("small", &DiscSet::small(Edc::Zero));
    for (word, dump) in [("n64", &rom), ("dreamcast", &sheet)] {
        let before = folders.whole();
        let output = run_program(&extract_args(word, dump, &folders.parent.join(word), true));
        assert_eq!(output.status.code(), Some(1), "{word}: {output:?}");
        let err = text(&output.stderr);
        assert!(err.starts_with("error: source.damaged ("), "{err}");
        assert_eq!(err.lines().count(), 1, "{err}");
        assert_eq!(text(&output.stdout), "");
        // Refused as the dump was checked, before even the staging was made.
        folders.assert_untouched(&before);
    }
}

#[test]
fn install_refusals_name_their_codes() {
    let folders = Folders::new();
    // The default image names no cartridge of the game, and its digest is not the one the
    // launcher's trust holds.
    let (name, image) = default_rom();
    let rom = folders.dump(&name, &image);
    let before = folders.whole();
    let output = run_program(&extract_args(
        "n64",
        &rom,
        &folders.parent.join("n64"),
        false,
    ));
    assert_eq!(output.status.code(), Some(1), "{output:?}");
    assert_eq!(
        text(&output.stderr),
        "error: source.otherGame (otherGame): the dump is refused for an installation: \
         otherGame\n"
    );
    assert_eq!(text(&output.stdout), "");
    folders.assert_untouched(&before);

    // A pressed disc's dump of the full set, which is the US disc by its boot area, but whose
    // files the launcher's fingerprints do not hold.
    let sheet = folders.disc("full", &DiscSet::full(Edc::Computed).with_audio_files());
    let before = folders.whole();
    let made = folders.parent.join("dreamcast");
    let output = run_program(&extract_args("dreamcast", &sheet, &made, false));
    assert_eq!(output.status.code(), Some(1), "{output:?}");
    let err = text(&output.stderr);
    let prefix = "error: dump.hashMismatch (fingerprintMissing): ";
    assert!(err.starts_with(prefix), "{err}");
    assert!(err.ends_with(": fingerprintMissing\n"), "{err}");
    assert_eq!(err.lines().count(), 1, "{err}");
    assert_eq!(text(&output.stdout), "");
    folders.assert_untouched(&before);
}

#[test]
fn an_output_inside_an_installation_is_refused() {
    let Some(platform) = this_platform() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let runtime = RuntimeFiles::standard(platform, VERSION);
    let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
    backdate(&fixture.root).unwrap();
    let installed = snapshot(&fixture.root).unwrap();
    // The dump is never looked for: the output's place is refused first. In the installation's
    // own folder, where an expansion's folder goes, and in a folder of its game's.
    let dump = dir.path().join("missing.z64");
    for made in [
        fixture.root.join("legends"),
        fixture.root.join("Gauntlet").join("legends"),
    ] {
        let output = run_program(&extract_args("n64", &dump, &made, true));
        let named = made.display();
        assert_eq!(output.status.code(), Some(1), "{named}: {output:?}");
        assert_eq!(
            text(&output.stderr),
            format!(
                "error: folder.insideInstallation (insideInstallation): the output lies in an \
                 installation, whose folder holds .gdl-launcher: {}\n",
                fixture.root.display()
            ),
            "{named}"
        );
        assert_eq!(text(&output.stdout), "", "{named}");
    }
    assert!(
        snapshot(&fixture.root).unwrap() == installed,
        "the installation changed"
    );
}
