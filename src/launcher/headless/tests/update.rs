//! An update and a recovery through the built headless program, end to end: the installation's
//! launcher copy updates the installation from the runtime its own file carries, with no feed, and
//! hands over to the copy it put in place; and a recovery run by a new copy that an update left in
//! place takes the update back, puts the installed copy back, and hands over to it, as an update
//! does whose own first recovery did so before it was refused. Each copy that is handed over to
//! waits for the one before to exit and sweeps the retired copies.

mod common;
mod launchers;
mod updates;

use std::collections::BTreeMap;
use std::env::consts::EXE_SUFFIX;
use std::ffi::OsStr;
use std::fs;
use std::path::Path;

use gdl_files::STAGING_RANDOM_CHARS;
use gdl_fixtures::{InstallFixture, pretty_json, sha256_hex};
use gdl_install::{
    BUILD_INFO, JOURNAL_DIR, JOURNAL_SCHEMA, MIN_LAUNCHER, RECEIPT_FILE, RETIRED_LAUNCHER_PREFIX,
    STATE_DIR, UPDATE_BACKUP, UPDATE_JOURNAL, UPDATE_STAGING_PREFIX, VERSION_FILE,
    installed_launcher_name, probe_game_version,
};
use gdl_release::Platform;
use serde_json::json;

use common::{VERSION, inspect_json};
use launchers::{
    Launched, OLDER, installation, launcher_file, one_run_at_a_time, past_progress, this_platform,
    waited, write_launcher,
};
use updates::{
    BUNDLED_PHASES, RECOVERY_PHASES, assert_only_kept_state, outside_state, phases_of, receipt_of,
};

/// The random part of the staging the hand-written journal names.
const STAGING_RANDOM: &str = "AbC123xy";

/// The names the hand-written journal's update moved aside and put in place: the two files every
/// release changes.
const MOVED: [&str; 2] = [VERSION_FILE, BUILD_INFO];

/// The phases an update from the program's own file goes through before it is refused for an
/// installed file changed since it was installed, in turn.
const REFUSED_PHASES: [&str; 3] = ["checkInstalled", "verifyDownload", "stage"];

/// An installed file no release in these tests changes, which the refused update's test changes
/// by hand.
const CHANGED_FILE: &str = "data/config.json";

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

/// An installation of [`OLDER`] as an update to the program's own release leaves it once it
/// decided to take itself back, killed after every move was made and before the rollback's first:
/// the files it changed in the backup and the new ones in place, the installed copy retired and
/// the new copy, a launcher file of the program's own release, in its place, the staging empty,
/// and its journal rolling back.
struct RollingBack {
    /// The installation.
    fixture: InstallFixture,
    /// The installed copy's bytes: a launcher file of the older release's.
    old_copy: Vec<u8>,
    /// The installation's entries outside its state folder before the update.
    before: BTreeMap<String, Option<String>>,
    /// Its receipt before the update.
    receipt: Vec<u8>,
}

/// Lays out in `dir`, for `platform`, the installation [`RollingBack`] describes.
fn rolling_back(dir: &Path, platform: Platform) -> RollingBack {
    assert_eq!(STAGING_RANDOM.len(), STAGING_RANDOM_CHARS);
    // The installed copy is a launcher file of the older release's; the update's new copy is one
    // of the program's own release.
    let (_, old_copy) = launcher_file(platform, OLDER);
    let (runtime, new_copy) = launcher_file(platform, VERSION);
    let fixture = installation(dir, platform, &old_copy);
    let root = fixture.root.as_path();
    let before = outside_state(root);
    let state = root.join(STATE_DIR);
    let receipt = fs::read(state.join(RECEIPT_FILE)).unwrap();

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
    RollingBack {
        fixture,
        old_copy,
        before,
        receipt,
    }
}

/// Checks that `lines`, the last three a run wrote on its output but its job's progress, are
/// `handedOver: <handed_over>` and the lines of the copy it handed over to on `platform`: it waited
/// for the run to exit, then swept the copy the run ran from, which on Windows the run could not
/// remove while it ran from it and on Linux the run removed itself.
#[track_caller]
fn assert_handed_over_to_the_copy_put_back(lines: &[&str], handed_over: u32, platform: Platform) {
    let [handed, previous, swept] = lines[..] else {
        panic!("{lines:#?}");
    };
    assert_eq!(handed, format!("handedOver: {handed_over}"));
    assert!(waited(previous).is_some(), "{previous:?}");
    let removed = if platform == Platform::WindowsX64 {
        "retired: removed 1, kept 0"
    } else {
        "retired: removed 0, kept 0"
    };
    assert_eq!(swept, removed);
}

#[test]
fn a_recovery_that_takes_its_own_copy_back_hands_over() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let RollingBack {
        fixture,
        old_copy,
        before,
        receipt,
    } = rolling_back(dir.path(), platform);
    let root = fixture.root.as_path();
    let state = root.join(STATE_DIR);
    let launcher_name = installed_launcher_name(platform);

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
    let mut expected = vec!["install: nothing", "update: rolledBack"];
    if platform == Platform::WindowsX64 {
        expected.push(&left);
    }
    let lines = past_progress(&finished.out);
    let (recovered, after) = lines.split_at_checked(expected.len()).expect("the lines");
    assert_eq!(recovered, expected, "{finished:#?}");
    assert_handed_over_to_the_copy_put_back(after, handed_over, platform);

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

#[test]
fn a_refused_update_whose_recovery_took_its_own_copy_back_hands_over() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let RollingBack {
        fixture,
        old_copy,
        mut before,
        receipt,
    } = rolling_back(dir.path(), platform);
    let root = fixture.root.as_path();
    let state = root.join(STATE_DIR);
    let launcher_name = installed_launcher_name(platform);
    // An installed file the interrupted update never moved, changed since, which the update
    // refuses to replace once its recovery has taken the interrupted one back.
    let changed = br#"{"changed": true}"#;
    fs::write(root.join(CHANGED_FILE), changed).unwrap();
    before.insert(CHANGED_FILE.to_owned(), Some(sha256_hex(changed)));

    // The new copy updates the installation from the runtime its own file carries: the update's
    // first recovery takes the interrupted update back, which moves the file the copy runs from
    // aside and puts the installed copy back, and then the update is refused. The copy put back
    // takes over all the same.
    let args = [
        OsStr::new("--update"),
        root.as_os_str(),
        OsStr::new("--yes"),
        OsStr::new("--offline"),
    ];
    let mut run = Launched::start(&root.join(launcher_name), &args);
    let finished = run.finish();
    assert_eq!(finished.status.code(), Some(1), "{finished:#?}");
    assert_eq!(
        finished.err,
        format!(
            "error: update.ownedFileChanged (ownedFileChanged): Installed runtime file was \
             modified or removed: {CHANGED_FILE}. Keep personal settings in \
             config/settings.json.\nchanges: none\n"
        ),
        "{finished:#?}"
    );
    assert_eq!(phases_of(&finished.out), REFUSED_PHASES, "{finished:#?}");
    let handed_over = finished
        .handed_over
        .expect("the refused update handed over");
    assert_ne!(handed_over, run.pid());
    assert_handed_over_to_the_copy_put_back(&past_progress(&finished.out), handed_over, platform);

    // The installation as it was before the interrupted update, but for the changed file.
    assert!(outside_state(root) == before, "the tree is not as it was");
    assert!(
        fs::read(root.join(launcher_name)).unwrap() == old_copy,
        "the installed copy is not back"
    );
    assert_eq!(fs::read(state.join(RECEIPT_FILE)).unwrap(), receipt);
    assert_only_kept_state(root);
}
