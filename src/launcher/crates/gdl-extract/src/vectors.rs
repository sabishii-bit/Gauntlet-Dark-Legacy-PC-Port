//! What the extraction's tests share: the shared vectors and the manifests the N64 extractor wrote
//! for their dumps, the folders a test extracts in and the one way it compares what lies outside a
//! staging before and after a fault, the N64 extraction run as the session runs it, and progress
//! that writes down what a job tells it or stops the job partway.

use std::collections::BTreeMap;
use std::ffi::OsStr;
use std::fs;
use std::panic::{self, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use gdl_files::{Cancel, DISC_NAMES, Job, PhaseId, Progress, RelPath, Staging, retry_locked};
use gdl_fixtures::{EntryState, backdate, sha256_hex, snapshot, snapshot_outside, unhex};
use gdl_media::N64Rom;
use serde_json::Value;
use tempfile::TempDir;

use crate::n64::{check_n64_with, extract_n64_with};
use crate::{ExtractError, ExtractSettings, N64Extraction};

/// The N64 cases.
const N64_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../../tests/scripts/data/legends/n64-vectors.json"
));

/// The manifests the N64 extractor wrote for the vectors' dumps, byte for byte, by the names the
/// vectors give them.
const N64_GOLDENS: [(&str, &[u8]); 4] = [
    (
        "n64-default-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-default-manifest.json"
        )),
    ),
    (
        "n64-mixed-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-mixed-manifest.json"
        )),
    ),
    (
        "n64-byte-swapped-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-byte-swapped-manifest.json"
        )),
    ),
    (
        "n64-little-endian-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-little-endian-manifest.json"
        )),
    ),
];

/// The prefix of the stagings the tests make in the folder they extract in.
pub(crate) const STAGING_PREFIX: &str = "stage-n64-";

/// The root the tests' trees lie under in their stagings.
const TREE_ROOT: &str = "n64";

/// The blocks the sweeps write in: so short that every file of a small tree spans several, and
/// every block is a step a fault can come at.
const SWEEP_BLOCK_LEN: usize = 16;

/// The shipped settings, but for blocks of [`SWEEP_BLOCK_LEN`] bytes.
pub(crate) const SWEEP: ExtractSettings = ExtractSettings {
    block_len: SWEEP_BLOCK_LEN,
    ..ExtractSettings::SHIPPED
};

/// The folder in the tests' extraction folder that is there before any staging and must stay as
/// it was, with a file in it.
const KEPT_FOLDER: &str = "kept";

/// A file of a tree as the vectors list one: its path, with `/` between its components, its size
/// and the SHA-256 digest of its bytes in hexadecimal.
pub(crate) type Listed = (String, u64, String);

/// The N64 cases, read.
pub(crate) fn n64_vectors() -> Value {
    serde_json::from_str(N64_VECTORS).expect("the N64 vectors are JSON")
}

/// The golden manifest the N64 vectors name `name`, as the extractor wrote it.
pub(crate) fn n64_golden(name: &Value) -> &'static [u8] {
    let name = text(name);
    let (_, golden) = N64_GOLDENS
        .iter()
        .find(|(golden, _)| *golden == name)
        .unwrap_or_else(|| panic!("no golden named {name}"));
    golden
}

/// The image the N64 vectors freeze under `name`.
pub(crate) fn frozen(vectors: &Value, name: &str) -> Vec<u8> {
    unhex(&vectors["images"][name])
}

/// A string of the vectors.
pub(crate) fn text(value: &Value) -> &str {
    value.as_str().expect("a string")
}

/// A tree as the vectors list it: each file's path, size and digest.
pub(crate) fn listed(rows: &Value) -> Vec<Listed> {
    let rows = rows.as_array().expect("a tree lists its files");
    rows.iter()
        .map(|row| {
            let size = row[1].as_u64().expect("a size");
            (text(&row[0]).to_owned(), size, text(&row[2]).to_owned())
        })
        .collect()
}

/// Every file of the tree at `root`, as the vectors list a tree: sorted by the path's text, which
/// orders as its UTF-8 bytes do. Panics on what is neither a folder nor a regular file, and on a
/// folder that holds nothing, since an extraction makes no folder but those above its files.
pub(crate) fn tree_listing(root: &Path) -> Vec<Listed> {
    let mut files = Vec::new();
    let mut folders = vec![(root.to_path_buf(), String::new())];
    while let Some((folder, above)) = folders.pop() {
        let mut empty = true;
        for entry in fs::read_dir(&folder).unwrap() {
            empty = false;
            let entry = entry.unwrap();
            let name = entry.file_name().into_string().unwrap();
            let path = format!("{above}{name}");
            let kind = entry.file_type().unwrap();
            if kind.is_dir() {
                folders.push((entry.path(), format!("{path}/")));
            } else {
                assert!(kind.is_file(), "{path} is no file");
                let bytes = fs::read(entry.path()).unwrap();
                files.push((path, bytes.len() as u64, sha256_hex(&bytes)));
            }
        }
        assert!(!empty, "the folder {above} holds nothing");
    }
    files.sort();
    files
}

/// The root the tests' trees lie under in their stagings.
pub(crate) fn root() -> RelPath {
    RelPath::parse(TREE_ROOT, &DISC_NAMES).unwrap()
}

/// Extracts the N64 dump at `dump` into a new staging in `parent` as the session does: the dump
/// read and checked first, the staging made only once the check has passed, then the tree written
/// under [`root`]. The staging comes back with the tree in it; on any failure it is gone.
pub(crate) fn stage_n64(
    dump: &Path,
    parent: &Path,
    settings: &ExtractSettings,
    job: &Job<'_>,
) -> Result<(Staging, N64Extraction), ExtractError> {
    let rom = N64Rom::open(dump, job)?;
    let checked = check_n64_with(&rom, settings, job)?;
    let staging = Staging::create_in(parent, STAGING_PREFIX)?;
    let extraction = extract_n64_with(&checked, &staging, &root(), settings, job)?;
    Ok((staging, extraction))
}

/// The folders a test extracts in, apart: one the stagings are made in, which holds a folder and a
/// file of its own before any staging, and one the dumps lie in.
pub(crate) struct Folders {
    /// The folder they lie in, removed with them.
    _dir: TempDir,
    /// The folder the stagings are made in.
    pub(crate) parent: PathBuf,
    /// The folder of the parent's own, with a file in it, that nothing a test makes may reach.
    pub(crate) kept: PathBuf,
    /// The folder the dumps lie in.
    dumps: PathBuf,
}

impl Folders {
    pub(crate) fn new() -> Folders {
        let dir = tempfile::tempdir().unwrap();
        let parent = dir.path().join("extracted");
        let kept = parent.join(KEPT_FOLDER);
        fs::create_dir_all(&kept).unwrap();
        fs::write(kept.join("kept.txt"), b"kept").unwrap();
        fs::write(parent.join("beside.txt"), b"beside").unwrap();
        let dumps = dir.path().join("dumps");
        fs::create_dir(&dumps).unwrap();
        Folders {
            _dir: dir,
            parent,
            kept,
            dumps,
        }
    }

    /// Writes `bytes` as the dump named `name`, and gives its path.
    pub(crate) fn dump(&self, name: &OsStr, bytes: &[u8]) -> PathBuf {
        let path = self.dumps.join(name);
        fs::write(&path, bytes).unwrap();
        path
    }

    /// What a test compares after a fault, taken once every time of change is long before it, so
    /// that an entry made and removed again shows as well as one changed.
    pub(crate) fn before(&self) -> Before {
        backdate(&self.parent).unwrap();
        backdate(&self.dumps).unwrap();
        Before {
            outside: snapshot_outside(&self.parent, STAGING_PREFIX).unwrap(),
            whole: snapshot(&self.parent).unwrap(),
            dumps: snapshot(&self.dumps).unwrap(),
        }
    }

    /// Checks that what lies in the parent outside its stagings is as it was before the fault,
    /// whether or not a staging is still there.
    #[track_caller]
    pub(crate) fn assert_outside_unchanged(&self, before: &Before) {
        let outside = snapshot_outside(&self.parent, STAGING_PREFIX).unwrap();
        assert!(
            outside == before.outside,
            "the parent changed outside its stagings"
        );
    }

    /// Checks, once a staging is dropped, that it is gone and that nothing else changed: neither
    /// what lies outside it in the parent nor the dump folder. A staging that a scanner held a file
    /// of as it was dropped is removed once the hold ends, as its drop ignores a failed removal;
    /// only Windows' scanners hold files so, so a staging left on another system fails the check.
    #[track_caller]
    pub(crate) fn assert_staging_gone(&self, before: &Before) {
        for entry in fs::read_dir(&self.parent).unwrap() {
            let entry = entry.unwrap();
            let name = entry.file_name();
            if !name
                .as_encoded_bytes()
                .starts_with(STAGING_PREFIX.as_bytes())
            {
                continue;
            }
            let left = entry.path();
            if cfg!(windows) {
                if let Err(error) = retry_locked(|| fs::remove_dir_all(&left)) {
                    panic!("a staging was left: {}: {error}", left.display());
                }
            } else {
                panic!("a staging was left: {}", left.display());
            }
        }
        self.assert_outside_unchanged(before);
        assert!(
            snapshot(&self.dumps).unwrap() == before.dumps,
            "the dump folder changed"
        );
    }

    /// Checks, after a refusal that came before any staging, that the parent is as it was, its own
    /// entry included, since nothing was made in it, and so is the dump folder.
    #[track_caller]
    pub(crate) fn assert_untouched(&self, before: &Before) {
        assert!(
            snapshot(&self.parent).unwrap() == before.whole,
            "the parent changed"
        );
        assert!(
            snapshot(&self.dumps).unwrap() == before.dumps,
            "the dump folder changed"
        );
    }
}

/// What lay in a test's folders before a fault.
pub(crate) struct Before {
    /// The parent's entries outside its stagings.
    outside: BTreeMap<PathBuf, EntryState>,
    /// The parent whole, its own entry included.
    whole: BTreeMap<PathBuf, EntryState>,
    /// The dump folder whole.
    dumps: BTreeMap<PathBuf, EntryState>,
}

/// What a job's progress was told.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Told {
    /// A phase begun, by its name.
    Phase(&'static str),
    /// How far the phase had come: what was done of its total, and the path it belonged to.
    Advance {
        done: u64,
        total: u64,
        item: Option<String>,
    },
}

/// Progress that writes down everything it is told, in order.
#[derive(Default)]
pub(crate) struct Recorded {
    /// What it was told.
    told: Mutex<Vec<Told>>,
}

impl Recorded {
    /// What it has been told so far.
    pub(crate) fn told(&self) -> Vec<Told> {
        self.told.lock().unwrap().clone()
    }
}

impl Progress for Recorded {
    fn phase(&self, phase: PhaseId) {
        self.told.lock().unwrap().push(Told::Phase(phase.0));
    }

    fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
        let item = item.map(|path| path.as_str().to_owned());
        let told = Told::Advance { done, total, item };
        self.told.lock().unwrap().push(told);
    }
}

/// Progress that asks its job to stop once it has been told `at` advances, counted from one, or
/// before the job begins when `at` is nought.
pub(crate) struct CancelAt<'a> {
    /// The job's request to stop.
    cancel: &'a Cancel,
    /// The advance it asks at.
    at: usize,
    /// How many advances it has been told.
    advances: AtomicUsize,
}

impl<'a> CancelAt<'a> {
    pub(crate) fn new(cancel: &'a Cancel, at: usize) -> CancelAt<'a> {
        if at == 0 {
            cancel.cancel();
        }
        CancelAt {
            cancel,
            at,
            advances: AtomicUsize::new(0),
        }
    }
}

impl Progress for CancelAt<'_> {
    fn phase(&self, _: PhaseId) {}

    fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {
        if self.advances.fetch_add(1, Ordering::SeqCst) + 1 == self.at {
            self.cancel.cancel();
        }
    }
}

/// Progress that panics once it is told its `at`-th step, a phase or an advance, counted from one.
/// The panic carries [`Injected`] and skips the panic hook, so that a sweep of them prints nothing.
pub(crate) struct PanicAt {
    /// The step it panics at.
    at: usize,
    /// How many steps it has been told.
    steps: AtomicUsize,
}

impl PanicAt {
    pub(crate) fn new(at: usize) -> PanicAt {
        PanicAt {
            at,
            steps: AtomicUsize::new(0),
        }
    }

    /// Counts a step, and panics at the chosen one.
    fn step(&self) {
        if self.steps.fetch_add(1, Ordering::SeqCst) + 1 == self.at {
            panic::resume_unwind(Box::new(Injected));
        }
    }
}

impl Progress for PanicAt {
    fn phase(&self, _: PhaseId) {
        self.step();
    }

    fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {
        self.step();
    }
}

/// What a panic of [`PanicAt`]'s carries, which tells it from any other.
struct Injected;

/// Runs `work`, which a [`PanicAt`] makes panic, and checks that it panicked with [`Injected`]. A
/// panic of any other kind goes on as it was.
#[track_caller]
pub(crate) fn assert_injected_panic<T>(work: impl FnOnce() -> T) {
    match panic::catch_unwind(AssertUnwindSafe(work)) {
        Ok(_) => panic!("the work ran to its end without the injected panic"),
        Err(payload) if payload.is::<Injected>() => {}
        Err(payload) => panic::resume_unwind(payload),
    }
}
