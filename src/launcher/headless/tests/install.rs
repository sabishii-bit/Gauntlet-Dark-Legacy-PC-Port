//! A fresh install of a synthetic GameCube disc made through the launcher's public library alone,
//! from a launcher file whose runtime's executable is the headless program, which answers the
//! game's version probe as the game does and on Windows takes the disc's icon; and the recovery of
//! installs cut short, each folder judged by the headless program's inspection.

mod common;

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use gdl_files::{Cancel, Job, NoProgress, PhaseId, Progress, RelPath, STAGING_RANDOM_CHARS};
use gdl_fixtures::{
    ICON_DISC_FILES, RuntimeFiles, append_sections, folder_names, icon_disc_receipt, icon_iso,
    pretty_json, tpl, tree_entries,
};
use gdl_install::{
    CHECK_DISC, CHECK_GAME, COPY_DISC_FILES, COPY_LAUNCHER, DISC_FILES, DISC_ICON, FolderRecord,
    GET_RUNTIME, GUNE5D, INSTALL_JOURNAL, INSTALL_STAGING_PREFIX, InstallPlan, Installed,
    JOURNAL_DIR, JOURNAL_SCHEMA, LOCK_FILE, MIN_LAUNCHER, PERSONALISE, PREPARE, PUBLISH,
    RECEIPT_FILE, RUNTIME_SECTION, RecoveryOutcome, STATE_DIR, Sections, Unrecorded, install_game,
    installed_launcher_name, plan_install, probe_game_version, recover_install,
};
use gdl_media::GameCubeImage;
use gdl_release::{Platform, Version};
use serde_json::{Value, json};
use tempfile::TempDir;

use common::{PROGRAM, VERSION, inspect_json};

/// The random part of the staging that the journal of an install cut short names.
const STAGING_RANDOM_PART: &str = "AbC123xy";

/// How many roots the journal of the install cut short records in place: the publish was cut short
/// after the next root moved, before its move was recorded.
const RECORDED_ROOTS: usize = 4;

/// How many folders the install cut short made: the folder it installed into.
const MADE_FOLDERS: u32 = 1;

/// The platform this program was built for, whose runtime the tests install; `None`, the reason
/// printed, on a system no release is built for.
fn this_platform() -> Option<Platform> {
    let platform = Platform::of_this_build();
    if platform.is_none() {
        eprintln!("skipped: no release is built for this system");
    }
    platform
}

/// The disc and the launcher file an install installs from, written as the player's image and the
/// running launcher's file are: the icon disc, and a copy of the headless program carrying a
/// runtime whose executable is the headless program too.
struct Inputs {
    /// The folder both lie in.
    source: TempDir,
    /// The runtime the launcher file carries.
    runtime: RuntimeFiles,
    /// The headless program's bytes, the runtime's executable as shipped.
    program: Vec<u8>,
    /// The launcher file's bytes.
    launcher: Vec<u8>,
}

impl Inputs {
    /// The image's name in the source folder.
    const IMAGE: &str = "game.iso";

    /// The launcher file's name in the source folder.
    const LAUNCHER: &str = "launcher";

    /// The inputs of an install for `platform`. The runtime's executable is stored in its archive
    /// rather than deflated, so that its megabytes build fast; the release record names its bytes.
    fn new(platform: Platform) -> Inputs {
        let program = fs::read(PROGRAM).unwrap();
        let mut runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        let zip = runtime.zip_with_program(&program);
        // The sections follow the program as a release appends them to the launcher file.
        let mut launcher = program.clone();
        append_sections(&mut launcher, &[(RUNTIME_SECTION, &zip)], 0);
        let source = tempfile::tempdir().unwrap();
        fs::write(source.path().join(Self::IMAGE), icon_iso()).unwrap();
        fs::write(source.path().join(Self::LAUNCHER), &launcher).unwrap();
        Inputs {
            source,
            runtime,
            program,
            launcher,
        }
    }

    /// The launcher file's path.
    fn launcher_path(&self) -> PathBuf {
        self.source.path().join(Self::LAUNCHER)
    }

    /// Plans and installs the inputs into `destination` as the launcher does, with the system's
    /// own probe and, on Windows, its own icon edit and shell, telling `record` and `progress`, and
    /// gives back the plan and what the install gave. The plan must be ready and the install must
    /// stand.
    fn install(
        &self,
        destination: &Path,
        record: &mut dyn FolderRecord,
        progress: &dyn Progress,
    ) -> (InstallPlan, Installed) {
        let mut image = GameCubeImage::open(&self.source.path().join(Self::IMAGE)).unwrap();
        let mut launcher = Sections::of_path(&self.launcher_path())
            .unwrap()
            .expect("the launcher file carries sections");
        let plan = plan_install(&mut image, &mut launcher, destination, VERSION)
            .unwrap_or_else(|error| panic!("{error:?}"));
        assert!(plan.is_ready(), "{:?}", plan.blockers);
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress,
        };
        let installed = install_game(&plan, &mut image, &mut launcher, record, &job)
            .unwrap_or_else(|failure| panic!("{failure:?}"));
        (plan, installed)
    }
}

/// A record of installations that keeps what it is told, by the call's name, in order.
#[derive(Default)]
struct Heard {
    calls: Vec<(&'static str, PathBuf)>,
}

impl FolderRecord for Heard {
    fn pending(&mut self, destination: &Path) -> io::Result<()> {
        self.calls.push(("pending", destination.to_path_buf()));
        Ok(())
    }

    fn installed(&mut self, destination: &Path) -> io::Result<()> {
        self.calls.push(("installed", destination.to_path_buf()));
        Ok(())
    }

    fn forget(&mut self, destination: &Path) -> io::Result<()> {
        self.calls.push(("forget", destination.to_path_buf()));
        Ok(())
    }
}

/// A progress that keeps what it is told: the phases, and every advance.
#[derive(Default)]
struct Told {
    phases: Mutex<Vec<&'static str>>,
    advances: Mutex<Vec<(u64, u64, Option<String>)>>,
}

impl Progress for Told {
    fn phase(&self, phase: PhaseId) {
        self.phases.lock().unwrap().push(phase.0);
    }

    fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
        let item = item.map(|item| item.as_str().to_owned());
        self.advances.lock().unwrap().push((done, total, item));
    }
}

/// Checks that the headless program inspects the folder at `folder` as the installation of the
/// release the tests install, for `platform`, that this program may change.
#[track_caller]
fn assert_inspected_installed(folder: &Path, platform: Platform) {
    let report = inspect_json(folder);
    assert_eq!(report["state"], "installed", "{report}");
    assert_eq!(report["path"], folder.display().to_string());
    assert_eq!(report["code"], Value::Null, "{report}");
    assert_eq!(report["journals"], json!([]), "{report}");
    let installation = &report["installation"];
    assert_eq!(installation["version"], VERSION, "{report}");
    assert_eq!(installation["platform"], platform.as_str(), "{report}");
    assert_eq!(installation["disc"], GUNE5D, "{report}");
    assert_eq!(installation["discRevision"], 1, "{report}");
    assert_eq!(installation["launcher"], VERSION, "{report}");
    assert_eq!(installation["relation"], "same", "{report}");
    assert_eq!(installation["access"], "writable", "{report}");
}

#[test]
fn a_synthetic_disc_installs_end_to_end() {
    let Some(platform) = this_platform() else {
        return;
    };
    let inputs = Inputs::new(platform);
    let top = tempfile::tempdir().unwrap();
    let destination = top.path().join("game");
    let mut record = Heard::default();
    let told = Told::default();
    let (plan, installed) = inputs.install(&destination, &mut record, &told);

    // The staged game answered the probe, and nothing else went wrong.
    assert!(installed.notices.is_empty(), "{:?}", installed.notices);
    assert_eq!(installed.folder.root(), destination);
    assert_eq!(
        record.calls,
        [
            ("pending", destination.clone()),
            ("installed", destination.clone())
        ]
    );
    let mut phases = vec![CHECK_DISC, PREPARE, GET_RUNTIME, COPY_DISC_FILES];
    if platform == Platform::WindowsX64 {
        phases.push(PERSONALISE);
    }
    phases.extend([COPY_LAUNCHER, CHECK_GAME, PUBLISH]);
    let expected: Vec<&str> = phases.iter().map(|phase| phase.0).collect();
    assert_eq!(*told.phases.lock().unwrap(), expected);
    let advances = told.advances.lock().unwrap();
    let (done, total, item) = advances
        .last()
        .cloned()
        .expect("the install told its progress");
    assert_eq!((done, item.as_deref()), (total, None));
    assert!(
        advances
            .iter()
            .all(|&(done, of, _)| of == total && done <= total),
        "the progress passed its total"
    );
    drop(advances);

    // The folder's top holds the plan's roots and the launcher's state, which holds what an
    // installation keeps.
    let mut top_names: Vec<&str> = plan.roots.iter().map(RelPath::as_str).collect();
    top_names.push(STATE_DIR);
    top_names.sort();
    assert_eq!(folder_names(&destination).unwrap(), top_names);
    let state = destination.join(STATE_DIR);
    assert_eq!(
        folder_names(&state).unwrap(),
        [DISC_FILES, JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE]
    );
    assert!(folder_names(&state.join(JOURNAL_DIR)).unwrap().is_empty());

    // The disc's files as the disc holds them, and the list of what was copied.
    let mut war = vec![b'A'; 16];
    war.extend([0; 16]);
    assert_eq!(
        fs::read(destination.join("Gauntlet/PDATA/WAR.WAD")).unwrap(),
        war
    );
    assert_eq!(
        fs::read(destination.join("Gauntlet/WDATA/TOWER.WAD")).unwrap(),
        b"tower"
    );
    assert_eq!(fs::read(destination.join(DISC_ICON)).unwrap(), tpl(5, 8, 8));
    assert_eq!(
        fs::read_to_string(state.join(DISC_FILES)).unwrap(),
        ICON_DISC_FILES
    );

    // The launcher's copy is the launcher file's bytes, and the file is as it was.
    let copy = fs::read(destination.join(installed_launcher_name(platform))).unwrap();
    assert!(copy == inputs.launcher, "the launcher's copy differs");
    drop(copy);
    assert!(
        fs::read(inputs.launcher_path()).unwrap() == inputs.launcher,
        "the launcher file changed"
    );

    // The executable is the shipped program, with the disc's icon set into it on Windows, and
    // still answers with the launcher's version; the receipt records it so.
    let executable = destination.join(platform.executable());
    let installed_program = fs::read(&executable).unwrap();
    if platform == Platform::WindowsX64 {
        assert!(
            installed_program != inputs.program,
            "the executable is as it was shipped"
        );
    } else {
        assert!(
            installed_program == inputs.program,
            "the executable is not as it was shipped"
        );
    }
    let receipt = icon_disc_receipt(&inputs.runtime, &inputs.launcher, &installed_program);
    drop(installed_program);
    assert_eq!(
        fs::read_to_string(state.join(RECEIPT_FILE)).unwrap(),
        receipt
    );
    let written: Value = serde_json::from_str(&receipt).unwrap();
    assert_eq!(serde_json::to_value(&installed.receipt).unwrap(), written);
    let probe = probe_game_version(&executable).unwrap();
    assert!(probe.succeeded(), "{probe:?}");
    assert_eq!(probe.first_line.as_deref(), Some(VERSION), "{probe:?}");

    assert_inspected_installed(&destination, platform);
}

#[test]
fn a_killed_install_reopens_as_pending_and_recovers() {
    let Some(platform) = this_platform() else {
        return;
    };
    assert_eq!(STAGING_RANDOM_PART.len(), STAGING_RANDOM_CHARS);
    let running = Version::parse(VERSION).unwrap();
    let inputs = Inputs::new(platform);
    let top = tempfile::tempdir().unwrap();
    let destination = top.path().join("game");
    let (plan, installed) = inputs.install(&destination, &mut Unrecorded, &NoProgress);
    let whole = tree_entries(&destination);

    // What a kill in the publish leaves, a journal written as the install writes it: the first
    // roots recorded in place, the next in place but not yet recorded, the rest still in the
    // staging, and no receipt.
    let state = destination.join(STATE_DIR);
    let staging_name = format!("{INSTALL_STAGING_PREFIX}{STAGING_RANDOM_PART}");
    let staging = state.join(&staging_name);
    fs::create_dir(&staging).unwrap();
    let roots: Vec<&str> = plan.roots.iter().map(RelPath::as_str).collect();
    for root in &roots[RECORDED_ROOTS + 1..] {
        fs::rename(destination.join(root), staging.join(root)).unwrap();
    }
    let receipt: Value =
        serde_json::from_slice(&fs::read(state.join(RECEIPT_FILE)).unwrap()).unwrap();
    fs::remove_file(state.join(RECEIPT_FILE)).unwrap();
    let journal = json!({
        "schema": JOURNAL_SCHEMA,
        "minLauncher": MIN_LAUNCHER,
        "phase": "publishing",
        "madeFolders": MADE_FOLDERS,
        "staging": staging_name,
        "roots": roots,
        "published": &roots[..RECORDED_ROOTS],
        "receipt": receipt,
    });
    fs::write(
        state.join(JOURNAL_DIR).join(INSTALL_JOURNAL),
        pretty_json(&journal),
    )
    .unwrap();

    let report = inspect_json(&destination);
    assert_eq!(report["state"], "pending", "{report}");
    assert_eq!(report["journals"], json!([INSTALL_JOURNAL]), "{report}");
    assert_eq!(report["installation"], Value::Null, "{report}");

    // The recovery finishes the publish: every root in place and the receipt the install wrote,
    // the tree the whole install made.
    let cancel = Cancel::default();
    let job = Job {
        cancel: &cancel,
        progress: &NoProgress,
    };
    let mut record = Heard::default();
    let outcome = recover_install(&destination, &running, &mut record, &job).unwrap();
    assert_eq!(
        outcome,
        RecoveryOutcome::Finished {
            receipt: Box::new(installed.receipt.clone())
        }
    );
    assert_eq!(record.calls, [("installed", destination.clone())]);
    assert!(
        tree_entries(&destination) == whole,
        "the recovered tree differs"
    );
    assert_inspected_installed(&destination, platform);

    // A state folder an install left before its first journal, holding only its lock: given up,
    // and the folder left as it was before the install, empty.
    let abandoned = top.path().join("abandoned");
    let abandoned_state = abandoned.join(STATE_DIR);
    fs::create_dir_all(&abandoned_state).unwrap();
    fs::write(abandoned_state.join(LOCK_FILE), b"").unwrap();
    let report = inspect_json(&abandoned);
    assert_eq!(report["state"], "pending", "{report}");
    assert_eq!(report["journals"], json!([]), "{report}");
    let mut record = Heard::default();
    let outcome = recover_install(&abandoned, &running, &mut record, &job).unwrap();
    assert_eq!(outcome, RecoveryOutcome::AbandonedRemoved);
    assert_eq!(record.calls, [("forget", abandoned.clone())]);
    assert!(folder_names(&abandoned).unwrap().is_empty());
    let report = inspect_json(&abandoned);
    assert_eq!(report["state"], "empty", "{report}");
}
