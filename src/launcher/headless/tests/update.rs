//! An update and a recovery through the built headless program, end to end: the installation's
//! launcher copy updates the installation from the runtime its own file carries, with no feed, and
//! hands over to the copy it put in place; and a recovery run by a new copy that an update left in
//! place takes the update back, puts the installed copy back, and hands over to it. Each copy that
//! is handed over to waits for the one before to exit and sweeps the retired copies.

mod common;
mod launchers;

use std::collections::BTreeMap;
use std::env::consts::EXE_SUFFIX;
use std::ffi::OsStr;
use std::fs;
use std::path::Path;

use gdl_files::STAGING_RANDOM_CHARS;
use gdl_fixtures::{folder_names, pretty_json, sha256_hex, tree_entries};
use gdl_install::{
    BUILD_INFO, JOURNAL_DIR, JOURNAL_SCHEMA, LOCK_FILE, MIN_LAUNCHER, RECEIPT_FILE,
    RETIRED_LAUNCHER_PREFIX, STATE_DIR, UPDATE_BACKUP, UPDATE_JOURNAL, UPDATE_STAGING_PREFIX,
    VERSION_FILE, installed_launcher_name, probe_game_version,
};
use gdl_release::Platform;
use serde_json::{Value, json};

use common::{VERSION, inspect_json};
use launchers::{
    Launched, OLDER, installation, launcher_file, one_run_at_a_time, past_progress, this_platform,
    waited, write_launcher,
};

/// The random part of the staging the hand-written journal names.
const STAGING_RANDOM: &str = "AbC123xy";

/// The phases an update from the program's own file goes through, in turn.
const BUNDLED_PHASES: [&str; 6] = [
    "checkInstalled",
    "verifyDownload",
    "stage",
    "checkGame",
    "swap",
    "cleanup",
];

/// The phases a recovery that takes an update back goes through, in turn.
const RECOVERY_PHASES: [&str; 2] = ["recoverUpdate", "finishCleanup"];

/// The names the hand-written journal's update moved aside and put in place: the two files every
/// release changes.
const MOVED: [&str; 2] = [VERSION_FILE, BUILD_INFO];

/// The phases a run's output tells, in turn.
fn phases_of(out: &str) -> Vec<&str> {
    out.lines()
        .filter_map(|line| line.strip_prefix("phase: "))
        .collect()
}

/// Every entry of the installation at `root` outside its state folder, as
/// [`tree_entries`] gives them.
fn outside_state(root: &Path) -> BTreeMap<String, Option<String>> {
    let mut entries = tree_entries(root);
    entries.retain(|path, _| path != STATE_DIR && !path.starts_with(&format!("{STATE_DIR}/")));
    entries
}

/// Checks that the state folder of the installation at `root` holds only what an installation
/// keeps once its jobs are done: its receipt, its lock and an empty journal folder; no staging,
/// download, backup or retired launcher copy is left.
#[track_caller]
fn assert_only_kept_state(root: &Path) {
    let state = root.join(STATE_DIR);
    assert_eq!(
        folder_names(&state).unwrap(),
        [JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE]
    );
    assert!(folder_names(&state.join(JOURNAL_DIR)).unwrap().is_empty());
}

/// The receipt of the installation at `root`.
fn receipt_of(root: &Path) -> Value {
    serde_json::from_slice(&fs::read(root.join(STATE_DIR).join(RECEIPT_FILE)).unwrap()).unwrap()
}

/// Checks that `line` is the one an update writes when the launcher copy it moved aside, from
/// `from`, could not be removed, as a launcher running from it holds it on Windows: its retired
/// name, with a staging's random part.
#[track_caller]
fn assert_left_retired(line: &str, from: &str) {
    let random = line
        .strip_prefix(&format!("retired: left {RETIRED_LAUNCHER_PREFIX}{from}-"))
        .and_then(|rest| rest.strip_suffix(EXE_SUFFIX))
        .unwrap_or_else(|| panic!("{line:?}"));
    assert!(
        random.len() == STAGING_RANDOM_CHARS && random.bytes().all(|b| b.is_ascii_alphanumeric()),
        "{line:?}"
    );
}

#[test]
fn the_installed_copy_updates_itself_offline_and_hands_over() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let (runtime, launcher) = launcher_file(platform, VERSION);
    let fixture = installation(dir.path(), platform, &launcher);
    let root = fixture.root.as_path();
    let before = outside_state(root);

    // The installed copy updates its own installation from the runtime its file carries.
    let copy = root.join(installed_launcher_name(platform));
    let args = [
        OsStr::new("--update"),
        root.as_os_str(),
        OsStr::new("--yes"),
        OsStr::new("--offline"),
    ];
    let mut run = Launched::start(&copy, &args);
    let finished = run.finish();
    assert!(finished.status.success(), "{finished:#?}");
    assert_eq!(finished.err, "", "{finished:#?}");
    assert_eq!(phases_of(&finished.out), BUNDLED_PHASES, "{finished:#?}");
    let handed_over = finished.handed_over.expect("the update handed over");
    assert_ne!(handed_over, run.pid());

    // The update's report, then, after its hand-over, the new copy's: it waited for the old copy
    // to exit, then swept the copy the old one ran from. On Windows the old copy could not remove
    // that while it ran from it, and the new copy removed it; Linux removes a running program's
    // file, so the old copy removed it itself and the new copy found none.
    let lines = past_progress(&finished.out);
    let [updated, probed, retired, handed, previous, swept] = lines[..] else {
        panic!("{finished:#?}");
    };
    assert_eq!(updated, format!("updated: {OLDER} -> {VERSION} (bundled)"));
    // The game changed, so the staged one was probed.
    assert_eq!(probed, "probed: staged");
    assert_eq!(handed, format!("handedOver: {handed_over}"));
    assert!(waited(previous).is_some(), "{previous:?}");
    if platform == Platform::WindowsX64 {
        assert_left_retired(retired, OLDER);
        assert_eq!(swept, "retired: removed 1, kept 0");
    } else {
        assert_eq!(retired, "retired: removed");
        assert_eq!(swept, "retired: removed 0, kept 0");
    }

    // The tree as the update leaves it: the files the release changed, and the rest as they were,
    // the launcher copy among them, which holds the bytes of the file that ran the update.
    let mut expected = before;
    let file = |name: &str| {
        let (_, data) = runtime
            .files
            .iter()
            .find(|(file, _)| file == name)
            .expect("the runtime holds the file");
        sha256_hex(data)
    };
    let build_info = sha256_hex(&runtime.build_info_bytes());
    expected.insert(VERSION_FILE.to_owned(), Some(file(VERSION_FILE)));
    expected.insert(BUILD_INFO.to_owned(), Some(build_info.clone()));
    let executable = platform.executable();
    let program = file(executable);
    let receipt = receipt_of(root);
    let installed_executable = if platform == Platform::WindowsX64 {
        // The disc's icon is set into it, so it holds other bytes than the program; the receipt
        // records which.
        let digest = receipt["installedDigests"][executable].as_str().unwrap();
        assert_ne!(digest, program);
        digest.to_owned()
    } else {
        program
    };
    expected.insert(executable.to_owned(), Some(installed_executable));
    let copy_digest = expected[installed_launcher_name(platform)].clone();
    assert!(
        outside_state(root) == expected,
        "the tree is not the update's"
    );
    let probe = probe_game_version(&root.join(executable)).unwrap();
    assert_eq!(probe.first_line.as_deref(), Some(VERSION), "{probe:?}");
    assert_eq!(receipt["version"], VERSION);
    assert_eq!(
        receipt["launcher"]["sha256"].as_str(),
        copy_digest.as_deref()
    );
    assert_eq!(receipt["buildInfoSha256"], build_info);
    assert_only_kept_state(root);
    let report = inspect_json(root);
    assert_eq!(report["state"], "installed", "{report}");
    assert_eq!(report["installation"]["version"], VERSION, "{report}");
}

#[test]
fn a_recovery_that_takes_its_own_copy_back_hands_over() {
    let Some(platform) = this_platform() else {
        return;
    };
    assert_eq!(STAGING_RANDOM.len(), STAGING_RANDOM_CHARS);
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    // The installed copy is a launcher file of the older release's; the update's new copy is one
    // of the program's own release.
    let (_, old_copy) = launcher_file(platform, OLDER);
    let (runtime, new_copy) = launcher_file(platform, VERSION);
    let fixture = installation(dir.path(), platform, &old_copy);
    let root = fixture.root.as_path();
    let before = outside_state(root);
    let state = root.join(STATE_DIR);
    let receipt = fs::read(state.join(RECEIPT_FILE)).unwrap();

    // What an update that decided to take itself back leaves once every move was made, when it
    // is killed before the rollback's first move: the files it changed in the backup and the new
    // ones in place, the installed copy retired and the new copy in its place, the staging empty,
    // and its journal rolling back.
    let staging = format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}");
    fs::create_dir(state.join(&staging)).unwrap();
    let backup = state.join(UPDATE_BACKUP);
    fs::create_dir(&backup).unwrap();
    for name in MOVED {
        fs::rename(root.join(name), backup.join(name)).unwrap();
    }
    let version_file = runtime
        .files
        .iter()
        .find(|(name, _)| name == VERSION_FILE)
        .map(|(_, data)| data.clone())
        .expect("the runtime names its version");
    let build_info = runtime.build_info_bytes();
    fs::write(root.join(VERSION_FILE), version_file).unwrap();
    fs::write(root.join(BUILD_INFO), &build_info).unwrap();
    let launcher_name = installed_launcher_name(platform);
    let retired = format!("{RETIRED_LAUNCHER_PREFIX}{OLDER}-{STAGING_RANDOM}{EXE_SUFFIX}");
    fs::rename(root.join(launcher_name), state.join(&retired)).unwrap();
    write_launcher(&root.join(launcher_name), &new_copy);
    let mut new_receipt = fixture.receipt();
    new_receipt["version"] = json!(VERSION);
    new_receipt["buildInfoSha256"] = json!(sha256_hex(&build_info));
    new_receipt["launcher"]["sha256"] = json!(sha256_hex(&new_copy));
    let journal = json!({
        "schema": JOURNAL_SCHEMA,
        "minLauncher": MIN_LAUNCHER,
        "phase": "rollingBack",
        "from": OLDER,
        "to": VERSION,
        "staging": staging,
        "backup": MOVED,
        "publish": MOVED,
        "madeFolders": [],
        "launcher": launcher_name,
        "retired": retired,
        "receipt": new_receipt,
    });
    fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
    fs::write(
        state.join(JOURNAL_DIR).join(UPDATE_JOURNAL),
        pretty_json(&journal),
    )
    .unwrap();

    // The new copy recovers the installation, which takes the update back and moves the new copy
    // the recovery runs from aside, then hands over to the installed copy it put back.
    let args = [OsStr::new("--recover"), root.as_os_str()];
    let mut run = Launched::start(&root.join(launcher_name), &args);
    let finished = run.finish();
    assert!(finished.status.success(), "{finished:#?}");
    assert_eq!(finished.err, "", "{finished:#?}");
    assert_eq!(phases_of(&finished.out), RECOVERY_PHASES, "{finished:#?}");
    let handed_over = finished.handed_over.expect("the recovery handed over");
    assert_ne!(handed_over, run.pid());

    // On Windows the copy the recovery ran from could not be removed while it ran, so the
    // recovery leaves it, and the copy put back removes it once the recovery exited; Linux removes
    // a running program's file, so the recovery removed it and the copy put back found none.
    let new_retired = format!("{RETIRED_LAUNCHER_PREFIX}{VERSION}-{STAGING_RANDOM}{EXE_SUFFIX}");
    let left = format!("left: {new_retired}");
    let handed = format!("handedOver: {handed_over}");
    let mut expected = vec!["install: nothing", "update: rolledBack"];
    if platform == Platform::WindowsX64 {
        expected.push(&left);
    }
    expected.push(&handed);
    let lines = past_progress(&finished.out);
    let (recovered, after) = lines.split_at_checked(expected.len()).expect("the lines");
    assert_eq!(recovered, expected, "{finished:#?}");
    let [previous, swept] = after[..] else {
        panic!("{finished:#?}");
    };
    assert!(waited(previous).is_some(), "{previous:?}");
    let removed = if platform == Platform::WindowsX64 {
        "retired: removed 1, kept 0"
    } else {
        "retired: removed 0, kept 0"
    };
    assert_eq!(swept, removed);

    // The installation as it was before the update: the installed copy back at its name, the
    // files back in place, the receipt as it was, and nothing the update made or moved aside left.
    assert!(outside_state(root) == before, "the tree is not as it was");
    assert!(
        fs::read(root.join(launcher_name)).unwrap() == old_copy,
        "the installed copy is not back"
    );
    assert_eq!(fs::read(state.join(RECEIPT_FILE)).unwrap(), receipt);
    assert_only_kept_state(root);
    let report = inspect_json(root);
    assert_eq!(report["state"], "installed", "{report}");
    assert_eq!(report["installation"]["version"], OLDER, "{report}");
}
