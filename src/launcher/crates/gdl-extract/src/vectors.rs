//! What the extraction's tests share: the shared vectors and the manifests the extractors wrote
//! for their dumps, the folders a test extracts in and the one way it compares what lies outside a
//! staging before and after a fault, each extraction run as the session runs it, the trust an
//! installation's extraction of the vectors' disc is held to, and progress that writes down what a
//! job tells it, stops the job partway or changes the dump under it.

use std::array;
use std::cell::Cell;
use std::collections::BTreeMap;
use std::ffi::OsStr;
use std::fs::{self, OpenOptions};
use std::io::{Seek, SeekFrom, Write};
use std::panic::{self, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use gdl_files::{
    Cancel, DISC_NAMES, Job, NoProgress, PhaseId, Progress, RelPath, Staging, retry_locked,
};
use gdl_fixtures::{
    DiscSet, Edc, EntryState, OverlaySpec, backdate, default_rom_files, rom_with_overlays,
    snapshot, snapshot_outside, unhex,
};
pub(crate) use gdl_fixtures::{Listed, listed, tree_listing};
use gdl_media::{N64Rom, OverlayTable, Sha256Digest, SourceName, US_DISC, US_OVERLAYS, US_SHA1};
use serde_json::Value;
use tempfile::TempDir;

use crate::dreamcast::extract_dreamcast_with;
use crate::n64::{check_n64_with, extract_n64_with};
use crate::{
    DISC_FOLDER, DreamcastExtraction, ExtractError, ExtractSettings, Fingerprints, IP_BIN,
    N64Extraction, READ_CARTRIDGE, READ_DISC, Strictness, Trust, UNPACK, check_dreamcast,
};

/// The N64 cases.
const N64_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../../tests/scripts/data/legends/n64-vectors.json"
));

/// The Dreamcast cases.
const DREAMCAST_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../../tests/scripts/data/legends/dreamcast-vectors.json"
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

/// The manifests the Dreamcast extractor wrote for the vectors' sets and for the native files of
/// the table its unpack test reads, byte for byte, by the names the vectors give them.
const DREAMCAST_GOLDENS: [(&str, &[u8]); 4] = [
    (
        "dreamcast-small-disc-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-small-disc-manifest.json"
        )),
    ),
    (
        "dreamcast-full-disc-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-full-disc-manifest.json"
        )),
    ),
    (
        "dreamcast-full-native-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-full-native-manifest.json"
        )),
    ),
    (
        "dreamcast-unpack-native-manifest.json",
        include_bytes!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-unpack-native-manifest.json"
        )),
    ),
];

/// What the fingerprint writer wrote for the tree the vectors' full set extracts to.
pub(crate) const FULL_FINGERPRINTS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../../tests/scripts/data/legends/dreamcast-full-fingerprints.json"
));

/// The prefix of the stagings the tests make in the folder they extract in.
pub(crate) const STAGING_PREFIX: &str = "stage-";

/// The name the refusal tests give the N64 dumps they write.
const REFUSED_DUMP: &str = "refused.z64";

/// The name the ROMs with a boot table of the tests' own are given.
pub(crate) const OVERLAYS_DUMP: &str = "overlays.z64";

/// The root the N64 tests' trees lie under in their stagings.
const N64_ROOT: &str = "n64";

/// The root the Dreamcast tests' trees lie under in their stagings.
const DREAMCAST_ROOT: &str = "dreamcast";

/// The blocks the N64 sweeps write in: so short that every file of a small tree spans several, and
/// every block is a step a fault can come at.
const N64_SWEEP_BLOCK_LEN: usize = 16;

/// The shipped settings, but for blocks of [`N64_SWEEP_BLOCK_LEN`] bytes.
pub(crate) const N64_SWEEP: ExtractSettings = ExtractSettings {
    block_len: N64_SWEEP_BLOCK_LEN,
    ..ExtractSettings::SHIPPED
};

/// The blocks the Dreamcast sweeps write in: short enough that the boot area alone spans dozens,
/// and long enough that a sweep over a whole tree stays a few dozen runs.
const DISC_SWEEP_BLOCK_LEN: usize = 1 << 10;

/// The shipped settings, but for blocks of [`DISC_SWEEP_BLOCK_LEN`] bytes.
pub(crate) const DISC_SWEEP: ExtractSettings = ExtractSettings {
    block_len: DISC_SWEEP_BLOCK_LEN,
    ..ExtractSettings::SHIPPED
};

/// The folder in the tests' extraction folder that is there before any staging and must stay as
/// it was, with a file in it.
const KEPT_FOLDER: &str = "kept";

/// The N64 cases, read.
pub(crate) fn n64_vectors() -> Value {
    serde_json::from_str(N64_VECTORS).expect("the N64 vectors are JSON")
}

/// The Dreamcast cases, read.
pub(crate) fn dreamcast_vectors() -> Value {
    serde_json::from_str(DREAMCAST_VECTORS).expect("the Dreamcast vectors are JSON")
}

/// The golden manifest the N64 vectors name `name`, as the extractor wrote it.
pub(crate) fn n64_golden(name: &Value) -> &'static [u8] {
    golden(&N64_GOLDENS, name)
}

/// The golden manifest the Dreamcast vectors name `name`, as the extractor wrote it.
pub(crate) fn dreamcast_golden(name: &Value) -> &'static [u8] {
    golden(&DREAMCAST_GOLDENS, name)
}

/// The manifest among `goldens` that the vectors name `name`.
fn golden(goldens: &[(&str, &'static [u8])], name: &Value) -> &'static [u8] {
    let name = text(name);
    let (_, golden) = goldens
        .iter()
        .find(|(golden, _)| *golden == name)
        .unwrap_or_else(|| panic!("no golden named {name}"));
    golden
}

/// The image the N64 vectors freeze under `name`.
pub(crate) fn frozen(vectors: &Value, name: &str) -> Vec<u8> {
    unhex(&vectors["images"][name])
}

/// The N64 vectors' case that extracts the frozen image `image`: the dump's name and its golden
/// manifest's.
pub(crate) fn extraction_case<'a>(vectors: &'a Value, image: &str) -> &'a Value {
    let cases = array(&vectors["extractions"]);
    cases
        .iter()
        .find(|case| case["image"] == image)
        .unwrap_or_else(|| panic!("no extraction of {image}"))
}

/// Four overlays for a boot table, named `names` in order: of different lengths, loaded at
/// different addresses, with zeroed memory after them or none and every amount of padding a
/// reader takes.
pub(crate) fn overlay_specs(names: [&'static str; 4]) -> [OverlaySpec; 4] {
    let lens = [300, 1, 2049, 64];
    let load_addresses = [0x8000_0400, 0x8010_0000, 0xE000_0400, 0x8030_0000];
    let bss_sizes = [0x40, 0, 0x1000, 4];
    let paddings = [0, 3, 15, 8];
    array::from_fn(|index| OverlaySpec {
        name: names[index],
        data: (0..lens[index])
            .map(|at: usize| u8::try_from((at * 37 + index) % 253).unwrap())
            .collect(),
        load_address: load_addresses[index],
        bss_size: bss_sizes[index],
        padding: paddings[index],
    })
}

/// A ROM of the default image's files with `specs` in its boot table, and the settings that
/// read that table, as the US ROM's is read, for this ROM.
pub(crate) fn rom_with_table(specs: &[OverlaySpec; 4]) -> (N64Rom, ExtractSettings) {
    let built = rom_with_overlays(&default_rom_files(), specs.clone());
    let name = SourceName::from_units(OVERLAYS_DUMP.encode_utf16().collect());
    let rom = N64Rom::from_bytes(name, built.bytes).unwrap();
    let table = OverlayTable {
        canonical_sha1: rom.canonical_sha1(),
        ..US_OVERLAYS
    };
    let settings = ExtractSettings {
        overlays: table,
        ..ExtractSettings::SHIPPED
    };
    (rom, settings)
}

/// The full set's disc manifest as the extractor wrote it, read: where its files and data tracks
/// lie.
pub(crate) fn full_golden(vectors: &Value) -> Value {
    let golden = dreamcast_golden(&vectors["sets"]["full"]["goldens"]["disc-manifest.json"]);
    serde_json::from_slice(golden).expect("a golden manifest is JSON")
}

/// The LBA the full set's golden records the disc file at `path` at, its path in the disc's tree.
pub(crate) fn disc_file_lba(golden: &Value, path: &str) -> u64 {
    let rows = array(&golden["files"]);
    let row = rows.iter().find(|row| row["path"] == path);
    row.and_then(|row| row["lba"].as_u64())
        .unwrap_or_else(|| panic!("the full set holds no {path}"))
}

/// The file of the full set's that holds the raw sector at `lba`, and where in it the sector
/// starts, as the full set's golden records its data tracks.
pub(crate) fn sector_place(golden: &Value, lba: u64) -> (String, u64) {
    for track in array(&golden["data_tracks"]) {
        let first = track["first_lba"].as_u64().unwrap();
        let sectors = track["sectors"].as_u64().unwrap();
        if (first..first + sectors).contains(&lba) {
            let sector = track["index"].as_u64().unwrap() + lba - first;
            let place = sector * gdl_fixtures::RAW_SECTOR as u64;
            return (text(&track["file"]).to_owned(), place);
        }
    }
    panic!("no data track of the full set holds LBA {lba}")
}

/// The vectors' full set as a pressed disc's dump holds it, for an installation's extraction:
/// every sector keeping its error detection code, and a file for its audio track.
pub(crate) fn installable_disc() -> DiscSet {
    DiscSet::full(Edc::Computed).with_audio_files()
}

/// What the fingerprint writer takes from a tree as `listing` lists it: the path and digest of
/// IP.BIN and of every file under `disc`, never a native file's or a manifest's.
pub(crate) fn fingerprint_files(listing: &[Listed]) -> Vec<(String, Sha256Digest)> {
    let disc_folder = format!("{DISC_FOLDER}/");
    let taken = listing
        .iter()
        .filter(|(path, _, _)| path == IP_BIN || path.starts_with(&disc_folder));
    taken
        .map(|(path, _, digest)| {
            let digest = Sha256Digest::from_hex(digest).expect("a listed digest");
            (path.clone(), digest)
        })
        .collect()
}

/// The fingerprints of a tree as `listing` lists it, as the fingerprint writer takes them.
pub(crate) fn fingerprints_of(listing: &[Listed]) -> Fingerprints {
    Fingerprints::from_files(fingerprint_files(listing)).expect("a tree's fingerprints")
}

/// What an installation's trust holds for the vectors' full set: the US disc's identity, which its
/// boot area gives, and the fingerprints of the tree the set extracts to. Its cartridge is the US
/// one, which no ROM of the tests is.
pub(crate) fn disc_trust() -> Trust {
    let tree = listed(&dreamcast_vectors()["sets"]["full"]["tree"]);
    Trust {
        n64_canonical_sha1: US_SHA1,
        disc: US_DISC,
        fingerprints: Some(fingerprints_of(&tree)),
    }
}

/// A string of the vectors.
pub(crate) fn text(value: &Value) -> &str {
    value.as_str().expect("a string")
}

/// The members of an array of the vectors.
pub(crate) fn array(value: &Value) -> &[Value] {
    value.as_array().expect("an array")
}

/// A job nobody follows, asked to stop only through `cancel`.
pub(crate) fn quiet(cancel: &Cancel) -> Job<'_> {
    Job {
        cancel,
        progress: &NoProgress,
    }
}

/// How many bytes the regular files under `root` hold together, of a tree partly written: none
/// when nothing of it was made.
pub(crate) fn bytes_under(root: &Path) -> u64 {
    if !root.exists() {
        return 0;
    }
    let mut total = 0;
    let mut folders = vec![root.to_path_buf()];
    while let Some(folder) = folders.pop() {
        for entry in fs::read_dir(folder).unwrap() {
            let entry = entry.unwrap();
            let kind = entry.file_type().unwrap();
            if kind.is_dir() {
                folders.push(entry.path());
            } else {
                total += entry.metadata().unwrap().len();
            }
        }
    }
    total
}

/// The root the N64 tests' trees lie under in their stagings.
pub(crate) fn n64_root() -> RelPath {
    RelPath::parse(N64_ROOT, &DISC_NAMES).unwrap()
}

/// The root the Dreamcast tests' trees lie under in their stagings.
pub(crate) fn dreamcast_root() -> RelPath {
    RelPath::parse(DREAMCAST_ROOT, &DISC_NAMES).unwrap()
}

/// Extracts the N64 dump at `dump` into a new staging in `parent` as the session does: the dump
/// read and checked under `mode` first, the staging made only once the check has passed, then the
/// tree written under [`n64_root`]. The staging comes back with the tree in it; on any failure it
/// is gone.
pub(crate) fn stage_n64(
    dump: &Path,
    parent: &Path,
    mode: &Strictness<'_>,
    settings: &ExtractSettings,
    job: &Job<'_>,
) -> Result<(Staging, N64Extraction), ExtractError> {
    let rom = N64Rom::open(dump, job)?;
    let checked = check_n64_with(&rom, mode, settings, job)?;
    let staging = Staging::create_in(parent, STAGING_PREFIX)?;
    let extraction = extract_n64_with(&checked, &staging, &n64_root(), settings, job)?;
    Ok((staging, extraction))
}

/// Extracts the Dreamcast disc whose sheet is at `sheet` into a new staging in `parent` as the
/// session does: the disc read and checked under `mode` first, the staging made only once the
/// check has passed, then the tree written under [`dreamcast_root`]. The staging comes back with
/// the tree in it; on any failure it is gone.
pub(crate) fn stage_dreamcast(
    sheet: &Path,
    parent: &Path,
    mode: &Strictness<'_>,
    settings: &ExtractSettings,
    job: &Job<'_>,
) -> Result<(Staging, DreamcastExtraction), ExtractError> {
    let mut checked = check_dreamcast(sheet, mode, job)?;
    let staging = Staging::create_in(parent, STAGING_PREFIX)?;
    let root = dreamcast_root();
    let extraction = extract_dreamcast_with(&mut checked, &staging, &root, settings, job)?;
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
    /// How many discs have been written into the dump folder, each into a folder of its own.
    discs: Cell<usize>,
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
            discs: Cell::new(0),
        }
    }

    /// Writes `bytes` as the dump named `name`, and gives its path.
    pub(crate) fn dump(&self, name: &OsStr, bytes: &[u8]) -> PathBuf {
        let path = self.dumps.join(name);
        fs::write(&path, bytes).unwrap();
        path
    }

    /// Writes the files of the disc `set` into a new folder of the dump folder, so that the discs
    /// a test writes never share a file, and gives the path of its sheet.
    pub(crate) fn disc(&self, set: &DiscSet) -> PathBuf {
        let count = self.discs.get();
        self.discs.set(count + 1);
        let folder = self.dumps.join(format!("disc{count}"));
        fs::create_dir(&folder).unwrap();
        set.write(&folder).unwrap()
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

    /// What `image`, given as an N64 dump, is refused by under `mode` and `settings`, once it is
    /// shown that the refusal came before any staging: the folder the staging would have been made
    /// in is as it was, its own entry included, and so is the dump.
    #[track_caller]
    pub(crate) fn refused_rom(
        &self,
        image: &[u8],
        mode: &Strictness<'_>,
        settings: &ExtractSettings,
    ) -> ExtractError {
        let dump = self.dump(OsStr::new(REFUSED_DUMP), image);
        let before = self.before();
        let cancel = Cancel::default();
        let staged = stage_n64(&dump, &self.parent, mode, settings, &quiet(&cancel));
        let error = staged.expect_err("the dump is refused");
        self.assert_untouched(&before);
        error
    }

    /// What the disc `set` is refused by under `mode`, once it is shown that the refusal came
    /// before any staging: the folder the staging would have been made in is as it was, its own
    /// entry included, and so is the dump.
    #[track_caller]
    pub(crate) fn refused_disc(&self, set: &DiscSet, mode: &Strictness<'_>) -> ExtractError {
        let sheet = self.disc(set);
        let before = self.before();
        let cancel = Cancel::default();
        let shipped = ExtractSettings::SHIPPED;
        let staged = stage_dreamcast(&sheet, &self.parent, mode, &shipped, &quiet(&cancel));
        let error = staged.expect_err("the disc is refused");
        self.assert_untouched(&before);
        error
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

/// How many bytes a run had written into its tree before its first advance and after each, from
/// what its progress was told: each phase's writing counts from nought, after what the phases
/// before it wrote, and a check's phase, which reads the dump, writes nothing.
pub(crate) fn written_after_each_advance(told: &[Told]) -> Vec<u64> {
    let mut written = vec![0];
    let mut phase_start = 0;
    let mut writes = true;
    for told in told {
        match told {
            Told::Phase(name) => {
                phase_start = *written.last().unwrap();
                writes = ![READ_CARTRIDGE.0, READ_DISC.0].contains(name);
            }
            Told::Advance { done, .. } => {
                let wrote = if writes { *done } else { 0 };
                written.push(phase_start + wrote);
            }
        }
    }
    written
}

/// A phase of a job as its progress was told it: its name, and each advance's done, total and
/// item.
pub(crate) type Phase = (&'static str, Vec<(u64, u64, String)>);

/// What `told` tells of each phase, in order.
pub(crate) fn phases_of(told: Vec<Told>) -> Vec<Phase> {
    let mut phases: Vec<Phase> = Vec::new();
    for told in told {
        match told {
            Told::Phase(name) => phases.push((name, Vec::new())),
            Told::Advance { done, total, item } => {
                let (_, advances) = phases.last_mut().expect("an advance within a phase");
                advances.push((done, total, item.expect("an advance names its file")));
            }
        }
    }
    phases
}

/// The files a phase's `advances` tell of, in the order it told of them, each once for a run of
/// advances in a row.
pub(crate) fn items_of(advances: &[(u64, u64, String)]) -> Vec<String> {
    let mut items: Vec<String> = Vec::new();
    for (_, _, item) in advances {
        if items.last() != Some(item) {
            items.push(item.clone());
        }
    }
    items
}

/// Progress that, as the extraction begins to write, writes `bytes` over the dump's file at
/// `path` from `place`: a dump that changes between the check's read and the extraction's.
pub(crate) struct ChangedAtUnpack {
    /// The dump's file it changes.
    pub(crate) path: PathBuf,
    /// Where in the file it writes.
    pub(crate) place: u64,
    /// What it writes there.
    pub(crate) bytes: Vec<u8>,
}

impl Progress for ChangedAtUnpack {
    fn phase(&self, phase: PhaseId) {
        if phase == UNPACK {
            let mut file = OpenOptions::new().write(true).open(&self.path).unwrap();
            file.seek(SeekFrom::Start(self.place)).unwrap();
            file.write_all(&self.bytes).unwrap();
        }
    }

    fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
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
