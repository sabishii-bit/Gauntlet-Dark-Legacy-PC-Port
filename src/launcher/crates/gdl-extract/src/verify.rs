//! The check of an extracted tree against its own manifests, and of a Dreamcast tree against the
//! fingerprints of a trusted disc's files too, which writes nothing: how a tree written already is
//! checked again, since an extraction never writes into a tree that is there. Every file the
//! manifests list, and a Dreamcast tree's IP.BIN, must be in the tree at exactly its path, letter
//! case included on every system, a regular file of the size and with the digest they record.
//! Held to fingerprints, IP.BIN and every disc file must also match its own, and every fingerprint
//! must be of a file the manifests list; and the native files, which DISK.ROM packs, must hold
//! what the tree's DISK.ROM holds where the native manifest places them, and that manifest must
//! record the table the tree's SIZES.ROM gives. So a tree whose manifests were changed with its
//! files, or cut short, does not pass. Whatever else the tree holds is reported, and is no damage.

use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use gdl_files::{
    Cancel, Cancelled, DISC_NAMES, FilesError, Job, MAX_JSON_LEN, NameRules, ROMDISK_NAMES,
    RelPath, no_links, read_bounded,
};
use gdl_media::{BOOT_LEN, MAX_SIZES_ROM_LEN, PackedFile, PackedPlace, Sha256Digest, packed_files};
use serde::Deserialize;
use serde::de::DeserializeOwned;
use sha2::{Digest, Sha256};

use crate::dreamcast::{disc_path, native_path, own_path};
use crate::n64::manifest_path;
use crate::tree::{SEPARATOR, TreePlan};
use crate::{
    DISC_FORMAT, DISC_MANIFEST, DISK_ROM, Expansion, ExtractSettings, Fingerprints, IP_BIN,
    N64_FORMAT, N64_MANIFEST, N64_VERSION, NATIVE_FORMAT, NATIVE_MANIFEST, SIZES_ROM,
};

/// What a manifest is refused as when the tree holds nothing of its name.
const NO_MANIFEST: &str = "the tree holds no file of this name";

/// What a manifest is refused as when what the tree holds of its name is no regular file.
const NO_REGULAR_MANIFEST: &str = "what the tree holds of this name is no regular file";

/// What a manifest is refused as, before the JSON reader's own words, when it is not JSON of the
/// shape its extractor writes.
const NOT_WRITTEN_SO: &str = "the manifest is not written as its extractor writes it";

/// The key of the list of files a manifest records.
const FILES: &str = "files";

/// The key of the list of executable overlays the N64 manifest records.
const PROGRAMS: &str = "programs";

/// How the N64 manifest's files are read: by the rules of a ROMDISK's paths, at the tree's top.
const ROMDISK_ROWS: RowRules = RowRules {
    list: FILES,
    names: &ROMDISK_NAMES,
    place: RelPath::clone,
    held: held_to_nothing,
};

/// How the N64 manifest's overlays are read: by the rules of the extractor's own names, at the
/// paths under `_program` it gives them.
const PROGRAM_ROWS: RowRules = RowRules {
    list: PROGRAMS,
    names: &DISC_NAMES,
    place: RelPath::clone,
    held: held_to_nothing,
};

/// How the disc manifest's files are read: by the rules of a disc's names, under `disc`.
const DISC_ROWS: RowRules = RowRules {
    list: FILES,
    names: &DISC_NAMES,
    place: disc_path,
    held: held_to_fingerprint,
};

/// How the native manifest's files are read: by the rules of a packed file table's names, under
/// `native`.
const NATIVE_ROWS: RowRules = RowRules {
    list: FILES,
    names: &DISC_NAMES,
    place: native_path,
    held: Held::Restored,
};

/// How far a check of a tree went.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VerifyLevel {
    /// Each file the tree's manifests list was held to what they record of it.
    Manifests,
    /// The tree's files from the disc were held to a trusted disc's fingerprints as well, and its
    /// native files and their manifest to the DISK.ROM and SIZES.ROM held so.
    Fingerprints,
}

/// What is wrong with a file a tree's manifests expect, or with a fingerprint the tree was held
/// to.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DamageKind {
    /// The tree holds nothing at the file's path, in any letter case.
    Missing,
    /// What the tree holds at the file's path is no regular file: a folder, a link of any kind,
    /// which is never followed, or anything else, which is never opened.
    NotAFile,
    /// The file holds another number of bytes than it should, as its manifest records or, for
    /// IP.BIN, as a boot area holds, so its digest was not checked.
    Size {
        /// How many it should hold.
        expected: u64,
        /// How many it holds.
        found: u64,
    },
    /// The file's bytes do not have the digest its manifest records.
    Digest,
    /// The file is not what the trusted disc holds: IP.BIN or one of the disc's files has no
    /// fingerprint, or its bytes do not have the digest its fingerprint records; a native file's
    /// bytes are not those of the tree's DISK.ROM where the native manifest places it, or DISK.ROM
    /// could not be read whole; or the native manifest does not record the table the tree's
    /// SIZES.ROM gives against its DISK.ROM, or either could not be read whole.
    Fingerprint,
    /// A fingerprint is of no file the manifests list.
    Unlisted,
    /// The tree holds the file's path only in another letter case, which a system that folds
    /// letter case would open as the file.
    LetterCase {
        /// The path the tree holds, its components joined by `/`.
        found: String,
    },
}

/// A file of the tree, or a fingerprint, and what is wrong with it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Damage {
    /// The file's path in the tree, its components joined by `/`; for a fingerprint of no file,
    /// the digest it is kept under, in lowercase hexadecimal.
    pub path: String,
    /// What is wrong.
    pub kind: DamageKind,
}

/// What a check of a tree found.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VerifyReport {
    /// The expansion whose tree it is.
    pub expansion: Expansion,
    /// How far the check went.
    pub level: VerifyLevel,
    /// Each manifest, by its name at the tree's top, with the SHA-256 digest of its bytes as they
    /// were read, in the order they were read.
    pub manifests: Vec<(String, Sha256Digest)>,
    /// How many files the manifests expect, the manifests included, each of which was looked at.
    pub checked: u64,
    /// How many bytes of them were read.
    pub bytes: u64,
    /// What is wrong, file by file in the order the extraction writes them, the native manifest
    /// after them, then each fingerprint of no file, in the order of the digests they are kept
    /// under.
    pub damage: Vec<Damage>,
    /// Every entry of the tree that nothing expects and that holds no entry of its own, by its
    /// path, in the order of the paths' text: each file, link or other entry no manifest lists,
    /// and each empty folder above none of the files they list.
    pub extra: Vec<String>,
}

impl VerifyReport {
    /// Whether nothing is wrong: every file the manifests expect is there as they record it,
    /// whatever the tree holds beside.
    pub fn is_intact(&self) -> bool {
        self.damage.is_empty()
    }
}

/// Why a tree could not be checked.
#[derive(Debug, thiserror::Error)]
pub enum VerifyError {
    /// A link stands on the way to the tree: its root or a folder above it is a link, a symbolic
    /// link or on Windows a junction or a mount point, or a link took a manifest's place as it was
    /// read.
    #[error("the tree's path holds a link or a junction: {}", .path.display())]
    Links {
        /// The link.
        path: PathBuf,
    },
    /// A manifest the tree must hold is not there, is too large to be one, or is not of its
    /// extractor's format and shape, or the paths its rows give make no tree.
    #[error("{}: {detail}", .path.display())]
    Manifest {
        /// Where the manifest is.
        path: PathBuf,
        /// What is wrong, in English.
        detail: Box<str>,
    },
    /// Listing a folder of the tree, or reading a file of it, failed.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The folder or the file.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The job was asked to stop before the check was done.
    #[error("cancelled")]
    Cancelled,
}

impl From<Cancelled> for VerifyError {
    /// A check stops where its job's cancel tells it to, before a folder is listed or a block
    /// read.
    fn from(_: Cancelled) -> VerifyError {
        VerifyError::Cancelled
    }
}

/// Checks the tree of `expansion` at `root` against its manifests, and against `fingerprints` when
/// they are given, writing nothing. A link on the way to the tree, its root included, refuses it.
/// The tree's entries are listed by their exact names in one walk that follows no link, the job's
/// cancel looked at before each folder is listed. The manifests are read next, each a regular file
/// at exactly its name of at most [`MAX_JSON_LEN`] bytes, of its extractor's format, with rows
/// whose paths are names the tree may hold and make one tree on every system, as the extraction
/// holds the paths it writes; else the tree is refused as [`VerifyError::Manifest`]. Then each
/// file they list, and a Dreamcast tree's IP.BIN, is found by its exact path among the entries,
/// in the order the extraction writes them: one that is missing, there only in another letter
/// case, no regular file or of another size is damage, and one of its size is read in blocks of
/// at most [`BLOCK_LEN`](gdl_files::BLOCK_LEN) bytes, the cancel looked at before each, and its
/// digest must be the one recorded. After each block, and past each file not read, the job's
/// progress is told how many of the bytes the manifests expect have been read or passed over, in
/// the phase the caller began. With fingerprints, IP.BIN and every file under `disc` must match
/// its own as well, and every fingerprint must be of a file the manifests list. Each native file
/// must then hold the bytes of the tree's DISK.ROM where its row of the native manifest places it,
/// each part hashed as DISK.ROM is read; and the native manifest must record the table the tree's
/// SIZES.ROM, of at most [`MAX_SIZES_ROM_LEN`] bytes and kept as it is read, gives against
/// DISK.ROM, row by row in its order, with SIZES.ROM's digest. The fingerprints are of a disc's
/// files, which an N64 tree holds none of, so each one given for it is damage. Whatever else the
/// tree holds is reported as extra.
pub fn verify_tree(
    root: &Path,
    expansion: Expansion,
    fingerprints: Option<&Fingerprints>,
    job: &Job<'_>,
) -> Result<VerifyReport, VerifyError> {
    verify_tree_with(
        root,
        expansion,
        fingerprints,
        &ExtractSettings::SHIPPED,
        job,
    )
}

/// [`verify_tree`], reading in `settings`' blocks.
pub(crate) fn verify_tree_with(
    root: &Path,
    expansion: Expansion,
    fingerprints: Option<&Fingerprints>,
    settings: &ExtractSettings,
    job: &Job<'_>,
) -> Result<VerifyReport, VerifyError> {
    no_links(root).map_err(|error| read_failure(error, root))?;
    let listing = Listing::walk(root, job.cancel)?;
    let Expectations {
        manifests,
        files,
        table,
    } = match expansion {
        Expansion::N64 => n64_expected(root, &listing)?,
        Expansion::Dreamcast => dreamcast_expected(root, &listing)?,
    };
    let total = files
        .iter()
        .fold(0, |total: u64, file| total.saturating_add(file.size));
    let mut check = Check {
        root,
        listing: &listing,
        fingerprints,
        restoring: table.filter(|_| fingerprints.is_some()).map(Restoring::new),
        job: *job,
        block: vec![0; settings.block_len],
        done: 0,
        total,
        bytes: manifests.iter().map(|manifest| manifest.len).sum(),
        damage: Vec::new(),
        claimed: BTreeSet::new(),
    };
    for file in &files {
        check.file(file)?;
    }
    let level = match fingerprints {
        Some(fingerprints) => {
            check.restored_table();
            check.unlisted(fingerprints, &files);
            VerifyLevel::Fingerprints
        }
        None => VerifyLevel::Manifests,
    };
    let names = manifests.iter().map(|manifest| manifest.name);
    let paths: BTreeSet<&str> = files
        .iter()
        .map(|file| file.path.as_str())
        .chain(names)
        .collect();
    let extra = listing.extra(&paths, &check.claimed);
    Ok(VerifyReport {
        expansion,
        level,
        manifests: manifests
            .iter()
            .map(|manifest| (manifest.name.to_owned(), manifest.sha256))
            .collect(),
        checked: (files.len() + manifests.len()) as u64,
        bytes: check.bytes,
        damage: check.damage,
        extra,
    })
}

/// What the walk found at a path of the tree.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum EntryKind {
    /// A regular file.
    File,
    /// A real folder, whose entries were listed too.
    Folder,
    /// Anything else: a link of any kind, whatever it leads to, or a FIFO, a socket or a device.
    Other,
}

/// Every entry of a tree, as one walk of it found them.
struct Listing {
    /// Each entry's kind, by its path in the tree: its components joined by `/`, each its exact
    /// name, read with what is not Unicode in it replaced.
    entries: BTreeMap<String, EntryKind>,
    /// The first of the entries' paths that are one in ASCII lower case, under that form.
    folded: BTreeMap<String, String>,
}

impl Listing {
    /// Lists every entry under `root` by its exact name, looking at `cancel` before each folder is
    /// listed, and never through a link: a link is listed, and what it leads to is not.
    fn walk(root: &Path, cancel: &Cancel) -> Result<Listing, VerifyError> {
        let mut entries = BTreeMap::new();
        let mut folders = vec![(root.to_path_buf(), String::new())];
        while let Some((folder, above)) = folders.pop() {
            cancel.check()?;
            let failed = |source: io::Error| VerifyError::Io {
                path: folder.clone(),
                source,
            };
            for entry in fs::read_dir(&folder).map_err(failed)? {
                let entry = entry.map_err(failed)?;
                let path = format!("{above}{}", entry.file_name().to_string_lossy());
                let file_type = entry.file_type().map_err(|source| VerifyError::Io {
                    path: entry.path(),
                    source,
                })?;
                let kind = if file_type.is_dir() {
                    folders.push((entry.path(), format!("{path}{SEPARATOR}")));
                    EntryKind::Folder
                } else if file_type.is_file() {
                    EntryKind::File
                } else {
                    EntryKind::Other
                };
                entries.insert(path, kind);
            }
        }
        let mut folded = BTreeMap::new();
        for path in entries.keys() {
            folded
                .entry(path.to_ascii_lowercase())
                .or_insert_with(|| path.clone());
        }
        Ok(Listing { entries, folded })
    }

    /// What the tree holds at exactly `path`.
    fn kind(&self, path: &str) -> Option<EntryKind> {
        self.entries.get(path).copied()
    }

    /// The path the tree holds where `path` is only in another ASCII letter case: none when it
    /// holds `path` itself.
    fn in_other_case(&self, path: &str) -> Option<&str> {
        if self.entries.contains_key(path) {
            return None;
        }
        self.folded
            .get(&path.to_ascii_lowercase())
            .map(String::as_str)
    }

    /// Whether the tree holds an entry inside the folder at `path`.
    fn holds_inside(&self, path: &str) -> bool {
        let inside = format!("{path}{SEPARATOR}");
        let mut after = self.entries.range(inside.clone()..);
        after
            .next()
            .is_some_and(|(next, _)| next.starts_with(&inside))
    }

    /// Every entry that nothing expects and that holds no entry of its own: each that is neither
    /// one of `expected`, the files the manifests expect, nor one of `claimed`, which a damage
    /// names in place of a file it stands for, unless it is a folder above one of `expected` or
    /// above another entry.
    fn extra(&self, expected: &BTreeSet<&str>, claimed: &BTreeSet<String>) -> Vec<String> {
        let above: BTreeSet<&str> = expected
            .iter()
            .flat_map(|path| folders_above(path))
            .collect();
        let unexpected = self.entries.iter().filter(|(path, kind)| {
            let path = path.as_str();
            let accounted = expected.contains(path) || claimed.contains(path);
            let holding =
                **kind == EntryKind::Folder && (above.contains(path) || self.holds_inside(path));
            !(accounted || holding)
        });
        unexpected.map(|(path, _)| path.clone()).collect()
    }
}

/// Every folder above `path`, a path of the tree, from its top down.
fn folders_above(path: &str) -> impl Iterator<Item = &str> {
    path.match_indices(SEPARATOR).map(|(end, _)| &path[..end])
}

/// A file the tree's manifests expect, and what they hold it to.
struct Expected {
    /// Its path in the tree.
    path: RelPath,
    /// How many bytes it must hold.
    size: u64,
    /// The digest its bytes must have, when a manifest records one.
    sha256: Option<Sha256Digest>,
    /// What a trusted disc holds it to.
    held: Held,
}

/// What a trusted disc holds a file the manifests expect to, when a check is held to its
/// fingerprints.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Held {
    /// Nothing: an N64 tree's file, which no disc holds.
    Nothing,
    /// Its own fingerprint: IP.BIN, or a file of the disc under `disc`.
    Fingerprint,
    /// The part of DISK.ROM it was restored from, which the native manifest's row of this index
    /// places it in.
    Restored(usize),
}

/// What a trusted disc holds a file of a list that no disc holds to, whatever its row.
fn held_to_nothing(_: usize) -> Held {
    Held::Nothing
}

/// What a trusted disc holds a file of its own to, whatever its row.
fn held_to_fingerprint(_: usize) -> Held {
    Held::Fingerprint
}

/// A manifest of the tree as a check read it.
struct ReadManifest {
    /// Its name, at the tree's top.
    name: &'static str,
    /// The SHA-256 digest of its bytes.
    sha256: Sha256Digest,
    /// How many bytes it holds.
    len: u64,
}

/// What a tree's manifests expect of it.
struct Expectations {
    /// Each manifest, as it was read.
    manifests: Vec<ReadManifest>,
    /// Every file they list, and a Dreamcast tree's IP.BIN, in the order the extraction writes
    /// them.
    files: Vec<Expected>,
    /// What a Dreamcast tree's native manifest records of the table its native files were
    /// restored by; none for an N64 tree.
    table: Option<NativeTable>,
}

/// A file's row as a manifest records it: what a check holds the file to, and, for a native
/// file, where in DISK.ROM it was restored from. The row's other keys are passed over.
#[derive(Deserialize)]
struct WrittenRow {
    /// The file's path, under the folder the manifest keeps its files in.
    path: String,
    /// How many bytes it holds.
    size: u64,
    /// The SHA-256 digest of its bytes, in lowercase hexadecimal.
    sha256: String,
    /// Where its bytes start in DISK.ROM, which the native manifest alone records of its files.
    #[serde(default)]
    offset: Option<u64>,
}

/// What a check reads of the native manifest: its files, and the rest of what it records of the
/// table they were restored by.
#[derive(Deserialize)]
struct WrittenNative {
    /// The format it is written in.
    format: String,
    /// The files DISK.ROM holds, in the table's order.
    files: Vec<WrittenRow>,
    /// The files the table lists outside DISK.ROM, in its order.
    external: Vec<WrittenExternal>,
    /// The SHA-256 digest of SIZES.ROM, in lowercase hexadecimal.
    sizes_sha256: String,
}

/// A file the native manifest records outside DISK.ROM, which the tree does not hold.
#[derive(Deserialize)]
struct WrittenExternal {
    /// Its path, as the table gives it.
    path: String,
    /// How many bytes its row of the table says it holds.
    declared_size: u64,
}

/// What the native manifest records of the table its files were restored by.
struct NativeTable {
    /// Each file DISK.ROM holds, by its row, in the manifest's order: its path as the row gives
    /// it, where the row says its bytes start in DISK.ROM, if it says, and how many there are.
    files: Vec<(String, Option<u64>, u64)>,
    /// Each file outside DISK.ROM, in the manifest's order: its path and its declared size.
    external: Vec<(String, u64)>,
    /// SIZES.ROM's digest, as the manifest records it, in lowercase hexadecimal.
    sizes_sha256: String,
}

impl NativeTable {
    /// What `native` records of the table.
    fn of(native: &WrittenNative) -> NativeTable {
        NativeTable {
            files: native
                .files
                .iter()
                .map(|row| (row.path.clone(), row.offset, row.size))
                .collect(),
            external: native
                .external
                .iter()
                .map(|row| (row.path.clone(), row.declared_size))
                .collect(),
            sizes_sha256: native.sizes_sha256.clone(),
        }
    }

    /// Whether it is the table `packed` lists, of a SIZES.ROM whose digest is `sizes`: the same
    /// files in DISK.ROM, in the same order, each at the same place and of the same size; the same
    /// files outside it, each of the same declared size; and SIZES.ROM's digest.
    fn is(&self, packed: &[PackedFile], sizes: Sha256Digest) -> bool {
        let mut held = Vec::new();
        let mut outside = Vec::new();
        for file in packed {
            let (path, size) = (file.path.as_str(), u64::from(file.size));
            match file.place {
                PackedPlace::Disk { offset } => held.push((path, Some(offset), size)),
                PackedPlace::External => outside.push((path, size)),
            }
        }
        let files = self.files.iter();
        let external = self.external.iter();
        Sha256Digest::from_hex(&self.sizes_sha256) == Some(sizes)
            && files
                .map(|(path, offset, size)| (path.as_str(), *offset, *size))
                .eq(held)
            && external
                .map(|(path, size)| (path.as_str(), *size))
                .eq(outside)
    }
}

/// A Dreamcast tree's native files held to the tree's DISK.ROM and SIZES.ROM, whose own
/// fingerprints hold them to the trusted disc's: each native file must hold what DISK.ROM holds
/// where its row of the native manifest places it, and the native manifest must record the table
/// SIZES.ROM gives against DISK.ROM. DISK.ROM's parts are hashed and SIZES.ROM's bytes kept as
/// each is read, so neither is read again.
struct Restoring {
    /// Where the tree holds DISK.ROM.
    disk_rom: RelPath,
    /// Where the tree holds SIZES.ROM.
    sizes_rom: RelPath,
    /// What the native manifest records of the table.
    table: NativeTable,
    /// Each native file's part of DISK.ROM, by its row, once DISK.ROM was read whole: the digest
    /// of its bytes, or none for a part its row gives no start or that does not lie within
    /// DISK.ROM.
    parts: Option<Vec<Option<Sha256Digest>>>,
    /// How many bytes DISK.ROM holds, once it was read whole.
    disk_len: Option<u64>,
    /// SIZES.ROM's bytes, once it was read whole.
    sizes: Option<Vec<u8>>,
}

impl Restoring {
    /// The native files held to the DISK.ROM and SIZES.ROM of the tree, whose native manifest
    /// records `table`, neither read yet.
    fn new(table: NativeTable) -> Restoring {
        let at_disc_top = |name| disc_path(&own_path(name));
        Restoring {
            disk_rom: at_disc_top(DISK_ROM),
            sizes_rom: at_disc_top(SIZES_ROM),
            table,
            parts: None,
            disk_len: None,
            sizes: None,
        }
    }

    /// Whether `digest`, a native file's whose row of the native manifest is the `row`th, is the
    /// digest of DISK.ROM's part that row places it in: never when DISK.ROM was not read whole.
    fn holds(&self, row: usize, digest: Sha256Digest) -> bool {
        let part = self.parts.as_ref().and_then(|parts| parts.get(row));
        part.copied().flatten() == Some(digest)
    }

    /// Whether the native manifest records the table SIZES.ROM gives against DISK.ROM, both as
    /// the tree holds them: never when either was not read whole, or SIZES.ROM gives no table.
    fn table_follows(&self) -> bool {
        let (Some(sizes), Some(disk_len)) = (&self.sizes, self.disk_len) else {
            return false;
        };
        packed_files(sizes, disk_len)
            .is_ok_and(|packed| self.table.is(&packed, Sha256Digest::of(sizes)))
    }
}

/// The parts of DISK.ROM the native manifest's rows place the native files in, each hashed as
/// DISK.ROM is read.
struct Parts {
    /// Each part's start and end in DISK.ROM, by its row, or none for a row that gives no start.
    spans: Vec<Option<(u64, u64)>>,
    /// What each part's digest has taken of DISK.ROM's bytes so far.
    digests: Vec<Sha256>,
}

impl Parts {
    /// The parts `table`'s rows place their files in, none of DISK.ROM read yet.
    fn of(table: &NativeTable) -> Parts {
        let spans: Vec<Option<(u64, u64)>> = table
            .files
            .iter()
            .map(|(_, offset, size)| offset.map(|start| (start, start.saturating_add(*size))))
            .collect();
        let digests = spans.iter().map(|_| Sha256::new()).collect();
        Parts { spans, digests }
    }

    /// Takes `block`, DISK.ROM's bytes from `at`, into each part it overlaps.
    fn take(&mut self, at: u64, block: &[u8]) {
        let end = at.saturating_add(block.len() as u64);
        for (span, digest) in self.spans.iter().zip(&mut self.digests) {
            let Some((start, stop)) = *span else {
                continue;
            };
            let (from, to) = (start.max(at), stop.min(end));
            if from < to {
                digest.update(&block[(from - at) as usize..(to - at) as usize]);
            }
        }
    }

    /// Each part's digest, DISK.ROM read whole and `disk_len` bytes long: none for a part that
    /// does not lie within it, or whose row gives no start.
    fn digests(self, disk_len: u64) -> Vec<Option<Sha256Digest>> {
        let parts = self.spans.into_iter().zip(self.digests);
        parts
            .map(|(span, digest)| {
                let within = span.is_some_and(|(_, stop)| stop <= disk_len);
                within.then(|| Sha256Digest(digest.finalize().into()))
            })
            .collect()
    }
}

/// What a check reads of the N64 manifest.
#[derive(Deserialize)]
struct WrittenN64 {
    /// The format it is written in.
    format: String,
    /// The version of that format.
    version: u64,
    /// The ROMDISK's files.
    files: Vec<WrittenRow>,
    /// The executable overlays.
    programs: Vec<WrittenRow>,
}

/// What a check reads of a Dreamcast manifest.
#[derive(Deserialize)]
struct WrittenFiles {
    /// The format it is written in.
    format: String,
    /// The files it records.
    files: Vec<WrittenRow>,
}

/// How a list of a manifest's rows is read.
struct RowRules {
    /// The list's key.
    list: &'static str,
    /// The rules its paths follow.
    names: &'static NameRules,
    /// Where a file of the list lies in the tree, given its path in the list.
    place: fn(&RelPath) -> RelPath,
    /// What a trusted disc holds a file of the list to, given its row's index.
    held: fn(usize) -> Held,
}

/// A manifest the tree must hold at its top.
struct ManifestFile {
    /// Its name.
    name: &'static str,
    /// Where it is.
    path: PathBuf,
}

impl ManifestFile {
    /// The manifest named `name` of the tree at `root`.
    fn at(root: &Path, name: &'static str) -> ManifestFile {
        ManifestFile {
            name,
            path: root.join(name),
        }
    }

    /// The manifest refused for what `detail` says.
    fn refused(&self, detail: impl Into<Box<str>>) -> VerifyError {
        VerifyError::Manifest {
            path: self.path.clone(),
            detail: detail.into(),
        }
    }

    /// Reads the manifest, as JSON of `T`'s shape, when `listing` holds a regular file at exactly
    /// its name and it holds at most [`MAX_JSON_LEN`] bytes, and gives it with what was read.
    fn read<T: DeserializeOwned>(
        &self,
        listing: &Listing,
    ) -> Result<(T, ReadManifest), VerifyError> {
        match listing.kind(self.name) {
            Some(EntryKind::File) => {}
            Some(EntryKind::Folder | EntryKind::Other) => {
                return Err(self.refused(NO_REGULAR_MANIFEST));
            }
            None => {
                let detail = match listing.in_other_case(self.name) {
                    Some(found) => format!("the tree holds it only as {found}"),
                    None => NO_MANIFEST.to_owned(),
                };
                return Err(self.refused(detail));
            }
        }
        let bytes = read_bounded(&self.path, MAX_JSON_LEN).map_err(|error| match error {
            FilesError::TooLarge { .. } => {
                self.refused(format!("the manifest holds more than {MAX_JSON_LEN} bytes"))
            }
            error => read_failure(error, &self.path),
        })?;
        let written = serde_json::from_slice(&bytes)
            .map_err(|error| self.refused(format!("{NOT_WRITTEN_SO}: {error}")))?;
        let read = ReadManifest {
            name: self.name,
            sha256: Sha256Digest::of(&bytes),
            len: bytes.len() as u64,
        };
        Ok((written, read))
    }

    /// Refuses the manifest unless `written`, the format it names, is `format`.
    fn of_format(&self, written: &str, format: &str) -> Result<(), VerifyError> {
        if written == format {
            Ok(())
        } else {
            Err(self.refused(format!("the manifest is not of the format {format}")))
        }
    }

    /// The files the manifest's `rows`, read by `rules`, expect, each planned in `plan`: a row
    /// whose path is no name of its kind, or whose digest is not written as the extractors write
    /// digests, refuses the manifest, and so does a path the plan refuses beside those planned
    /// before it.
    fn expected(
        &self,
        rows: &[WrittenRow],
        rules: &RowRules,
        plan: &mut TreePlan,
    ) -> Result<Vec<Expected>, VerifyError> {
        let mut files = Vec::with_capacity(rows.len());
        for (index, row) in rows.iter().enumerate() {
            let refused =
                |what: &str| self.refused(format!("{}[{index}] holds {what}", rules.list));
            let path = RelPath::parse(&row.path, rules.names).map_err(|error| {
                refused(&format!("a path the tree may not hold ({})", error.rule))
            })?;
            let sha256 = Sha256Digest::from_hex(&row.sha256)
                .ok_or_else(|| refused("a digest that is not 64 lowercase hexadecimal digits"))?;
            let path = (rules.place)(&path);
            plan.add(&path)
                .map_err(|error| self.refused(error.to_string()))?;
            files.push(Expected {
                path,
                size: row.size,
                sha256: Some(sha256),
                held: (rules.held)(index),
            });
        }
        Ok(files)
    }

    /// Plans `own`, the extractor's own paths beside the manifest's rows, in `plan`, and refuses
    /// the manifest unless what it holds then makes one tree on every system, as the extraction
    /// holds the paths it writes to.
    fn plan(&self, plan: &mut TreePlan, own: &[RelPath]) -> Result<(), VerifyError> {
        for path in own {
            plan.add(path)
                .map_err(|error| self.refused(error.to_string()))?;
        }
        plan.finish()
            .map_err(|error| self.refused(error.to_string()))
    }
}

/// What the N64 tree at `root`, whose walk is `listing`, is expected to hold: every file and
/// overlay `manifest.json` lists, and the manifest, which must be of its version of the N64
/// extractor's format.
fn n64_expected(root: &Path, listing: &Listing) -> Result<Expectations, VerifyError> {
    let manifest = ManifestFile::at(root, N64_MANIFEST);
    let (written, read): (WrittenN64, _) = manifest.read(listing)?;
    if written.format != N64_FORMAT || written.version != N64_VERSION {
        let detail =
            format!("the manifest is not version {N64_VERSION} of the format {N64_FORMAT}");
        return Err(manifest.refused(detail));
    }
    let mut plan = TreePlan::new();
    let mut files = manifest.expected(&written.files, &ROMDISK_ROWS, &mut plan)?;
    files.extend(manifest.expected(&written.programs, &PROGRAM_ROWS, &mut plan)?);
    manifest.plan(&mut plan, &[manifest_path()])?;
    Ok(Expectations {
        manifests: vec![read],
        files,
        table: None,
    })
}

/// What the Dreamcast tree at `root`, whose walk is `listing`, is expected to hold: every file
/// `disc-manifest.json` lists, under `disc`; IP.BIN, of a boot area's length; every file
/// `native-manifest.json` lists, under `native`; and both manifests, each of its format of the
/// Dreamcast extractor's, the native manifest recording the table its files were restored by.
fn dreamcast_expected(root: &Path, listing: &Listing) -> Result<Expectations, VerifyError> {
    let disc_manifest = ManifestFile::at(root, DISC_MANIFEST);
    let (disc, disc_read): (WrittenFiles, _) = disc_manifest.read(listing)?;
    disc_manifest.of_format(&disc.format, DISC_FORMAT)?;
    let native_manifest = ManifestFile::at(root, NATIVE_MANIFEST);
    let (native, native_read): (WrittenNative, _) = native_manifest.read(listing)?;
    native_manifest.of_format(&native.format, NATIVE_FORMAT)?;
    let mut plan = TreePlan::new();
    let mut files = disc_manifest.expected(&disc.files, &DISC_ROWS, &mut plan)?;
    let boot = own_path(IP_BIN);
    disc_manifest.plan(&mut plan, &[boot.clone(), own_path(DISC_MANIFEST)])?;
    files.push(Expected {
        path: boot,
        size: BOOT_LEN as u64,
        sha256: None,
        held: Held::Fingerprint,
    });
    files.extend(native_manifest.expected(&native.files, &NATIVE_ROWS, &mut plan)?);
    native_manifest.plan(&mut plan, &[own_path(NATIVE_MANIFEST)])?;
    Ok(Expectations {
        manifests: vec![disc_read, native_read],
        files,
        table: Some(NativeTable::of(&native)),
    })
}

/// A check of a tree's files under way: what it holds them to, and what it has found.
struct Check<'a> {
    /// The tree's root.
    root: &'a Path,
    /// What the tree's walk found.
    listing: &'a Listing,
    /// The fingerprints the tree's files from the disc are held to, when there are any.
    fingerprints: Option<&'a Fingerprints>,
    /// How a Dreamcast tree's native files are held to its DISK.ROM and SIZES.ROM, when the tree
    /// is held to fingerprints.
    restoring: Option<Restoring>,
    /// How the check is asked to stop, and where its progress goes.
    job: Job<'a>,
    /// What each read of a file fills.
    block: Vec<u8>,
    /// How many of the bytes the manifests expect have been read or passed over.
    done: u64,
    /// How many bytes the manifests expect the tree's files to hold.
    total: u64,
    /// How many bytes have been read, the manifests' included.
    bytes: u64,
    /// What is wrong, as far as the check has found.
    damage: Vec<Damage>,
    /// Every entry a damage names in another letter case in place of a file it stands for.
    claimed: BTreeSet<String>,
}

impl Check<'_> {
    /// Checks `file` against what the tree holds at its path, and writes down what is wrong with
    /// it: whether it is there, there only in another letter case, a regular file of its size, and
    /// then whether its digest is the one its manifest records and, held to fingerprints, the one
    /// the trusted disc holds it to. The job's progress is then told the check has come past it.
    fn file(&mut self, file: &Expected) -> Result<(), VerifyError> {
        let path = file.path.as_str();
        let end = self.done.saturating_add(file.size);
        let listing = self.listing;
        let found = match listing.kind(path) {
            Some(EntryKind::File) => self.read(file, end)?,
            Some(EntryKind::Folder | EntryKind::Other) => Found::Damaged(DamageKind::NotAFile),
            None => Found::Damaged(match listing.in_other_case(path) {
                Some(found) => {
                    self.claimed.insert(found.to_owned());
                    DamageKind::LetterCase {
                        found: found.to_owned(),
                    }
                }
                None => DamageKind::Missing,
            }),
        };
        match found {
            Found::Whole(digest) => {
                if file.sha256.is_some_and(|recorded| recorded != digest) {
                    self.damaged(path, DamageKind::Digest);
                }
                if !self.trusted(file, digest) {
                    self.damaged(path, DamageKind::Fingerprint);
                }
            }
            Found::Damaged(kind) => self.damaged(path, kind),
        }
        self.advance_to(end, &file.path);
        Ok(())
    }

    /// Whether `file`, whose bytes have the digest `digest`, is what the trusted disc holds it to
    /// when the check is held to fingerprints: IP.BIN or a disc file its own fingerprint, a native
    /// file the part of DISK.ROM it was restored from. Every file is, held to none.
    fn trusted(&self, file: &Expected, digest: Sha256Digest) -> bool {
        let Some(fingerprints) = self.fingerprints else {
            return true;
        };
        match file.held {
            Held::Nothing => true,
            Held::Fingerprint => fingerprints.get(file.path.as_str()) == Some(digest),
            Held::Restored(row) => self
                .restoring
                .as_ref()
                .is_some_and(|restoring| restoring.holds(row, digest)),
        }
    }

    /// Reads `file`, which the walk found a regular file, and gives the digest of its bytes, read
    /// in blocks with the job's cancel looked at before each, its progress told after each how
    /// far the check has come towards `end`, where the file's bytes end among those the manifests
    /// expect. What is at its path is looked at again first, and what is opened too: a file that
    /// is no longer a regular file, or that holds another number of bytes than its manifest
    /// records, as it is opened or as it is read, one byte past its size at most, is damage. When
    /// the native files are held to the disc's, DISK.ROM's parts are hashed as it is read, and
    /// SIZES.ROM's bytes kept when it holds no more than a table may; each is kept once it was
    /// read whole.
    fn read(&mut self, file: &Expected, end: u64) -> Result<Found, VerifyError> {
        let path = file.path.path_under(self.root);
        let failed = |source: io::Error| VerifyError::Io {
            path: path.clone(),
            source,
        };
        if !fs::symlink_metadata(&path).map_err(failed)?.is_file() {
            return Ok(Found::Damaged(DamageKind::NotAFile));
        }
        let opened = File::open(&path).map_err(failed)?;
        let metadata = opened.metadata().map_err(failed)?;
        if !metadata.is_file() {
            return Ok(Found::Damaged(DamageKind::NotAFile));
        }
        if metadata.len() != file.size {
            return Ok(Found::Damaged(DamageKind::Size {
                expected: file.size,
                found: metadata.len(),
            }));
        }
        let restoring = self.restoring.as_ref();
        let mut parts = restoring
            .filter(|restoring| restoring.disk_rom == file.path)
            .map(|restoring| Parts::of(&restoring.table));
        let is_table = restoring.is_some_and(|restoring| restoring.sizes_rom == file.path);
        let mut kept = (is_table && file.size <= u64::from(MAX_SIZES_ROM_LEN)).then(Vec::new);
        let mut reader = opened.take(file.size.saturating_add(1));
        let mut digest = Sha256::new();
        let start = self.done;
        let mut read: u64 = 0;
        loop {
            self.job.cancel.check()?;
            let got = match reader.read(&mut self.block) {
                Ok(got) => got,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(failed(error)),
            };
            if got == 0 {
                break;
            }
            let block = &self.block[..got];
            digest.update(block);
            if let Some(parts) = &mut parts {
                parts.take(read, block);
            }
            if let Some(kept) = &mut kept {
                kept.extend_from_slice(block);
            }
            read += got as u64;
            self.bytes += got as u64;
            self.advance_to(start.saturating_add(read).min(end), &file.path);
        }
        if read != file.size {
            return Ok(Found::Damaged(DamageKind::Size {
                expected: file.size,
                found: read,
            }));
        }
        if let Some(restoring) = &mut self.restoring {
            if let Some(parts) = parts {
                restoring.parts = Some(parts.digests(read));
                restoring.disk_len = Some(read);
            }
            if kept.is_some() {
                restoring.sizes = kept;
            }
        }
        Ok(Found::Whole(Sha256Digest(digest.finalize().into())))
    }

    /// Tells the job's progress the check has come to `done` of its total, at `item`, when that is
    /// further than it told last.
    fn advance_to(&mut self, done: u64, item: &RelPath) {
        if done > self.done {
            self.done = done;
            self.job.progress.advance(done, self.total, Some(item));
        }
    }

    /// Writes down that the file at `path` is damaged as `kind` says.
    fn damaged(&mut self, path: &str, kind: DamageKind) {
        self.damage.push(Damage {
            path: path.to_owned(),
            kind,
        });
    }

    /// Writes down the native manifest as damaged when the check holds a Dreamcast tree's native
    /// files to its DISK.ROM and SIZES.ROM and the manifest does not record the table SIZES.ROM
    /// gives against DISK.ROM.
    fn restored_table(&mut self) {
        let restoring = self.restoring.as_ref();
        if restoring.is_some_and(|restoring| !restoring.table_follows()) {
            self.damaged(NATIVE_MANIFEST, DamageKind::Fingerprint);
        }
    }

    /// Writes down as unlisted every one of `fingerprints` kept under the key of no file of `files`
    /// that is one of the disc's, in the order of the keys.
    fn unlisted(&mut self, fingerprints: &Fingerprints, files: &[Expected]) {
        let listed: BTreeSet<Sha256Digest> = files
            .iter()
            .filter(|file| file.held == Held::Fingerprint)
            .map(|file| Fingerprints::path_key(file.path.as_str()))
            .collect();
        for key in fingerprints.keys().filter(|key| !listed.contains(key)) {
            self.damage.push(Damage {
                path: key.hex(),
                kind: DamageKind::Unlisted,
            });
        }
    }
}

/// What reading a file the manifests expect found.
enum Found {
    /// The file whole, of its size, and the digest of its bytes.
    Whole(Sha256Digest),
    /// What keeps the file from being checked further.
    Damaged(DamageKind),
}

/// The check's failure for a failure of the reading the files crate does for it at `at`: a link on
/// the way is [`VerifyError::Links`], and the system's refusal [`VerifyError::Io`]. That reading
/// fails in no other way; were it to, its failure is kept as an I/O error at `at`.
fn read_failure(error: FilesError, at: &Path) -> VerifyError {
    match error {
        FilesError::Link { path } => VerifyError::Links { path },
        FilesError::Io { path, source } => VerifyError::Io { path, source },
        error => VerifyError::Io {
            path: at.to_path_buf(),
            source: io::Error::other(error),
        },
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::env;
    use std::error::Error;
    use std::ffi::OsStr;
    use std::fs::{self, OpenOptions};
    use std::io::{self, Write};
    use std::path::{Path, PathBuf};

    use gdl_files::{
        Cancel, Cancelled, DISC_NAMES, FilesError, Job, MAX_JSON_LEN, RelPath, Staging,
        retry_locked,
    };
    use gdl_fixtures::{
        DiscSet, Edc, backdate, hold_closed, holds_bind, link_folder, sha256_hex, snapshot,
        symlink_file_where_allowed,
    };
    use gdl_media::{BOOT_LEN, N64Rom, ROMDISK_LIMITS, Sha256Digest, SourceName};
    use serde_json::{Value, json};

    use super::{
        Damage, DamageKind, NOT_WRITTEN_SO, VerifyError, VerifyLevel, VerifyReport, read_failure,
        verify_tree, verify_tree_with,
    };
    use crate::vectors::{
        CancelAt, DISC_SWEEP, FULL_FINGERPRINTS, Folders, Listed, Recorded, Told, array,
        dreamcast_golden, dreamcast_root, dreamcast_vectors, extraction_case, fingerprint_files,
        frozen, listed, n64_golden, n64_root, n64_vectors, quiet, stage_dreamcast, stage_n64, text,
        tree_listing,
    };
    use crate::{
        DISC_FORMAT, DISC_MANIFEST, Expansion, ExtractSettings, Fingerprints, IP_BIN, N64_MANIFEST,
        NATIVE_MANIFEST, Strictness,
    };

    /// The research tools' audio catalogue, which they write beside an N64 tree.
    const AUDIO_CATALOG: &str = "audio-catalog.json";

    /// The folder the research tools write a Dreamcast tree's audio into, beside it.
    const AUDIO_WAV: &str = "audio-wav";

    /// The full set's stream, the first file the walk of its disc meets.
    const STREAM: &str = "disc/AUDIO/X.STR";

    /// The full set's DISK.ROM, which packs its native files.
    const PACKED: &str = "disc/DISK.ROM";

    /// The full set's SIZES.ROM, the table of the files DISK.ROM packs.
    const TABLE: &str = "disc/SIZES.ROM";

    /// The full set's file of no bytes.
    const EMPTY: &str = "disc/EMPTY.BIN";

    /// The first native file the full set's table places in DISK.ROM, restored in the tree.
    const RESTORED: &str = "native/levels/a.wad";

    /// The second native file the full set's table places in DISK.ROM: a file of no bytes, where
    /// the first ends.
    const RESTORED_EMPTY: &str = "native/levels/b.rom";

    /// The folder the full set's native files are restored into.
    const NATIVE_LEVELS: &str = "native/levels";

    /// The plain file of the N64 vectors' default image.
    const TEXTURE: &str = "texture.bin";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The repository's root, from this crate's folder.
    const REPOSITORY_FROM_CRATE: &str = "../../../..";

    /// The variable that names the N64 tree the extraction scripts wrote of the player's own
    /// cartridge, for the test that needs it.
    const N64_TREE_VARIABLE: &str = "GDL_LEGENDS_N64_TREE";

    /// Where that tree lies in the repository's assets, when the variable names no other place.
    const N64_TREE_IN_ASSETS: &str = "assets/n64/extracted";

    /// The variable that names the Dreamcast tree the extraction scripts wrote of the player's own
    /// disc, for the test that needs it.
    const DREAMCAST_TREE_VARIABLE: &str = "GDL_LEGENDS_DREAMCAST_TREE";

    /// Where that tree lies in the repository's assets, when the variable names no other place.
    const DREAMCAST_TREE_IN_ASSETS: &str = "assets/dreamcast/extracted";

    /// How many paths the player's N64 tree's manifest expects: its 618 files, its 4 overlays and
    /// itself.
    const N64_PATHS: u64 = 623;

    /// How many paths the player's Dreamcast tree's manifests expect: its 156 disc files, IP.BIN,
    /// its 1,361 native files and the two manifests.
    const DREAMCAST_PATHS: u64 = 1_520;

    /// A tree a test checks, which an extraction wrote into a staging and which stays there until
    /// the test is done with it.
    struct Tree {
        /// The tree's root.
        root: PathBuf,
        /// The staging it lies in, removed with it before the folders it lies in.
        _staging: Staging,
        /// The folders the staging and the dump lie in.
        _folders: Folders,
    }

    /// The full set's tree, as an extraction under the extractor's rules writes it.
    fn dreamcast_tree() -> Tree {
        let folders = Folders::new();
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let cancel = Cancel::default();
        let (script, shipped) = (Strictness::Script, ExtractSettings::SHIPPED);
        let staged = stage_dreamcast(&sheet, &folders.parent, &script, &shipped, &quiet(&cancel));
        let (staging, _) = staged.unwrap();
        Tree {
            root: dreamcast_root().path_under(staging.path()),
            _staging: staging,
            _folders: folders,
        }
    }

    /// The tree of the N64 vectors' default image, as an extraction under the extractor's rules
    /// writes it.
    fn n64_tree() -> Tree {
        let vectors = n64_vectors();
        let case = extraction_case(&vectors, "default");
        let folders = Folders::new();
        let name = OsStr::new(text(&case["name"]));
        let dump = folders.dump(name, &frozen(&vectors, "default"));
        let cancel = Cancel::default();
        let (script, shipped) = (Strictness::Script, ExtractSettings::SHIPPED);
        let staged = stage_n64(&dump, &folders.parent, &script, &shipped, &quiet(&cancel));
        let (staging, _) = staged.unwrap();
        Tree {
            root: n64_root().path_under(staging.path()),
            _staging: staging,
            _folders: folders,
        }
    }

    /// What checking the tree of `expansion` at `root`, held to `fingerprints`, finds.
    #[track_caller]
    fn checked(
        root: &Path,
        expansion: Expansion,
        fingerprints: Option<&Fingerprints>,
    ) -> VerifyReport {
        let cancel = Cancel::default();
        verify_tree(root, expansion, fingerprints, &quiet(&cancel)).unwrap()
    }

    /// The manifest checking the tree of `expansion` at `root` stops at, and what is wrong with it.
    #[track_caller]
    fn refused_manifest(root: &Path, expansion: Expansion) -> (PathBuf, String) {
        let cancel = Cancel::default();
        match verify_tree(root, expansion, None, &quiet(&cancel)) {
            Err(VerifyError::Manifest { path, detail }) => (path, detail.into()),
            other => panic!("{other:?}"),
        }
    }

    /// The damage `kind` at the tree's path `path`.
    fn damage(path: &str, kind: DamageKind) -> Damage {
        Damage {
            path: path.to_owned(),
            kind,
        }
    }

    /// The size the vectors' `tree` lists the file at `path` with.
    fn size_in(tree: &[Listed], path: &str) -> u64 {
        let row = tree.iter().find(|(listed, _, _)| listed == path);
        row.map(|(_, size, _)| *size)
            .unwrap_or_else(|| panic!("no {path} in {tree:?}"))
    }

    /// The tree's path `path`, its components joined by `/`, under `root`.
    fn at(root: &Path, path: &str) -> PathBuf {
        RelPath::parse(path, &DISC_NAMES).unwrap().path_under(root)
    }

    /// Changes the first byte of the file at `path`, which keeps its length, and gives what the
    /// file holds then.
    fn flip_first_byte(path: &Path) -> Vec<u8> {
        let mut bytes = fs::read(path).unwrap();
        bytes[0] ^= 1;
        fs::write(path, &bytes).unwrap();
        bytes
    }

    /// Puts `bytes` at the end of the file at `path`.
    fn append(path: &Path, bytes: &[u8]) {
        let mut file = OpenOptions::new().append(true).open(path).unwrap();
        file.write_all(bytes).unwrap();
    }

    /// Makes the file at `path` `len` bytes long: cut short, or grown with zeros.
    fn resize(path: &Path, len: u64) {
        let file = OpenOptions::new().write(true).open(path).unwrap();
        file.set_len(len).unwrap();
    }

    /// Removes the file or the real folder at `path`, waiting out a scanner that holds it.
    fn remove(path: &Path) {
        let removed = if fs::symlink_metadata(path).unwrap().is_dir() {
            retry_locked(|| fs::remove_dir_all(path))
        } else {
            retry_locked(|| fs::remove_file(path))
        };
        removed.unwrap();
    }

    /// Renames `from` to `to`, waiting out a scanner that holds what it names.
    fn rename(from: &Path, to: &Path) {
        retry_locked(|| fs::rename(from, to)).unwrap();
    }

    /// Rewrites the manifest at `path` as `edit` changes it, as JSON of its shape.
    fn edit_manifest(path: &Path, edit: impl FnOnce(&mut Value)) {
        let mut manifest: Value = serde_json::from_slice(&fs::read(path).unwrap()).unwrap();
        edit(&mut manifest);
        fs::write(path, serde_json::to_vec_pretty(&manifest).unwrap()).unwrap();
    }

    /// The row of `manifest`'s files at `path`, the file's path in the manifest.
    fn row_of<'m>(manifest: &'m mut Value, path: &str) -> &'m mut Value {
        let rows = manifest["files"].as_array_mut().unwrap();
        rows.iter_mut().find(|row| row["path"] == path).unwrap()
    }

    /// Takes the row of `manifest`'s files at `path` out of it.
    fn drop_row(manifest: &mut Value, path: &str) {
        let rows = manifest["files"].as_array_mut().unwrap();
        let before = rows.len();
        rows.retain(|row| row["path"] != path);
        assert_eq!(rows.len() + 1, before, "{path}");
    }

    /// A tree the extraction scripts wrote of one of the player's own dumps, `what` it is: the
    /// folder the variable `variable` names, else the one at `in_assets` in the repository. Fails,
    /// naming the variable to set, when there is none.
    fn reference_tree(variable: &str, in_assets: &str, what: &str) -> PathBuf {
        let path = env::var_os(variable).map_or_else(
            || {
                Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join(REPOSITORY_FROM_CRATE)
                    .join(in_assets)
            },
            PathBuf::from,
        );
        assert!(
            path.is_dir(),
            "no {what} tree at {}: set {variable} to the tree the extraction scripts wrote of the \
             player's dump",
            path.display()
        );
        path
    }

    /// The name of the kind of damage `kind` is, which says nothing of the file it is found in.
    fn kind_name(kind: &DamageKind) -> &'static str {
        match kind {
            DamageKind::Missing => "missing",
            DamageKind::NotAFile => "notAFile",
            DamageKind::Size { .. } => "size",
            DamageKind::Digest => "digest",
            DamageKind::Fingerprint => "fingerprint",
            DamageKind::Unlisted => "unlisted",
            DamageKind::LetterCase { .. } => "letterCase",
        }
    }

    /// What kind of failure `error` is, without the words of what the tree holds.
    fn failure_kind(error: &VerifyError) -> String {
        match error {
            VerifyError::Links { .. } => "a link on the tree's path".to_owned(),
            VerifyError::Manifest { path, .. } => {
                let name = path.file_name().unwrap_or_default();
                format!("the manifest {}", name.to_string_lossy())
            }
            VerifyError::Io { source, .. } => format!("a read failed: {:?}", source.kind()),
            VerifyError::Cancelled => "cancelled".to_owned(),
        }
    }

    /// Fails unless `report` found nothing wrong with the tree of `what`, saying how much of each
    /// kind of damage it found, and nothing of the files it found it in.
    #[track_caller]
    fn assert_whole(report: &VerifyReport, what: &str) {
        let mut kinds: BTreeMap<&str, usize> = BTreeMap::new();
        for found in &report.damage {
            *kinds.entry(kind_name(&found.kind)).or_default() += 1;
        }
        assert!(kinds.is_empty(), "the {what} tree is damaged: {kinds:?}");
    }

    #[test]
    fn an_extracted_tree_verifies_and_a_changed_byte_is_damage() {
        let vectors = dreamcast_vectors();
        let full = &vectors["sets"]["full"];
        let golden = listed(&full["tree"]);
        let tree = dreamcast_tree();
        assert_eq!(tree_listing(&tree.root), golden);

        // Checked again, the tree is whole: every file its manifests list, IP.BIN, the native
        // files restored from DISK.ROM, and the manifests, which are the extractor's own.
        backdate(&tree.root).unwrap();
        let before = snapshot(&tree.root).unwrap();
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert!(report.is_intact(), "{:?}", report.damage);
        assert_eq!(report.expansion, Expansion::Dreamcast);
        assert_eq!(report.level, VerifyLevel::Manifests);
        assert_eq!(report.checked, golden.len() as u64);
        let whole: u64 = golden.iter().map(|(_, size, _)| size).sum();
        assert_eq!(report.bytes, whole);
        let manifests = [DISC_MANIFEST, NATIVE_MANIFEST].map(|name| {
            let written = dreamcast_golden(&full["goldens"][name]);
            (name.to_owned(), Sha256Digest::of(written))
        });
        assert_eq!(report.manifests, manifests);
        assert!(report.extra.is_empty(), "{:?}", report.extra);
        // The check wrote nothing, and changed nothing it read.
        assert!(
            snapshot(&tree.root).unwrap() == before,
            "the check changed the tree"
        );

        // A byte of DISK.ROM changed is damage to it alone: the native file restored from it is
        // as its manifest records it.
        let changed = flip_first_byte(&at(&tree.root, PACKED));
        backdate(&tree.root).unwrap();
        let before = snapshot(&tree.root).unwrap();
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert_eq!(report.damage, [damage(PACKED, DamageKind::Digest)]);
        assert!(!report.is_intact());
        assert_eq!(report.bytes, whole);
        // It stays as it was changed: a check repairs nothing.
        assert!(
            snapshot(&tree.root).unwrap() == before,
            "the check changed the tree"
        );
        assert_eq!(fs::read(at(&tree.root, PACKED)).unwrap(), changed);
    }

    #[test]
    fn a_longer_or_shorter_file_is_damage() {
        let vectors = dreamcast_vectors();
        let golden = listed(&vectors["sets"]["full"]["tree"]);
        let sized = |expected: u64, found: u64| DamageKind::Size { expected, found };
        let tree = dreamcast_tree();
        // A byte more in the stream, and a byte less in IP.BIN and in a native file.
        let (stream, restored) = (size_in(&golden, STREAM), size_in(&golden, RESTORED));
        let boot = BOOT_LEN as u64;
        append(&at(&tree.root, STREAM), b"x");
        resize(&at(&tree.root, IP_BIN), boot - 1);
        resize(&at(&tree.root, RESTORED), restored - 1);
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        let expected = [
            damage(STREAM, sized(stream, stream + 1)),
            damage(IP_BIN, sized(boot, boot - 1)),
            damage(RESTORED, sized(restored, restored - 1)),
        ];
        assert_eq!(report.damage, expected);
        // A file of another size is not read: what was read is every other file's.
        let whole: u64 = golden.iter().map(|(_, size, _)| size).sum();
        assert_eq!(report.bytes, whole - stream - boot - restored);
        assert!(report.extra.is_empty(), "{:?}", report.extra);

        // And a file of the N64 tree.
        let n64 = n64_tree();
        let texture = size_in(&listed(&n64_vectors()["trees"]["default"]), TEXTURE);
        append(&at(&n64.root, TEXTURE), b"x");
        let report = checked(&n64.root, Expansion::N64, None);
        assert_eq!(
            report.damage,
            [damage(TEXTURE, sized(texture, texture + 1))]
        );
    }

    #[test]
    fn a_missing_file_is_damage_and_an_extra_one_is_reported() {
        // The research tools' audio catalogue beside an N64 tree is reported, and is no damage.
        let n64 = n64_tree();
        fs::write(at(&n64.root, AUDIO_CATALOG), b"{}\n").unwrap();
        let report = checked(&n64.root, Expansion::N64, None);
        assert!(report.is_intact(), "{:?}", report.damage);
        assert_eq!(report.extra, [AUDIO_CATALOG]);
        let golden = listed(&n64_vectors()["trees"]["default"]);
        assert_eq!(report.checked, golden.len() as u64);
        // Without one of its files, the tree is damaged.
        remove(&at(&n64.root, TEXTURE));
        let report = checked(&n64.root, Expansion::N64, None);
        assert_eq!(report.damage, [damage(TEXTURE, DamageKind::Missing)]);
        assert_eq!(report.extra, [AUDIO_CATALOG]);

        // Beside a Dreamcast tree, the research tools' audio folder is reported by its files, a
        // folder in it by its own, and an empty folder nothing expects by itself.
        let tree = dreamcast_tree();
        let research = [
            format!("{AUDIO_WAV}/a.wav"),
            format!("{AUDIO_WAV}/more/b.wav"),
        ];
        for path in &research {
            let place = at(&tree.root, path);
            fs::create_dir_all(place.parent().unwrap()).unwrap();
            fs::write(place, b"RIFF").unwrap();
        }
        fs::create_dir(at(&tree.root, "notes")).unwrap();
        // A disc file of no bytes gone, IP.BIN gone, and the native files' folder gone with what
        // it held: each file is missing, and the folders that held them are no extras.
        for path in [EMPTY, IP_BIN, NATIVE_LEVELS] {
            remove(&at(&tree.root, path));
        }
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        let expected = [
            damage(EMPTY, DamageKind::Missing),
            damage(IP_BIN, DamageKind::Missing),
            damage(RESTORED, DamageKind::Missing),
            damage("native/levels/b.rom", DamageKind::Missing),
        ];
        assert_eq!(report.damage, expected);
        let [first, second] = research;
        assert_eq!(report.extra, [first, second, "notes".to_owned()]);
    }

    #[test]
    fn letter_case_is_checked_on_every_system() {
        let tree = dreamcast_tree();
        // DISK.ROM and the native files' folder, each renamed in another letter case. A system
        // that folds letter case would open the files by the paths the manifests give them, so
        // only the names the tree holds tell them apart, and they do on every system.
        rename(&at(&tree.root, PACKED), &at(&tree.root, "disc/disk.rom"));
        let upper = "native/LEVELS";
        rename(&at(&tree.root, NATIVE_LEVELS), &at(&tree.root, upper));
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        let found = |path: &str| DamageKind::LetterCase {
            found: path.to_owned(),
        };
        let expected = [
            damage(PACKED, found("disc/disk.rom")),
            damage(RESTORED, found("native/LEVELS/a.wad")),
            damage("native/levels/b.rom", found("native/LEVELS/b.rom")),
        ];
        assert_eq!(report.damage, expected);
        // What the damage names in a file's place is no extra.
        assert!(report.extra.is_empty(), "{:?}", report.extra);

        // A manifest in another letter case is not the tree's manifest.
        let manifest = at(&tree.root, DISC_MANIFEST);
        rename(&manifest, &at(&tree.root, "DISC-MANIFEST.JSON"));
        let (path, detail) = refused_manifest(&tree.root, Expansion::Dreamcast);
        assert_eq!(path, manifest);
        assert_eq!(detail, "the tree holds it only as DISC-MANIFEST.JSON");
    }

    #[test]
    fn fingerprints_raise_the_level() {
        // The fingerprints the writer took of the full set's tree: IP.BIN's and those of every
        // file of it from the disc.
        let written = Fingerprints::parse(FULL_FINGERPRINTS).unwrap();
        let tree = dreamcast_tree();
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        assert!(report.is_intact(), "{:?}", report.damage);
        assert_eq!(report.level, VerifyLevel::Fingerprints);

        // IP.BIN changed, whose digest no manifest records, and DISK.ROM changed with its row, so
        // that the disc manifest records what it holds now: the tree is whole against its
        // manifests, and not against the fingerprints, and neither is the native file restored
        // from the part of DISK.ROM that changed.
        flip_first_byte(&at(&tree.root, IP_BIN));
        let packed = flip_first_byte(&at(&tree.root, PACKED));
        edit_manifest(&at(&tree.root, DISC_MANIFEST), |manifest| {
            row_of(manifest, "DISK.ROM")["sha256"] = json!(sha256_hex(&packed));
        });
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert!(report.is_intact(), "{:?}", report.damage);
        assert_eq!(report.level, VerifyLevel::Manifests);
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        let expected = [
            damage(PACKED, DamageKind::Fingerprint),
            damage(IP_BIN, DamageKind::Fingerprint),
            damage(RESTORED, DamageKind::Fingerprint),
        ];
        assert_eq!(report.damage, expected);

        // A disc file changed without its row is damage against both; SIZES.ROM's change is to
        // the table, which the native manifest then no longer records.
        flip_first_byte(&at(&tree.root, TABLE));
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        let expected = [
            damage(PACKED, DamageKind::Fingerprint),
            damage(TABLE, DamageKind::Digest),
            damage(TABLE, DamageKind::Fingerprint),
            damage(IP_BIN, DamageKind::Fingerprint),
            damage(RESTORED, DamageKind::Fingerprint),
            damage(NATIVE_MANIFEST, DamageKind::Fingerprint),
        ];
        assert_eq!(report.damage, expected);

        // Fingerprints that hold none for a file of the disc, and one for a file it does not
        // hold.
        let tree = dreamcast_tree();
        let mut files = fingerprint_files(&tree_listing(&tree.root));
        files.retain(|(path, _)| path != EMPTY);
        let stranger = "disc/OTHER.BIN";
        files.push((stranger.to_owned(), Sha256Digest::of(b"other")));
        let partial = Fingerprints::from_files(files).unwrap();
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&partial));
        let unlisted = Fingerprints::path_key(stranger).hex();
        let expected = [
            damage(EMPTY, DamageKind::Fingerprint),
            damage(&unlisted, DamageKind::Unlisted),
        ];
        assert_eq!(report.damage, expected);

        // An N64 tree holds none of a disc's files: each fingerprint given for it is of no file
        // it lists.
        let n64 = n64_tree();
        let report = checked(&n64.root, Expansion::N64, Some(&written));
        assert_eq!(report.level, VerifyLevel::Fingerprints);
        let unlisted: Vec<Damage> = written
            .keys()
            .map(|key| damage(&key.hex(), DamageKind::Unlisted))
            .collect();
        assert_eq!(report.damage, unlisted);
    }

    #[test]
    fn a_manifest_cut_short_is_damage_at_the_fingerprint_level() {
        let written = Fingerprints::parse(FULL_FINGERPRINTS).unwrap();
        let tree = dreamcast_tree();
        // The disc manifest without the stream's row: against the manifests the tree is whole,
        // the stream an extra; against the fingerprints, the stream's is of no file the
        // manifests list.
        edit_manifest(&at(&tree.root, DISC_MANIFEST), |manifest| {
            drop_row(manifest, "AUDIO/X.STR");
        });
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert!(report.is_intact(), "{:?}", report.damage);
        assert_eq!(report.extra, [STREAM]);
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        let stream = Fingerprints::path_key(STREAM).hex();
        assert_eq!(report.damage, [damage(&stream, DamageKind::Unlisted)]);
        assert_eq!(report.extra, [STREAM]);

        // A native file has no fingerprint of its own, but the table it was restored by has:
        // the native manifest without a row of one leaves the file an extra, and no longer
        // records the table SIZES.ROM gives.
        edit_manifest(&at(&tree.root, NATIVE_MANIFEST), |manifest| {
            drop_row(manifest, "levels/a.wad");
        });
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert!(report.is_intact(), "{:?}", report.damage);
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        let expected = [
            damage(NATIVE_MANIFEST, DamageKind::Fingerprint),
            damage(&stream, DamageKind::Unlisted),
        ];
        assert_eq!(report.damage, expected);
        assert_eq!(report.extra, [STREAM, RESTORED]);
    }

    #[test]
    fn native_files_are_held_to_the_disc_they_were_restored_from() {
        let written = Fingerprints::parse(FULL_FINGERPRINTS).unwrap();
        // A native file changed with its row, so that the native manifest records what it holds
        // now: whole against the manifests, and not what DISK.ROM, which its fingerprint holds,
        // holds where the row places it.
        let tree = dreamcast_tree();
        let other = b"world";
        fs::write(at(&tree.root, RESTORED), other).unwrap();
        edit_manifest(&at(&tree.root, NATIVE_MANIFEST), |manifest| {
            row_of(manifest, "levels/a.wad")["sha256"] = json!(sha256_hex(other));
        });
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert!(report.is_intact(), "{:?}", report.damage);
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        assert_eq!(report.damage, [damage(RESTORED, DamageKind::Fingerprint)]);
        assert_eq!(report.level, VerifyLevel::Fingerprints);

        // A native manifest that records another table than SIZES.ROM gives: whole against the
        // manifests, and damage against the fingerprints. A row that gives its file no place in
        // DISK.ROM leaves that file held to no part of it as well.
        type Change = (&'static str, fn(&mut Value), &'static [&'static str]);
        let changes: [Change; 7] = [
            (
                "another digest of SIZES.ROM",
                |manifest| manifest["sizes_sha256"] = json!(sha256_hex(b"other")),
                &[],
            ),
            (
                "a file at another place",
                |manifest| row_of(manifest, "levels/b.rom")["offset"] = json!(0),
                &[],
            ),
            (
                "a file past DISK.ROM's end",
                |manifest| row_of(manifest, "levels/b.rom")["offset"] = json!(6),
                &[RESTORED_EMPTY],
            ),
            (
                "a file at no place",
                |manifest| {
                    let row = row_of(manifest, "levels/b.rom").as_object_mut().unwrap();
                    row.remove("offset");
                },
                &[RESTORED_EMPTY],
            ),
            (
                "the files in another order",
                |manifest| manifest["files"].as_array_mut().unwrap().reverse(),
                &[],
            ),
            (
                "no file outside DISK.ROM",
                |manifest| manifest["external"] = json!([]),
                &[],
            ),
            (
                "a file outside DISK.ROM of another size",
                |manifest| manifest["external"][0]["declared_size"] = json!(8),
                &[],
            ),
        ];
        for (case, change, unheld) in changes {
            let tree = dreamcast_tree();
            edit_manifest(&at(&tree.root, NATIVE_MANIFEST), change);
            let report = checked(&tree.root, Expansion::Dreamcast, None);
            assert!(report.is_intact(), "{case}: {:?}", report.damage);
            let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
            let mut expected: Vec<Damage> = unheld
                .iter()
                .map(|path| damage(path, DamageKind::Fingerprint))
                .collect();
            expected.push(damage(NATIVE_MANIFEST, DamageKind::Fingerprint));
            assert_eq!(report.damage, expected, "{case}");
        }

        // SIZES.ROM changed with its row: whole against the manifests, and against the
        // fingerprints neither SIZES.ROM nor the native manifest, which records its old table.
        let tree = dreamcast_tree();
        let table = flip_first_byte(&at(&tree.root, TABLE));
        edit_manifest(&at(&tree.root, DISC_MANIFEST), |manifest| {
            row_of(manifest, "SIZES.ROM")["sha256"] = json!(sha256_hex(&table));
        });
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert!(report.is_intact(), "{:?}", report.damage);
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        let expected = [
            damage(TABLE, DamageKind::Fingerprint),
            damage(NATIVE_MANIFEST, DamageKind::Fingerprint),
        ];
        assert_eq!(report.damage, expected);

        // Without DISK.ROM, no native file can be held to what it was restored from, and the
        // table cannot be read against it.
        let tree = dreamcast_tree();
        remove(&at(&tree.root, PACKED));
        let report = checked(&tree.root, Expansion::Dreamcast, Some(&written));
        let expected = [
            damage(PACKED, DamageKind::Missing),
            damage(RESTORED, DamageKind::Fingerprint),
            damage(RESTORED_EMPTY, DamageKind::Fingerprint),
            damage(NATIVE_MANIFEST, DamageKind::Fingerprint),
        ];
        assert_eq!(report.damage, expected);
    }

    #[test]
    fn a_tree_the_script_wrote_verifies() {
        let vectors = n64_vectors();
        // Each manifest the N64 extractor wrote, written as it wrote it beside the files of the
        // image it wrote it for, each written as a plain file: a tree as the extractor writes it.
        let extractions = array(&vectors["extractions"]).iter();
        let mut cases: Vec<(&str, &Value)> = extractions
            .map(|case| (text(&case["image"]), &case["golden"]))
            .collect();
        let byte_orders = array(&vectors["byteOrders"]).iter();
        cases.extend(byte_orders.map(|case| ("default", &case["golden"])));
        assert_eq!(cases.len(), 4);
        let files = |listing: Vec<Listed>| -> Vec<Listed> {
            let rows = listing.into_iter();
            rows.filter(|(path, _, _)| path != N64_MANIFEST).collect()
        };
        for (image, golden) in cases {
            let dir = tempfile::tempdir().unwrap();
            let root = dir.path().join("extracted");
            let name = SourceName::from_units("source.z64".encode_utf16().collect());
            let rom = N64Rom::from_bytes(name, frozen(&vectors, image)).unwrap();
            let romdisk = rom.romdisk(&ROMDISK_LIMITS).unwrap();
            for entry in &romdisk.entries {
                let place = entry.path.path_under(&root);
                fs::create_dir_all(place.parent().unwrap()).unwrap();
                fs::write(place, romdisk.decode(entry).unwrap()).unwrap();
            }
            let manifest = n64_golden(golden);
            fs::write(root.join(N64_MANIFEST), manifest).unwrap();
            // The files are those the vectors list for the image's tree.
            let expected = files(listed(&vectors["trees"][image]));
            assert_eq!(files(tree_listing(&root)), expected, "{golden}");
            let report = checked(&root, Expansion::N64, None);
            assert!(report.is_intact(), "{golden}: {:?}", report.damage);
            assert!(report.extra.is_empty(), "{golden}: {:?}", report.extra);
            assert_eq!(report.checked, expected.len() as u64 + 1, "{golden}");
            let digest = Sha256Digest::of(manifest);
            assert_eq!(report.manifests, [(N64_MANIFEST.to_owned(), digest)]);
        }
    }

    #[test]
    fn manifests_that_cannot_be_read_stop_the_check() {
        /// A change to a tree, and the manifest and the words its check then stops at: the whole
        /// detail, or its start where the JSON reader's own words close it.
        type Case = (&'static str, fn(&Path), &'static str, &'static str);
        let dreamcast: [Case; 13] = [
            (
                "no disc manifest",
                |root| remove(&at(root, DISC_MANIFEST)),
                DISC_MANIFEST,
                "the tree holds no file of this name",
            ),
            (
                "a folder for the native manifest",
                |root| {
                    let path = at(root, NATIVE_MANIFEST);
                    remove(&path);
                    fs::create_dir(path).unwrap();
                },
                NATIVE_MANIFEST,
                "what the tree holds of this name is no regular file",
            ),
            (
                "a manifest too long to read",
                |root| resize(&at(root, DISC_MANIFEST), MAX_JSON_LEN + 1),
                DISC_MANIFEST,
                "the manifest holds more than 4194304 bytes",
            ),
            (
                "no JSON",
                |root| fs::write(at(root, DISC_MANIFEST), b"not json").unwrap(),
                DISC_MANIFEST,
                NOT_WRITTEN_SO,
            ),
            (
                "no files",
                |root| {
                    edit_manifest(&at(root, NATIVE_MANIFEST), |manifest| {
                        manifest.as_object_mut().unwrap().remove("files");
                    });
                },
                NATIVE_MANIFEST,
                NOT_WRITTEN_SO,
            ),
            (
                "a size below nought",
                |root| {
                    edit_manifest(&at(root, DISC_MANIFEST), |manifest| {
                        manifest["files"][0]["size"] = json!(-1);
                    });
                },
                DISC_MANIFEST,
                NOT_WRITTEN_SO,
            ),
            (
                "another format of the disc manifest",
                |root| {
                    edit_manifest(&at(root, DISC_MANIFEST), |manifest| {
                        manifest["format"] = json!("dreamcast-legends-disc-v2");
                    });
                },
                DISC_MANIFEST,
                "the manifest is not of the format dreamcast-legends-disc-v1",
            ),
            (
                "the disc's format for the native manifest",
                |root| {
                    edit_manifest(&at(root, NATIVE_MANIFEST), |manifest| {
                        manifest["format"] = json!(DISC_FORMAT);
                    });
                },
                NATIVE_MANIFEST,
                "the manifest is not of the format dreamcast-legends-native-v1",
            ),
            (
                "a path out of the tree",
                |root| {
                    edit_manifest(&at(root, DISC_MANIFEST), |manifest| {
                        manifest["files"][0]["path"] = json!("../X.STR");
                    });
                },
                DISC_MANIFEST,
                "files[0] holds a path the tree may not hold (dot)",
            ),
            (
                "a digest in capitals",
                |root| {
                    edit_manifest(&at(root, DISC_MANIFEST), |manifest| {
                        let row = &mut manifest["files"][1];
                        let digest = row["sha256"].as_str().unwrap().to_uppercase();
                        row["sha256"] = json!(digest);
                    });
                },
                DISC_MANIFEST,
                "files[1] holds a digest that is not 64 lowercase hexadecimal digits",
            ),
            (
                "one path in two letter cases",
                |root| {
                    edit_manifest(&at(root, DISC_MANIFEST), |manifest| {
                        manifest["files"][1]["path"] = json!("audio/x.str");
                    });
                },
                DISC_MANIFEST,
                "duplicate case-insensitive tree path: disc/audio/x.str",
            ),
            (
                "a native folder spelled two ways",
                |root| {
                    edit_manifest(&at(root, NATIVE_MANIFEST), |manifest| {
                        manifest["files"][1]["path"] = json!("LEVELS/b.rom");
                    });
                },
                NATIVE_MANIFEST,
                "a folder is spelled native/levels and native/LEVELS",
            ),
            (
                "a native file that is a folder too",
                |root| {
                    edit_manifest(&at(root, NATIVE_MANIFEST), |manifest| {
                        manifest["files"][1]["path"] = json!("levels/a.wad/c");
                    });
                },
                NATIVE_MANIFEST,
                "tree file/directory collision: native/levels/a.wad",
            ),
        ];
        for (case, change, name, words) in dreamcast {
            let tree = dreamcast_tree();
            change(&tree.root);
            let (path, detail) = refused_manifest(&tree.root, Expansion::Dreamcast);
            assert_eq!(path, at(&tree.root, name), "{case}");
            if words == NOT_WRITTEN_SO {
                assert!(
                    detail.starts_with(&format!("{words}: ")),
                    "{case}: {detail}"
                );
            } else {
                assert_eq!(detail, words, "{case}");
            }
        }

        let version = "the manifest is not version 1 of the format gauntlet-legends-n64-romdisk";
        let n64: [Case; 6] = [
            (
                "another version",
                |root| {
                    edit_manifest(&at(root, N64_MANIFEST), |manifest| {
                        manifest["version"] = json!(2);
                    });
                },
                N64_MANIFEST,
                version,
            ),
            (
                "another format",
                |root| {
                    edit_manifest(&at(root, N64_MANIFEST), |manifest| {
                        manifest["format"] = json!(DISC_FORMAT);
                    });
                },
                N64_MANIFEST,
                version,
            ),
            (
                "a file under the overlays' folder",
                |root| {
                    edit_manifest(&at(root, N64_MANIFEST), |manifest| {
                        manifest["files"][0]["path"] = json!("_program/x.bin");
                    });
                },
                N64_MANIFEST,
                "files[0] holds a path the tree may not hold (reserved)",
            ),
            (
                "an overlay out of the tree",
                |root| {
                    edit_manifest(&at(root, N64_MANIFEST), |manifest| {
                        let row = json!({"path": "../game.bin", "size": 4,
                                         "sha256": sha256_hex(b"game")});
                        manifest["programs"] = json!([row]);
                    });
                },
                N64_MANIFEST,
                "programs[0] holds a path the tree may not hold (dot)",
            ),
            (
                "an overlay at the manifest's own path",
                |root| {
                    edit_manifest(&at(root, N64_MANIFEST), |manifest| {
                        let row = json!({"path": "manifest.json", "size": 4,
                                         "sha256": sha256_hex(b"game")});
                        manifest["programs"] = json!([row]);
                    });
                },
                N64_MANIFEST,
                "duplicate case-insensitive tree path: manifest.json",
            ),
            (
                "the manifest in capitals",
                |root| rename(&at(root, N64_MANIFEST), &at(root, "MANIFEST.JSON")),
                N64_MANIFEST,
                "the tree holds it only as MANIFEST.JSON",
            ),
        ];
        for (case, change, name, words) in n64 {
            let tree = n64_tree();
            change(&tree.root);
            let (path, detail) = refused_manifest(&tree.root, Expansion::N64);
            assert_eq!(path, at(&tree.root, name), "{case}");
            assert_eq!(detail, words, "{case}");
        }
    }

    #[test]
    fn links_on_the_way_are_refused_and_links_within_are_never_followed() {
        let tree = dreamcast_tree();
        let elsewhere = tempfile::tempdir().unwrap();
        // The tree reached through a link, or through a link to the folder above it, is refused
        // before anything of it is read.
        let linked = elsewhere.path().join("linked");
        link_folder(&linked, &tree.root).unwrap();
        let above = elsewhere.path().join("above");
        link_folder(&above, tree.root.parent().unwrap()).unwrap();
        let through = above.join(tree.root.file_name().unwrap());
        let cancel = Cancel::default();
        for (root, link) in [(&linked, &linked), (&through, &above)] {
            match verify_tree(root, Expansion::Dreamcast, None, &quiet(&cancel)) {
                Err(VerifyError::Links { path }) => assert_eq!(path, *link),
                other => panic!("{}: {other:?}", root.display()),
            }
        }

        // A link within the tree in place of a folder of it is listed, and never followed: the
        // file the folder held is missing, though the folder the link leads to holds it, and the
        // link is an extra.
        let held = elsewhere.path().join("AUDIO");
        fs::create_dir(&held).unwrap();
        fs::copy(at(&tree.root, STREAM), held.join("X.STR")).unwrap();
        let audio = at(&tree.root, "disc/AUDIO");
        remove(&audio);
        link_folder(&audio, &held).unwrap();
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert_eq!(report.damage, [damage(STREAM, DamageKind::Missing)]);
        assert_eq!(report.extra, ["disc/AUDIO"]);

        // A link in place of a file is no file, where this account may make a link to a file.
        let packed = at(&tree.root, PACKED);
        let copy = elsewhere.path().join("DISK.ROM");
        fs::copy(&packed, &copy).unwrap();
        remove(&packed);
        if !symlink_file_where_allowed(&packed, &copy) {
            eprintln!(
                "skipped a link to a file: Windows withholds the privilege from this account"
            );
            return;
        }
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        let expected = [
            damage(STREAM, DamageKind::Missing),
            damage(PACKED, DamageKind::NotAFile),
        ];
        assert_eq!(report.damage, expected);
        assert_eq!(report.extra, ["disc/AUDIO"]);
    }

    #[test]
    fn a_file_that_cannot_be_read_stops_the_check() {
        let tree = dreamcast_tree();
        let cancel = Cancel::default();
        // A tree that is not there cannot be checked.
        let missing = tree.root.join("missing");
        match verify_tree(&missing, Expansion::Dreamcast, None, &quiet(&cancel)) {
            Err(VerifyError::Io { path, source }) => {
                assert_eq!(path, missing);
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("{other:?}"),
        }

        // A file of the tree that cannot be opened stops the check, which names it.
        let stream = at(&tree.root, STREAM);
        let probe = tempfile::tempdir().unwrap();
        let binds = holds_bind(probe.path()).unwrap();
        let held = hold_closed(&[&stream]).unwrap();
        assert_eq!(held.proven(), binds);
        if !binds {
            eprintln!("skipped a file that cannot be opened: this account opens every file");
            return;
        }
        match verify_tree(&tree.root, Expansion::Dreamcast, None, &quiet(&cancel)) {
            Err(VerifyError::Io { path, .. }) => assert_eq!(path, stream),
            other => panic!("{other:?}"),
        }
        // Once it can be read again, the tree is whole.
        drop(held);
        let report = checked(&tree.root, Expansion::Dreamcast, None);
        assert!(report.is_intact(), "{:?}", report.damage);
    }

    #[test]
    fn a_cancel_at_any_block_stops_the_check() {
        let vectors = dreamcast_vectors();
        let golden = listed(&vectors["sets"]["full"]["tree"]);
        let tree = dreamcast_tree();
        // A whole check, in blocks short enough that IP.BIN alone spans dozens, tells after each
        // block how far it has come in the bytes of the files the manifests expect, IP.BIN's
        // included, and in which file, in the order the extraction writes them; those of no bytes
        // tell nothing. It begins no phase of its own.
        let recorded = Recorded::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &recorded,
        };
        let dreamcast = Expansion::Dreamcast;
        let report = verify_tree_with(&tree.root, dreamcast, None, &DISC_SWEEP, &job).unwrap();
        assert!(report.is_intact(), "{:?}", report.damage);
        let manifests = [DISC_MANIFEST, NATIVE_MANIFEST];
        let files = golden
            .iter()
            .filter(|(path, _, _)| !manifests.contains(&path.as_str()));
        let (count, total) = files.fold((0, 0), |(count, total), (_, size, _)| {
            (count + 1, total + size)
        });
        let told = recorded.told();
        let mut items: Vec<String> = Vec::new();
        let mut last = 0;
        for told in &told {
            let Told::Advance {
                done,
                total: of,
                item,
            } = told
            else {
                panic!("the check began a phase: {told:?}");
            };
            assert_eq!(*of, total);
            assert!(*done > last, "{told:?}");
            last = *done;
            let item = item.clone().expect("an advance names its file");
            if items.last() != Some(&item) {
                items.push(item);
            }
        }
        assert_eq!(last, total);
        assert_eq!(items, [STREAM, PACKED, TABLE, IP_BIN, RESTORED]);
        assert!(told.len() > count * 2, "{} advances", told.len());

        // Asked to stop before it begins, or after any of its advances, the last included, it
        // stops before it lists a folder or reads a block more.
        for at in 0..=told.len() {
            let cancel = Cancel::default();
            let progress = CancelAt::new(&cancel, at);
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let stopped = verify_tree_with(&tree.root, dreamcast, None, &DISC_SWEEP, &job);
            let error = stopped.unwrap_err();
            assert!(matches!(error, VerifyError::Cancelled), "{at}: {error:?}");
        }
    }

    #[test]
    fn errors_say_what_failed() {
        let link = Path::new("installs").join("n64");
        let error = VerifyError::Links { path: link.clone() };
        let words = format!(
            "the tree's path holds a link or a junction: {}",
            link.display()
        );
        assert_eq!(error.to_string(), words);
        let manifest = Path::new("extracted").join(DISC_MANIFEST);
        let error = VerifyError::Manifest {
            path: manifest.clone(),
            detail: "the tree holds no file of this name".into(),
        };
        let words = format!(
            "{}: the tree holds no file of this name",
            manifest.display()
        );
        assert_eq!(error.to_string(), words);
        assert!(error.source().is_none());
        let file = Path::new("extracted").join(IP_BIN);
        let error = VerifyError::Io {
            path: file.clone(),
            source: io::Error::new(io::ErrorKind::PermissionDenied, "denied"),
        };
        assert_eq!(error.to_string(), format!("{}: denied", file.display()));
        assert_eq!(error.source().unwrap().to_string(), "denied");
        assert_eq!(VerifyError::Cancelled.to_string(), "cancelled");
        assert!(matches!(
            VerifyError::from(Cancelled),
            VerifyError::Cancelled
        ));

        // A link the files crate finds on the way is a link on the tree's path, its refusal of a
        // read the system's, and anything else it might give is kept as a failed read.
        let at = Path::new("extracted");
        let failure = read_failure(FilesError::Link { path: link.clone() }, at);
        assert!(matches!(failure, VerifyError::Links { path } if path == link));
        let refused = FilesError::Io {
            path: file.clone(),
            source: io::ErrorKind::PermissionDenied.into(),
        };
        let failure = read_failure(refused, at);
        let kind = io::ErrorKind::PermissionDenied;
        assert!(
            matches!(failure, VerifyError::Io { path, source } if path == file && source.kind() == kind)
        );
        let failure = read_failure(FilesError::Exists { path: file.clone() }, at);
        assert!(matches!(failure, VerifyError::Io { path, .. } if path == at));

        // Every failure is cheap to return, whatever it carries.
        assert!(size_of::<VerifyError>() <= MOST_ERROR_BYTES);
    }

    #[test]
    #[ignore = "needs the player's dump"]
    fn the_reference_trees_verify() {
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        // The N64 tree the extraction scripts wrote of the player's cartridge is whole against its
        // manifest: every file of the ROMDISK, every overlay and the manifest, with the research
        // tools' audio catalogue beside them.
        let n64 = reference_tree(N64_TREE_VARIABLE, N64_TREE_IN_ASSETS, "N64");
        let report = verify_tree(&n64, Expansion::N64, None, &job)
            .unwrap_or_else(|error| panic!("the N64 tree is refused: {}", failure_kind(&error)));
        assert_whole(&report, "N64");
        assert_eq!(report.level, VerifyLevel::Manifests);
        assert_eq!(report.checked, N64_PATHS);
        assert!(
            report.extra == [AUDIO_CATALOG],
            "the N64 tree holds {} extras",
            report.extra.len()
        );
        eprintln!(
            "n64: {} paths whole, {} bytes, {} extra",
            report.checked,
            report.bytes,
            report.extra.len()
        );

        // The Dreamcast tree is whole against its manifests and against the fingerprints the
        // launcher carries: every disc file, IP.BIN, every native file and both manifests, with
        // the research tools' audio folder beside them.
        let dreamcast = reference_tree(
            DREAMCAST_TREE_VARIABLE,
            DREAMCAST_TREE_IN_ASSETS,
            "Dreamcast",
        );
        let committed = Fingerprints::committed();
        let report = verify_tree(&dreamcast, Expansion::Dreamcast, Some(&committed), &job)
            .unwrap_or_else(|error| {
                panic!("the Dreamcast tree is refused: {}", failure_kind(&error))
            });
        assert_whole(&report, "Dreamcast");
        assert_eq!(report.level, VerifyLevel::Fingerprints);
        assert_eq!(report.checked, DREAMCAST_PATHS);
        let research = format!("{AUDIO_WAV}/");
        let outside = report
            .extra
            .iter()
            .filter(|path| *path != AUDIO_WAV && !path.starts_with(&research))
            .count();
        assert!(
            outside == 0,
            "{outside} of the Dreamcast tree's {} extras lie outside {AUDIO_WAV}",
            report.extra.len()
        );
        eprintln!(
            "dreamcast: {} paths whole, {} bytes, {} extra",
            report.checked,
            report.bytes,
            report.extra.len()
        );
    }
}
