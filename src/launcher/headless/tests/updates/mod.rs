//! What the test programs that update an installation through the built program, and recover it,
//! share: the phases a run tells, those an update from the program's own file and a recovery that
//! takes an update back go through, the installation's tree outside its state folder, what that
//! folder keeps once the jobs are done, and the receipt.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use gdl_fixtures::{folder_names, tree_entries};
use gdl_install::{JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE, STATE_DIR};
use serde_json::Value;

/// The phases an update from the program's own file goes through, in turn.
pub const BUNDLED_PHASES: [&str; 6] = [
    "checkInstalled",
    "verifyDownload",
    "stage",
    "checkGame",
    "swap",
    "cleanup",
];

/// The phases a recovery that takes an update back goes through, in turn.
pub const RECOVERY_PHASES: [&str; 2] = ["recoverUpdate", "finishCleanup"];

/// The phases a run's output tells, in turn.
pub fn phases_of(out: &str) -> Vec<&str> {
    out.lines()
        .filter_map(|line| line.strip_prefix("phase: "))
        .collect()
}

/// Every entry of the installation at `root` outside its state folder, as [`tree_entries`] gives
/// them.
pub fn outside_state(root: &Path) -> BTreeMap<String, Option<String>> {
    let mut entries = tree_entries(root);
    entries.retain(|path, _| path != STATE_DIR && !path.starts_with(&format!("{STATE_DIR}/")));
    entries
}

/// Checks that the state folder of the installation at `root` holds only what an installation
/// keeps once its jobs are done: its receipt, its lock and an empty journal folder; no staging,
/// download, backup or retired launcher copy is left.
#[track_caller]
pub fn assert_only_kept_state(root: &Path) {
    let state = root.join(STATE_DIR);
    assert_eq!(
        folder_names(&state).unwrap(),
        [JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE]
    );
    assert!(folder_names(&state.join(JOURNAL_DIR)).unwrap().is_empty());
}

/// The receipt of the installation at `root`.
pub fn receipt_of(root: &Path) -> Value {
    serde_json::from_slice(&fs::read(root.join(STATE_DIR).join(RECEIPT_FILE)).unwrap()).unwrap()
}
