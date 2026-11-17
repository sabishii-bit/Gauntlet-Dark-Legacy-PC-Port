//! The Dreamcast tree, as the Dreamcast extractor writes it: every file of the disc's ISO 9660
//! volume under `disc`, at the path the walk gives it; the boot area as `IP.BIN`;
//! `disc-manifest.json`, which records the volume, three of the boot area's fields, each disc file
//! with its digest and each data track; every file SIZES.ROM lists that DISK.ROM holds, restored
//! under `native` from the DISK.ROM the tree holds; and `native-manifest.json`, which records each
//! of them, each file the disc keeps outside DISK.ROM, and SIZES.ROM's digest. [`check_dreamcast`]
//! opens the disc, reads its native table and decides on the whole tree first, writing nothing,
//! so that a staging is made only for a disc that passed; held to an installation's fingerprints,
//! it reads every disc file then as well. [`extract_dreamcast`] then reads the disc again to write
//! its tree there, and reads the sectors of a disc file the check did not read for the first time
//! as it does.

use std::collections::BTreeSet;
use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use gdl_files::{Cancel, DISC_NAMES, Job, Progress, RelPath, Staging};
use gdl_media::{
    DataTrack, DiscFile, GdRom, MAX_SIZES_ROM_LEN, PackedFile, PackedPlace, Sha256Digest,
    disc_verdict, packed_files,
};

use crate::pyjson::{array, digest, text};
use crate::strictness::held;
use crate::tree::{TreeFile, TreePlan, TreeWriter};
use crate::{
    CHECK, ExtractError, ExtractSettings, FingerprintRule, Fingerprints, JsonValue, NativeRule,
    READ_DISC, Strictness, UNPACK, render,
};

/// The format the disc manifest names.
pub const DISC_FORMAT: &str = "dreamcast-legends-disc-v1";

/// What the disc manifest says of the disc files the tree holds.
pub const DISC_SCOPE: &str = "Native disc bytes; not converted or certified runtime-compatible";

/// The format the native manifest names.
pub const NATIVE_FORMAT: &str = "dreamcast-legends-native-v1";

/// What the native manifest says of the native files the tree holds.
pub const NATIVE_SCOPE: &str =
    "Exact DISK.ROM payloads; external records may be absent from the disc";

/// The folder at the tree's top that holds the disc's files.
pub const DISC_FOLDER: &str = "disc";

/// The folder at the tree's top that holds the native files DISK.ROM packs.
pub const NATIVE_FOLDER: &str = "native";

/// The boot area's name, at the tree's top.
pub const IP_BIN: &str = "IP.BIN";

/// The disc manifest's name, at the tree's top.
pub const DISC_MANIFEST: &str = "disc-manifest.json";

/// The native manifest's name, at the tree's top.
pub const NATIVE_MANIFEST: &str = "native-manifest.json";

/// The name, in exactly this letter case, of the table at the disc's root that lists the files
/// DISK.ROM packs.
pub const SIZES_ROM: &str = "SIZES.ROM";

/// The name, in exactly this letter case, of the file at the disc's root that packs the native
/// files.
pub const DISK_ROM: &str = "DISK.ROM";

/// The most bytes of DISK.ROM read at once as a native file is restored from it.
pub const NATIVE_BLOCK_LEN: usize = 1 << 20;

/// What the extraction answers, before the name, for a disc whose root lists no file of its native
/// table by exactly that name.
const TABLE_MISSING: &str = "The disc's root holds no";

/// What the Dreamcast extractor answers for a DISK.ROM that ends before a file SIZES.ROM places in
/// it.
const DISK_TRUNCATED: &str = "Truncated DISK.ROM payload";

/// What `disc-manifest.json` records of a Dreamcast tree.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscManifest {
    /// The name of the disc's ISO 9660 volume.
    pub volume: String,
    /// The product's number the boot area gives.
    pub product: String,
    /// The title the boot area gives, the NULs after it kept.
    pub title: String,
    /// The name of the executable the boot area loads.
    pub executable: String,
    /// Each file of the disc, in the order the walk met them.
    pub files: Vec<DiscFileRow>,
    /// Each data track, in LBA order.
    pub data_tracks: Vec<DataTrack>,
}

/// What the disc manifest records of a file of the disc.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscFileRow {
    /// Its path in the disc's tree, which the extracted tree holds it at under `disc`.
    pub path: RelPath,
    /// The LBA of its extent's first sector.
    pub lba: u32,
    /// How many bytes it holds.
    pub size: u32,
    /// The SHA-256 digest of its bytes, which the tree holds.
    pub sha256: Sha256Digest,
}

/// What `native-manifest.json` records of a Dreamcast tree.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativeManifest {
    /// Each file SIZES.ROM lists that DISK.ROM holds, in SIZES.ROM's order.
    pub files: Vec<NativeFileRow>,
    /// Each file SIZES.ROM lists that the disc keeps outside DISK.ROM, in SIZES.ROM's order.
    pub external: Vec<ExternalRow>,
    /// The SHA-256 digest of SIZES.ROM.
    pub sizes_sha256: Sha256Digest,
}

/// What the native manifest records of a file DISK.ROM holds.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativeFileRow {
    /// Its path, which the tree holds it at under `native`.
    pub path: RelPath,
    /// Where its bytes start in DISK.ROM.
    pub offset: u64,
    /// How many bytes it holds.
    pub size: u32,
    /// The SHA-256 digest of its bytes, which the tree holds.
    pub sha256: Sha256Digest,
}

/// What the native manifest records of a file SIZES.ROM lists that the disc keeps outside
/// DISK.ROM, or not at all: its path and the size its row declares, and no bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExternalRow {
    /// Its path as SIZES.ROM gives it.
    pub path: RelPath,
    /// How many bytes its row says it holds.
    pub declared_size: u32,
}

impl DiscManifest {
    /// The manifest as the Dreamcast extractor writes it, every key in the extractor's order.
    pub fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("format", text(DISC_FORMAT)),
            ("volume", text(&self.volume)),
            ("product", text(&self.product)),
            ("title", text(&self.title)),
            ("executable", text(&self.executable)),
            ("files", array(&self.files, DiscFileRow::to_json)),
            ("data_tracks", array(&self.data_tracks, track_json)),
            ("scope", text(DISC_SCOPE)),
        ])
    }
}

impl DiscFileRow {
    /// What the manifest writes of the file.
    fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("path", text(self.path.as_str())),
            ("lba", JsonValue::Uint(self.lba.into())),
            ("size", JsonValue::Uint(self.size.into())),
            ("sha256", digest(&self.sha256)),
        ])
    }
}

/// What the disc manifest writes of a data track: its file as the sheet spells it, and where the
/// track lies.
fn track_json(track: &DataTrack) -> JsonValue {
    JsonValue::Object(vec![
        ("file", text(&track.file)),
        ("index", JsonValue::Uint(track.index)),
        ("first_lba", JsonValue::Int(track.first_lba)),
        ("sectors", JsonValue::Uint(track.sectors)),
    ])
}

impl NativeManifest {
    /// The manifest as the Dreamcast extractor writes it, every key in the extractor's order.
    pub fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("format", text(NATIVE_FORMAT)),
            ("files", array(&self.files, NativeFileRow::to_json)),
            ("external", array(&self.external, ExternalRow::to_json)),
            ("sizes_sha256", digest(&self.sizes_sha256)),
            ("scope", text(NATIVE_SCOPE)),
        ])
    }
}

impl NativeFileRow {
    /// What the manifest writes of the file.
    fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("path", text(self.path.as_str())),
            ("offset", JsonValue::Uint(self.offset)),
            ("size", JsonValue::Uint(self.size.into())),
            ("sha256", digest(&self.sha256)),
        ])
    }
}

impl ExternalRow {
    /// What the manifest writes of the file the disc keeps outside DISK.ROM.
    fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("path", text(self.path.as_str())),
            ("declared_size", JsonValue::Uint(self.declared_size.into())),
        ])
    }
}

/// A Dreamcast dump read and decided before anything is written: the disc opened under the rules
/// of the strictness it was held to, its identity held to an installation's trust, SIZES.ROM read
/// and its table parsed, every path of the tree planned, and, under a trust that holds
/// fingerprints, every disc file and IP.BIN hashed against them. The disc stays open for the
/// second read, which writes its files.
pub struct CheckedDisc {
    /// The disc, its data tracks' files open.
    disc: GdRom,
    /// Each file SIZES.ROM lists, in its order.
    packed: Vec<PackedFile>,
    /// The SHA-256 digest of SIZES.ROM as the check read it.
    sizes_sha256: Sha256Digest,
    /// The digest of each disc file, in the order the walk met them, as the check read them
    /// against an installation's fingerprints; none when the check read no disc file but
    /// SIZES.ROM.
    first_read: Option<Vec<Sha256Digest>>,
}

impl CheckedDisc {
    /// How many bytes the extraction writes before the native manifest: every disc file's, the
    /// boot area's, the disc manifest's, and every native file's that DISK.ROM holds.
    fn unpacked_len(&self) -> u64 {
        let disc_files: u64 = self
            .disc
            .files()
            .iter()
            .map(|file| u64::from(file.size))
            .sum();
        let native_files: u64 = self
            .packed
            .iter()
            .filter(|file| file.place != PackedPlace::External)
            .map(|file| u64::from(file.size))
            .sum();
        let boot = self.disc.boot().bytes().len() as u64;
        disc_files + boot + self.disc_manifest_len() + native_files
    }

    /// How many bytes the disc manifest takes once written. What it records of a file but its
    /// digest is known before the second read, and every digest is written in as many digits as
    /// any other, so a manifest of stand-in digests is as long.
    fn disc_manifest_len(&self) -> u64 {
        let stand_in = Sha256Digest::of(&[]);
        let rows = self
            .disc
            .files()
            .iter()
            .map(|file| disc_row(file, stand_in))
            .collect();
        render(&disc_manifest(&self.disc, rows).to_json()).len() as u64
    }
}

impl fmt::Debug for CheckedDisc {
    /// The disc as its own description gives it, and how many files its native table lists, in
    /// DISK.ROM and outside it: never a path or a byte of either.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        let external = self
            .packed
            .iter()
            .filter(|file| file.place == PackedPlace::External)
            .count();
        to.debug_struct("CheckedDisc")
            .field("disc", &self.disc)
            .field("native_files", &(self.packed.len() - external))
            .field("external", &external)
            .field("sizes_sha256", &self.sizes_sha256)
            .finish_non_exhaustive()
    }
}

/// What a Dreamcast extraction wrote.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DreamcastExtraction {
    /// What the tree's disc manifest records.
    pub disc: DiscManifest,
    /// What the tree's native manifest records.
    pub native: NativeManifest,
    /// The SHA-256 digest of `disc-manifest.json` as it was written.
    pub disc_manifest_sha256: Sha256Digest,
    /// The SHA-256 digest of `native-manifest.json` as it was written.
    pub native_manifest_sha256: Sha256Digest,
    /// How many bytes the tree holds.
    pub bytes: u64,
}

/// Reads the disc whose sheet is at `sheet` and decides on it, writing nothing, holding it as
/// `mode` says. In the phase [`READ_DISC`], the disc is opened: its sheet, its data tracks, its
/// boot area and its ISO 9660 volume, the job's cancel looked at before each chunk a directory's
/// read takes. Under [`Strictness::Script`] it is opened by the Dreamcast extractor's rules; under
/// [`Strictness::Install`] by an installation's, which hold every sector read to its error
/// detection code, want every track's file there and a data track to start where a pressed disc's
/// high-density area begins, and the boot area's product, version and areas are then held to the
/// trust's, any other disc refused as [`ExtractError::Refused`]. Its root must then list
/// [`SIZES_ROM`] and [`DISK_ROM`] by exactly those names; SIZES.ROM, no larger than
/// [`MAX_SIZES_ROM_LEN`](gdl_media::MAX_SIZES_ROM_LEN), is read, the cancel looked at before each
/// chunk, and its table parsed against DISK.ROM's size; and every path the tree will hold is
/// planned. When the trust holds fingerprints, every disc file and IP.BIN must then have one,
/// every one must be theirs, and IP.BIN and then each disc file, read in the order the walk met
/// them with the cancel looked at before each chunk and the job's progress told the bytes read of
/// them all, must match its own, else the disc is refused as [`ExtractError::Fingerprint`]. The
/// disc is refused by the first rule a part of it breaks, in that order: its format's, its
/// identity's, its native table's, the tree's, which refuses a native folder spelled in two letter
/// cases or a native file that is also the folder above another, and its fingerprints'. Without
/// fingerprints, only SIZES.ROM of the disc's files is read here, as the extractor reads the disc
/// once: a sector of another file that breaks a rule of the disc's format is first met as the
/// extraction writes that file. With them, every disc file is read here, and such a sector is
/// refused before anything is written.
pub fn check_dreamcast(
    sheet: &Path,
    mode: &Strictness<'_>,
    job: &Job<'_>,
) -> Result<CheckedDisc, ExtractError> {
    job.progress.phase(READ_DISC);
    let mut disc = GdRom::open(sheet, mode.disc_rules(), job)?;
    let fingerprints = match mode {
        Strictness::Script => None,
        Strictness::Install(trust) => {
            held(disc_verdict(disc.boot(), &trust.disc))?;
            trust.fingerprints.as_ref()
        }
    };
    let sizes = root_file(&disc, SIZES_ROM)?;
    let disk = root_file(&disc, DISK_ROM)?;
    if sizes.size > MAX_SIZES_ROM_LEN {
        let detail = format!(
            "{SIZES_ROM} holds {} bytes, more than the {MAX_SIZES_ROM_LEN} a table of packed files \
             may hold",
            sizes.size
        );
        return Err(ExtractError::native(NativeRule::TooLarge, detail));
    }
    let mut table = Vec::new();
    let sizes_sha256 = disc.copy(&sizes, &mut table, job.cancel)?;
    let packed = packed_files(&table, u64::from(disk.size))?;
    plan_tree(&disc, &packed)?;
    let first_read = match fingerprints {
        Some(fingerprints) => Some(read_against(&mut disc, fingerprints, job)?),
        None => None,
    };
    Ok(CheckedDisc {
        disc,
        packed,
        sizes_sha256,
        first_read,
    })
}

/// Holds IP.BIN and every disc file of `disc` to `fingerprints`, finding each by its path in the
/// tree, and gives each disc file's digest, in the order the walk met them. Every disc file and
/// IP.BIN must have a fingerprint, else the first without one is refused as
/// [`FingerprintRule::Missing`]; every fingerprint must be one of theirs, else the first of the
/// others, by its key, is refused as [`FingerprintRule::Unexpected`]; and IP.BIN and then each disc
/// file, read whole as the extraction reads it with `job`'s cancel looked at before each chunk,
/// must match its own, else it is refused as [`FingerprintRule::Digest`]. The job's progress is
/// told, after each chunk, how many bytes of all the disc files have been read.
fn read_against(
    disc: &mut GdRom,
    fingerprints: &Fingerprints,
    job: &Job<'_>,
) -> Result<Vec<Sha256Digest>, ExtractError> {
    let files = disc.files().to_vec();
    let paths: Vec<RelPath> = files.iter().map(|file| disc_path(&file.path)).collect();
    let fingerprint_of = |path: &str| {
        let digest = fingerprints.get(path);
        digest.ok_or_else(|| ExtractError::fingerprint(FingerprintRule::Missing, path))
    };
    let expected = paths
        .iter()
        .map(|path| fingerprint_of(path.as_str()))
        .collect::<Result<Vec<_>, _>>()?;
    let boot = fingerprint_of(IP_BIN)?;
    let covered: BTreeSet<Sha256Digest> = paths
        .iter()
        .map(|path| Fingerprints::path_key(path.as_str()))
        .chain([Fingerprints::path_key(IP_BIN)])
        .collect();
    if let Some(key) = fingerprints.keys().find(|key| !covered.contains(key)) {
        return Err(ExtractError::fingerprint(
            FingerprintRule::Unexpected,
            key.hex(),
        ));
    }
    if Sha256Digest::of(disc.boot().bytes()) != boot {
        return Err(ExtractError::fingerprint(FingerprintRule::Digest, IP_BIN));
    }
    let total = files.iter().map(|file| u64::from(file.size)).sum();
    let mut done = 0;
    let mut digests = Vec::with_capacity(files.len());
    for ((file, path), expected) in files.iter().zip(&paths).zip(expected) {
        let mut read = ReadProgress {
            done: &mut done,
            total,
            item: path,
            progress: job.progress,
        };
        let digest = disc.copy(file, &mut read, job.cancel)?;
        if digest != expected {
            return Err(ExtractError::fingerprint(
                FingerprintRule::Digest,
                path.as_str(),
            ));
        }
        digests.push(digest);
    }
    Ok(digests)
}

/// Where a disc file read against its fingerprint goes: nowhere, since only its digest is kept,
/// but the job's progress is told of each chunk, as the bytes read of all the disc files so far.
struct ReadProgress<'a> {
    /// How many bytes of the disc files have been read.
    done: &'a mut u64,
    /// How many bytes the disc files hold together.
    total: u64,
    /// The path of the file being read, in the tree.
    item: &'a RelPath,
    /// Where the progress goes.
    progress: &'a dyn Progress,
}

impl Write for ReadProgress<'_> {
    /// Takes a chunk of the file whole, and tells the progress of it.
    fn write(&mut self, chunk: &[u8]) -> io::Result<usize> {
        if !chunk.is_empty() {
            *self.done += chunk.len() as u64;
            self.progress
                .advance(*self.done, self.total, Some(self.item));
        }
        Ok(chunk.len())
    }

    /// Nothing is held back.
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Writes the tree of `checked` into the staging `into`, under `root` there, reading the disc a
/// second time to do so. In the phase [`UNPACK`]: each disc file in the order the walk met them,
/// under `disc`, as it is read; then `IP.BIN`; then `disc-manifest.json`; then, in SIZES.ROM's
/// order, each native file DISK.ROM holds, under `native`, read back in blocks of at most
/// [`NATIVE_BLOCK_LEN`] bytes from the DISK.ROM just written, which is closed once they are. In
/// the phase [`CHECK`], `native-manifest.json`, last. Each file is made new and written in blocks
/// of at most [`BLOCK_LEN`](gdl_files::BLOCK_LEN) bytes, the job's cancel looked at before each
/// and its progress told after. Each disc file's sectors are held to the rules the disc was
/// checked by as they are read, so a disc file whose sectors break one, which the check did not
/// read, refuses the dump here. SIZES.ROM must give the bytes it gave the check, and every disc
/// file the check read against fingerprints the bytes it gave then, else the dump changed while it
/// was read; and the DISK.ROM read back must hold every file the table places in it. On a failure,
/// what was written stays in the staging, for its drop to remove.
pub fn extract_dreamcast(
    checked: &mut CheckedDisc,
    into: &Staging,
    root: &RelPath,
    job: &Job<'_>,
) -> Result<DreamcastExtraction, ExtractError> {
    extract_dreamcast_with(checked, into, root, &ExtractSettings::SHIPPED, job)
}

/// [`extract_dreamcast`], writing in `settings`' blocks.
pub(crate) fn extract_dreamcast_with(
    checked: &mut CheckedDisc,
    into: &Staging,
    root: &RelPath,
    settings: &ExtractSettings,
    job: &Job<'_>,
) -> Result<DreamcastExtraction, ExtractError> {
    job.progress.phase(UNPACK);
    let unpacked = checked.unpacked_len();
    let mut writer = TreeWriter::new(into, root, job, unpacked, settings.block_len);
    let files = checked.disc.files().to_vec();
    let mut rows = Vec::with_capacity(files.len());
    for (index, file) in files.iter().enumerate() {
        let path = disc_path(&file.path);
        let disc = &mut checked.disc;
        let sha256 = writer.write_with(&path, |to| {
            disc.copy(file, to, job.cancel)?;
            Ok(())
        })?;
        // The native files are restored by the table the check read, so the tree must hold that
        // very table; and it must hold every file the check held to its fingerprint as it was
        // then.
        let first = checked.first_read.as_ref().and_then(|read| read.get(index));
        let table_changed = file.path.as_str() == SIZES_ROM && sha256 != checked.sizes_sha256;
        if table_changed || first.is_some_and(|first| *first != sha256) {
            return Err(ExtractError::Changed {
                path: path.as_str().into(),
            });
        }
        rows.push(disc_row(file, sha256));
    }
    writer.write_bytes(&own_path(IP_BIN), checked.disc.boot().bytes())?;
    let disc = disc_manifest(&checked.disc, rows);
    let document = render(&disc.to_json());
    let disc_manifest_sha256 = writer.write_bytes(&own_path(DISC_MANIFEST), &document)?;
    let (native_files, external) = restore_native(checked, &mut writer, job.cancel)?;

    job.progress.phase(CHECK);
    let native = NativeManifest {
        files: native_files,
        external,
        sizes_sha256: checked.sizes_sha256,
    };
    let document = render(&native.to_json());
    let checked_len = document.len() as u64;
    let mut writer = TreeWriter::new(into, root, job, checked_len, settings.block_len);
    let native_manifest_sha256 = writer.write_bytes(&own_path(NATIVE_MANIFEST), &document)?;
    Ok(DreamcastExtraction {
        disc,
        native,
        disc_manifest_sha256,
        native_manifest_sha256,
        bytes: unpacked + checked_len,
    })
}

/// The file `disc`'s root lists by exactly the name `name`, a file of its native table: refused as
/// [`NativeRule::TableMissing`] when there is none, whatever the root lists in another letter
/// case.
fn root_file(disc: &GdRom, name: &str) -> Result<DiscFile, ExtractError> {
    let mut files = disc.files().iter();
    files
        .find(|file| file.path.as_str() == name)
        .cloned()
        .ok_or_else(|| {
            ExtractError::native(NativeRule::TableMissing, format!("{TABLE_MISSING} {name}"))
        })
}

/// Plans every path the tree of `disc` will hold, whose native table lists `packed`: each disc
/// file under `disc`, `IP.BIN`, the disc manifest, each native file DISK.ROM holds under `native`
/// and the native manifest, refused by the first rule of the tree they break.
fn plan_tree(disc: &GdRom, packed: &[PackedFile]) -> Result<(), ExtractError> {
    let mut plan = TreePlan::new();
    for file in disc.files() {
        plan.add(&disc_path(&file.path))?;
    }
    plan.add(&own_path(IP_BIN))?;
    plan.add(&own_path(DISC_MANIFEST))?;
    let held = packed
        .iter()
        .filter(|file| file.place != PackedPlace::External);
    for file in held {
        plan.add(&native_path(&file.path))?;
    }
    plan.add(&own_path(NATIVE_MANIFEST))?;
    plan.finish()
}

/// Restores, in SIZES.ROM's order, every file it lists that DISK.ROM holds, read back from the
/// DISK.ROM `writer` has written, as the Dreamcast extractor restores them, and lists every file
/// the disc keeps outside DISK.ROM, which is not written. `cancel` is looked at before each block
/// read back. The DISK.ROM read back is closed before this returns.
fn restore_native(
    checked: &CheckedDisc,
    writer: &mut TreeWriter<'_>,
    cancel: &Cancel,
) -> Result<(Vec<NativeFileRow>, Vec<ExternalRow>), ExtractError> {
    let disk_rom = disc_path(&own_path(DISK_ROM));
    let mut disk = StagedDisk {
        file: writer.read_staged(&disk_rom)?,
        path: writer.staged_path(&disk_rom),
        block: vec![0; NATIVE_BLOCK_LEN],
    };
    let (mut files, mut external) = (Vec::new(), Vec::new());
    for packed in &checked.packed {
        let PackedPlace::Disk { offset } = packed.place else {
            external.push(ExternalRow {
                path: packed.path.clone(),
                declared_size: packed.size,
            });
            continue;
        };
        let sha256 = writer.write_with(&native_path(&packed.path), |to| {
            disk.restore(offset, packed.size, to, cancel)
        })?;
        files.push(NativeFileRow {
            path: packed.path.clone(),
            offset,
            size: packed.size,
            sha256,
        });
    }
    Ok((files, external))
}

/// The DISK.ROM the tree holds, open for reading back the native files it packs, and the block
/// they are read back through.
struct StagedDisk {
    /// The file, open for reading.
    file: File,
    /// Where it lies in the staging, which a failure to read it names.
    path: PathBuf,
    /// What each read fills, [`NATIVE_BLOCK_LEN`] bytes of it at most.
    block: Vec<u8>,
}

impl StagedDisk {
    /// Writes into `to` the `size` bytes of DISK.ROM from `offset`, read in blocks of at most
    /// [`NATIVE_BLOCK_LEN`] bytes, `cancel` looked at before each: refused as
    /// [`NativeRule::DiskTruncated`] when the file ends first.
    fn restore(
        &mut self,
        offset: u64,
        size: u32,
        to: &mut TreeFile<'_>,
        cancel: &Cancel,
    ) -> Result<(), ExtractError> {
        let path = &self.path;
        let failed = |source: io::Error| ExtractError::Io {
            path: path.clone(),
            source,
        };
        self.file.seek(SeekFrom::Start(offset)).map_err(failed)?;
        let mut left = u64::from(size);
        while left > 0 {
            cancel.check()?;
            let block = &mut self.block[..left.min(NATIVE_BLOCK_LEN as u64) as usize];
            self.file.read_exact(block).map_err(|error| {
                if error.kind() == io::ErrorKind::UnexpectedEof {
                    ExtractError::native(NativeRule::DiskTruncated, DISK_TRUNCATED)
                } else {
                    failed(error)
                }
            })?;
            to.put(block)?;
            left -= block.len() as u64;
        }
        Ok(())
    }
}

/// What the disc manifest records of `disc`, whose files' rows are `files`.
fn disc_manifest(disc: &GdRom, files: Vec<DiscFileRow>) -> DiscManifest {
    let boot = disc.boot();
    DiscManifest {
        volume: disc.volume().to_owned(),
        product: boot.product().to_owned(),
        title: boot.title().to_owned(),
        executable: boot.executable().to_owned(),
        files,
        data_tracks: disc.data_tracks().to_vec(),
    }
}

/// What the disc manifest records of `file`, whose bytes have the digest `sha256`.
fn disc_row(file: &DiscFile, sha256: Sha256Digest) -> DiscFileRow {
    DiscFileRow {
        path: file.path.clone(),
        lba: file.lba,
        size: file.size,
        sha256,
    }
}

/// The extractor's own name `name`, of a file or a folder at the tree's top, as a path of the
/// tree.
fn own_path(name: &str) -> RelPath {
    RelPath::parse(name, &DISC_NAMES).expect("the extractor's own names are names a tree may hold")
}

/// Where the disc's file at `path` lies in the tree: under [`DISC_FOLDER`].
fn disc_path(path: &RelPath) -> RelPath {
    own_path(DISC_FOLDER).join(path)
}

/// Where the native file at `path` lies in the tree: under [`NATIVE_FOLDER`].
fn native_path(path: &RelPath) -> RelPath {
    own_path(NATIVE_FOLDER).join(path)
}

#[cfg(test)]
mod tests {
    use std::fs::{self, OpenOptions};
    use std::io;
    use std::path::PathBuf;

    use gdl_files::{Cancel, DISC_NAMES, Job, PhaseId, Progress, RelPath, Staging};
    use gdl_fixtures::{DiscSet, Edc, raw_sector, sha256_hex, sizes_rom, unhex};
    use gdl_media::{BOOT_LBA, DataTrack, GdRom, MAX_SIZES_ROM_LEN, SCRIPT_RULES, Sha256Digest};
    use serde_json::Value;

    use super::{
        DISC_MANIFEST, DISK_ROM, DiscFileRow, DreamcastExtraction, IP_BIN, NATIVE_MANIFEST,
        SIZES_ROM, check_dreamcast, disc_manifest, disc_path, disc_row, extract_dreamcast,
        extract_dreamcast_with, own_path,
    };
    use crate::vectors::{
        CancelAt, ChangedAtUnpack, DISC_SWEEP, Folders, Listed, PanicAt, Recorded, STAGING_PREFIX,
        Told, array, assert_injected_panic, bytes_under, disc_file_lba, disc_trust,
        dreamcast_golden, dreamcast_root, dreamcast_vectors, full_golden, installable_disc,
        items_of, listed, phases_of, quiet, sector_place, stage_dreamcast, text, tree_listing,
        written_after_each_advance,
    };
    use crate::{CHECK, ExtractError, ExtractSettings, READ_DISC, Strictness, UNPACK, render};

    /// The flag of a directory record whose file goes on in the next record's extent, which the
    /// extractor refuses.
    const MULTIEXTENT: u8 = 0x80;

    /// An LBA between the cue sets' data tracks, where the audio track lies and no data track.
    const AUDIO_GAP_LBA: u32 = 45_050;

    /// A byte outside ASCII.
    const NON_ASCII: u8 = 0xE9;

    /// What a disc's own folder holds of a table of packed files whose folder is spelled in two
    /// letter cases: `Font/a`, DISK.ROM's five bytes, and `font/b`, empty, after them.
    const TWO_SPELLINGS: [(u32, i32, [i16; 4]); 2] =
        [(5, 0, [0, 5, -1, 0]), (0, 5, [7, 12, -1, 0])];

    /// The pool of names those rows' components point into.
    const TWO_SPELLINGS_POOL: &[u8] = b"Font\0a\0font\0b\0";

    /// The file of the full set's that holds its first data track, which holds the boot area and
    /// the root directory.
    fn first_track(golden: &Value) -> String {
        text(&array(&golden["data_tracks"])[0]["file"]).to_owned()
    }

    /// The full set with the identifier `from`, which its first data track holds once, written as
    /// `to`, of the same length: a name its root directory lists, changed where it is.
    fn renamed(from: &[u8], to: &[u8]) -> DiscSet {
        assert_eq!(from.len(), to.len());
        let track = first_track(&full_golden(&dreamcast_vectors()));
        let set = DiscSet::full(Edc::Zero);
        let held = set.file(&track).contents();
        let places: Vec<usize> = held
            .windows(from.len())
            .enumerate()
            .filter(|(_, window)| *window == from)
            .map(|(at, _)| at)
            .collect();
        assert_eq!(places.len(), 1, "{:?} is held once", from.escape_ascii());
        set.patched(&track, places[0] as u64, to)
    }

    /// The full set with SIZES.ROM holding `table`: its root's record giving the table's length,
    /// and the table in SIZES.ROM's one sector.
    fn with_sizes_rom(table: &[u8]) -> DiscSet {
        let golden = full_golden(&dreamcast_vectors());
        let lba = disc_file_lba(&golden, SIZES_ROM);
        let (track, place) = sector_place(&golden, lba);
        let len = u32::try_from(table.len()).unwrap();
        let set = DiscSet::full_with_sizes_rom_len(Edc::Zero, len);
        let sector = raw_sector(u32::try_from(lba).unwrap(), table, Edc::Zero);
        set.patched(&track, place, &sector)
    }

    /// The full set with a byte outside ASCII in its boot area's title, right after the game's
    /// name.
    fn with_title_outside_ascii() -> DiscSet {
        let golden = full_golden(&dreamcast_vectors());
        let (track, boot) = sector_place(&golden, u64::from(BOOT_LBA));
        let within =
            gdl_media::USER_DATA.start + gdl_media::TITLE.start + gdl_media::TITLE_PREFIX.len();
        DiscSet::full(Edc::Zero).patched(&track, boot + within as u64, &[NON_ASCII])
    }

    /// What a test's extraction of a disc wrote: its tree, its two manifests' bytes, and what the
    /// extraction says it wrote.
    struct Written {
        listing: Vec<Listed>,
        disc_manifest: Vec<u8>,
        native_manifest: Vec<u8>,
        extraction: DreamcastExtraction,
    }

    /// Extracts the disc `set` as a player's extraction does: checked, and only then written into
    /// a new staging. Checks that nothing outside the staging changed, the dump included, and that
    /// the staging goes once dropped.
    fn extract_set(set: &DiscSet) -> Written {
        let folders = Folders::new();
        let sheet = folders.disc(set);
        let before = folders.before();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let mut checked = check_dreamcast(&sheet, &Strictness::Script, &job).unwrap();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let extraction =
            extract_dreamcast(&mut checked, &staging, &dreamcast_root(), &job).unwrap();
        let tree = dreamcast_root().path_under(staging.path());
        let written = Written {
            listing: tree_listing(&tree),
            disc_manifest: fs::read(tree.join(DISC_MANIFEST)).unwrap(),
            native_manifest: fs::read(tree.join(NATIVE_MANIFEST)).unwrap(),
            extraction,
        };
        folders.assert_outside_unchanged(&before);
        drop(staging);
        drop(checked);
        folders.assert_staging_gone(&before);
        written
    }

    /// A refusal of the vectors' tables, as a disc's SIZES.ROM brings it to the check: its case's
    /// name, the table, the rule it is refused by and, where a name decides, the rule of names it
    /// breaks.
    struct RefusedTable {
        case: String,
        table: Vec<u8>,
        rule: String,
        name_rule: Option<String>,
    }

    /// Every table of the vectors' cases that the launcher refuses, each read against a DISK.ROM
    /// of `disk_len` bytes, as the full set's is.
    fn refused_tables(vectors: &Value, disk_len: u64) -> Vec<RefusedTable> {
        let sizes = vectors["sizes"].as_object().expect("the table cases");
        let named = sizes
            .iter()
            .filter(|(name, _)| *name != "extras")
            .map(|(name, case)| (name.as_str(), case));
        let extras = array(&sizes["extras"])
            .iter()
            .map(|case| (text(&case["name"]), case));
        let mut refused = Vec::new();
        for (name, case) in named.chain(extras) {
            let Some(rule) = case["rule"].as_str() else {
                continue;
            };
            assert_eq!(case["diskLen"], disk_len, "{name}");
            refused.push(RefusedTable {
                case: name.to_owned(),
                table: unhex(&case["hex"]),
                rule: rule.to_owned(),
                name_rule: case["nameRule"].as_str().map(str::to_owned),
            });
        }
        refused
    }

    #[test]
    fn the_full_set_writes_the_golden_trees() {
        let vectors = dreamcast_vectors();
        let full = &vectors["sets"]["full"];
        // The extractor takes the set, writing both its trees, and so does the launcher.
        assert!(full["python"].is_null() && full["rule"].is_null(), "{full}");
        let written = extract_set(&DiscSet::full(Edc::Zero));
        let disc_golden = dreamcast_golden(&full["goldens"]["disc-manifest.json"]);
        let native_golden = dreamcast_golden(&full["goldens"]["native-manifest.json"]);
        assert!(
            written.disc_manifest == disc_golden,
            "the disc manifest is not the golden"
        );
        assert!(
            written.native_manifest == native_golden,
            "the native manifest is not the golden"
        );
        assert_eq!(written.listing, listed(&full["tree"]));
        // DISK.ROM is the disc's file exactly, as the extractor's own case reads it, and so is
        // the native file it packs whole.
        let digest_of = |path: &str| {
            let row = written.listing.iter().find(|(listed, _, _)| listed == path);
            row.map(|(_, _, digest)| digest.as_str()).unwrap()
        };
        assert_eq!(digest_of("disc/DISK.ROM"), sha256_hex(b"hello"));
        assert_eq!(digest_of("native/levels/a.wad"), sha256_hex(b"hello"));

        // What the extraction says it wrote is what it wrote.
        let extraction = &written.extraction;
        assert_eq!(
            extraction.disc_manifest_sha256,
            Sha256Digest::of(disc_golden)
        );
        assert_eq!(
            extraction.native_manifest_sha256,
            Sha256Digest::of(native_golden)
        );
        let bytes: u64 = written.listing.iter().map(|(_, size, _)| size).sum();
        assert_eq!(extraction.bytes, bytes);
        assert!(render(&extraction.disc.to_json()) == disc_golden);
        assert!(render(&extraction.native.to_json()) == native_golden);
    }

    #[test]
    fn a_checked_disc_writes_the_same_tree_each_time() {
        let vectors = dreamcast_vectors();
        let golden = listed(&vectors["sets"]["full"]["tree"]);
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        // Under the extractor's rules, and under an installation's, which holds every disc file
        // read again to what it read against the fingerprints.
        let trust = disc_trust();
        let runs = [
            (Strictness::Script, DiscSet::full(Edc::Zero)),
            (Strictness::Install(&trust), installable_disc()),
        ];
        for (mode, set) in &runs {
            let sheet = folders.disc(set);
            let mut checked = check_dreamcast(&sheet, mode, &job).unwrap();
            // The disc stays open between the extractions, each of which reads it again.
            let mut extractions = Vec::new();
            for _ in 0..2 {
                let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
                let root = dreamcast_root();
                extractions.push(extract_dreamcast(&mut checked, &staging, &root, &job).unwrap());
                assert_eq!(tree_listing(&root.path_under(staging.path())), golden);
            }
            assert_eq!(extractions[0], extractions[1]);
            // The checked disc describes itself by counts, never by a path of the disc.
            let described = format!("{checked:?}");
            assert!(
                described.contains("native_files: 2, external: 1"),
                "{described}"
            );
            assert!(!described.contains("X.STR"), "{described}");
        }
    }

    #[test]
    fn the_small_sets_disc_manifest_renders_as_the_extractor_wrote_it() {
        // The launcher refuses the small set's tree, whose disc holds no native table, but what
        // its disc manifest would record is the extractor's to the byte: a title followed by its
        // NULs, written as escapes.
        let vectors = dreamcast_vectors();
        let small = &vectors["sets"]["small"];
        let golden = dreamcast_golden(&small["goldens"]["disc-manifest.json"]);
        let folders = Folders::new();
        let sheet = folders.disc(&DiscSet::small(Edc::Zero));
        let cancel = Cancel::default();
        let mut disc = GdRom::open(&sheet, &SCRIPT_RULES, &quiet(&cancel)).unwrap();
        let files = disc.files().to_vec();
        let rows: Vec<DiscFileRow> = files
            .iter()
            .map(|file| {
                let sha256 = disc.copy(file, &mut io::sink(), &cancel).unwrap();
                disc_row(file, sha256)
            })
            .collect();
        let manifest = disc_manifest(&disc, rows);
        assert!(render(&manifest.to_json()) == golden);
        assert_eq!(
            manifest.title,
            format!("GAUNTLET LEGENDS{}", "\0".repeat(112))
        );
        // The rest of the tree the extractor writes of it: the boot area, and the disc's file.
        let tree = listed(&small["tree"]);
        let boot = (IP_BIN.to_owned(), 32_768, sha256_hex(disc.boot().bytes()));
        assert!(tree.contains(&boot), "{tree:?}");
        let file = &manifest.files[0];
        let row = (
            disc_path(&file.path).as_str().to_owned(),
            u64::from(file.size),
            file.sha256.hex(),
        );
        assert!(tree.contains(&row), "{tree:?}");
    }

    #[test]
    fn a_gdi_set_writes_the_cue_sets_files() {
        let vectors = dreamcast_vectors();
        let gdi_case = &vectors["sets"]["fullGdi"];
        assert!(gdi_case["rule"].is_null(), "{gdi_case}");
        let cue = extract_set(&DiscSet::full(Edc::Zero));
        let gdi = extract_set(&DiscSet::full_gdi(Edc::Zero));
        // Every file is the cue set's, but the disc manifest.
        let others = |written: &Written| -> Vec<Listed> {
            let rows = written.listing.iter();
            rows.filter(|(path, _, _)| path != DISC_MANIFEST)
                .cloned()
                .collect()
        };
        assert_eq!(others(&gdi), others(&cue));
        assert_eq!(gdi.extraction.native, cue.extraction.native);
        // The disc manifest is the cue set's to the byte but for its data tracks, which are the
        // GDI's own, each from its file's first sector.
        let tracks: Vec<DataTrack> = array(&gdi_case["dataTracks"])
            .iter()
            .map(|row| DataTrack {
                file: text(&row[0]).to_owned(),
                index: row[1].as_u64().unwrap(),
                first_lba: row[2].as_i64().unwrap(),
                sectors: row[3].as_u64().unwrap(),
            })
            .collect();
        assert_eq!(gdi.extraction.disc.data_tracks, tracks);
        let mut expected = cue.extraction.disc.clone();
        expected.data_tracks = tracks;
        assert!(
            gdi.disc_manifest == render(&expected.to_json()),
            "the GDI set's disc manifest is not the cue set's with its own tracks"
        );
    }

    #[test]
    fn native_files_follow_sizes_rom_and_externals_stay_listed() {
        let vectors = dreamcast_vectors();
        let case = &vectors["sizes"]["unpack"];
        assert!(case["python"].is_null() && case["rule"].is_null(), "{case}");
        // The extractor's case restores its table against a DISK.ROM of five bytes, which the
        // full set's DISK.ROM holds.
        let disk = unhex(&case["disk"]);
        assert_eq!(case["diskLen"], disk.len());
        let held = (
            "disc/DISK.ROM".to_owned(),
            disk.len() as u64,
            sha256_hex(&disk),
        );
        assert!(listed(&vectors["sets"]["full"]["tree"]).contains(&held));

        let table = unhex(&case["hex"]);
        let written = extract_set(&with_sizes_rom(&table));
        assert!(written.native_manifest == dreamcast_golden(&case["golden"]));
        // The file DISK.ROM holds is restored from it; the one the disc keeps outside it is
        // listed with the size its row declares, and not written.
        let native: Vec<Listed> = written
            .listing
            .iter()
            .filter_map(|(path, size, digest)| {
                let path = path.strip_prefix("native/")?;
                Some((path.to_owned(), *size, digest.clone()))
            })
            .collect();
        assert_eq!(native, listed(&case["native"]));
        let external: Vec<(String, u64)> = array(&case["files"])
            .iter()
            .filter(|row| row[1] == -1)
            .map(|row| (text(&row[0]).to_owned(), row[2].as_u64().unwrap()))
            .collect();
        let recorded: Vec<(String, u64)> = written
            .extraction
            .native
            .external
            .iter()
            .map(|row| (row.path.as_str().to_owned(), u64::from(row.declared_size)))
            .collect();
        assert_eq!(recorded, external);
        assert!(!external.is_empty());
        for (path, _) in &external {
            let path = format!("native/{path}");
            assert!(written.listing.iter().all(|(listed, _, _)| *listed != path));
        }
        let sizes_sha256 = written.extraction.native.sizes_sha256;
        assert_eq!(sizes_sha256, Sha256Digest::of(&table));
    }

    #[test]
    fn every_refusal_comes_before_the_staging() {
        let vectors = dreamcast_vectors();
        let folders = Folders::new();
        let small = &vectors["sets"]["small"];
        // The extractor writes the small set's tree, and its program then fails restoring the
        // native files, which the disc does not hold; the launcher refuses the set before writing
        // anything, though its disc reads whole.
        assert!(small["python"].is_null(), "{small}");
        assert_eq!(small["deviation"], 12);
        let small_sheet = folders.disc(&DiscSet::small(Edc::Zero));
        GdRom::open(&small_sheet, &SCRIPT_RULES, &quiet(&Cancel::default())).unwrap();

        // The small set's one file, which its root directory lists by this identifier.
        let record = &array(&vectors["records"])[0];
        let identifier = text(&record["name"]).as_bytes();
        let lba = u32::try_from(record["lba"].as_u64().unwrap()).unwrap();
        let missing = |name: &str| Some(format!("The disc's root holds no {name}"));
        let two_spellings = sizes_rom(&TWO_SPELLINGS, TWO_SPELLINGS_POOL);
        let mut cases = vec![
            (
                "the small set".to_owned(),
                DiscSet::small(Edc::Zero),
                text(&small["rule"]).to_owned(),
                missing(SIZES_ROM),
            ),
            (
                "no SIZES.ROM".to_owned(),
                renamed(b"SIZES.ROM;1", b"SIZES.ROX;1"),
                "nativeTableMissing".to_owned(),
                missing(SIZES_ROM),
            ),
            (
                "no DISK.ROM".to_owned(),
                renamed(b"DISK.ROM;1", b"DISK.ROX;1"),
                "nativeTableMissing".to_owned(),
                missing(DISK_ROM),
            ),
            (
                "SIZES.ROM in lower case".to_owned(),
                renamed(b"SIZES.ROM;1", b"sizes.rom;1"),
                "nativeTableMissing".to_owned(),
                missing(SIZES_ROM),
            ),
            (
                "DISK.ROM in lower case".to_owned(),
                renamed(b"DISK.ROM;1", b"disk.rom;1"),
                "nativeTableMissing".to_owned(),
                missing(DISK_ROM),
            ),
            (
                "a native folder spelled two ways".to_owned(),
                with_sizes_rom(&two_spellings),
                "treeFolderCase".to_owned(),
                Some("a folder is spelled native/Font and native/font".to_owned()),
            ),
            (
                "a title outside ASCII".to_owned(),
                with_title_outside_ascii(),
                "textField".to_owned(),
                None,
            ),
            (
                "a multiextent record".to_owned(),
                DiscSet::small_with(identifier, lba, MULTIEXTENT, Edc::Zero),
                "recordUnsupported".to_owned(),
                None,
            ),
            (
                "an extent in the audio gap".to_owned(),
                DiscSet::small_with(identifier, AUDIO_GAP_LBA, 0, Edc::Zero),
                "extentOutside".to_owned(),
                None,
            ),
        ];
        // Every table the vectors refuse, as the full set's SIZES.ROM: the rule of names a name
        // breaks closes the words for it.
        let golden = full_golden(&vectors);
        let disk = array(&golden["files"])
            .iter()
            .find(|row| row["path"] == DISK_ROM)
            .unwrap();
        let tables = refused_tables(&vectors, disk["size"].as_u64().unwrap());
        assert!(tables.len() >= 10, "{} tables", tables.len());
        for table in tables {
            let ending = table.name_rule.map(|rule| format!("({rule})"));
            cases.push((table.case, with_sizes_rom(&table.table), table.rule, ending));
        }
        for (case, set, rule, ending) in cases {
            let error = folders.refused_disc(&set, &Strictness::Script);
            assert_eq!(error.rule(), rule, "{case}: {error}");
            if let Some(ending) = ending {
                let detail = error.to_string();
                assert!(detail.ends_with(&ending), "{case}: {detail}");
            }
        }
    }

    #[test]
    fn a_sizes_rom_over_16_mib_is_refused_before_reading() {
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        let declared = MAX_SIZES_ROM_LEN + 1;
        // SIZES.ROM's first sector holds no header, so that a check that read a sector of it
        // would be refused by the disc's format instead; the extent lies on the disc all the same.
        let (track, place) = sector_place(&golden, disc_file_lba(&golden, SIZES_ROM));
        let set = DiscSet::full_with_sizes_rom_len(Edc::Zero, declared);
        let set = set.patched(&track, place, &[0; gdl_media::HEADER_LEN]);
        let folders = Folders::new();
        let error = folders.refused_disc(&set, &Strictness::Script);
        assert_eq!(error.rule(), "sizesTooLarge", "{error}");
        let detail = error.to_string();
        assert!(detail.contains(&declared.to_string()), "{detail}");
    }

    #[test]
    fn a_fault_in_a_files_own_sectors_is_refused_within_the_staging() {
        // The full set with the last sector of the first file the walk meets, which the check
        // does not read under the extractor's rules, under a header that gives another address.
        // The check takes the disc; the extraction refuses it as it reads that file, with nothing
        // outside the staging changed and nothing left of the run once the staging is dropped.
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        let first = &array(&golden["files"])[0];
        let sectors = first["size"]
            .as_u64()
            .unwrap()
            .div_ceil(gdl_fixtures::SECTOR as u64);
        assert!(sectors > 1, "{first}");
        let lba = first["lba"].as_u64().unwrap() + sectors - 1;
        let (track, place) = sector_place(&golden, lba);
        let elsewhere = u32::try_from(lba + sectors).unwrap();
        let header = &raw_sector(elsewhere, &[], Edc::Zero)[..gdl_media::HEADER_LEN];
        let set = DiscSet::full(Edc::Zero).patched(&track, place, header);
        let folders = Folders::new();
        let sheet = folders.disc(&set);
        let before = folders.before();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let mut checked = check_dreamcast(&sheet, &Strictness::Script, &job).unwrap();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let root = dreamcast_root();
        let error = extract_dreamcast(&mut checked, &staging, &root, &job).unwrap_err();
        assert_eq!(error.rule(), "sectorAddress", "{error:?}");
        // The file was made, and nothing of it written.
        let file = disc_path(&RelPath::parse(text(&first["path"]), &DISC_NAMES).unwrap());
        let staged = root.join(&file).path_under(staging.path());
        assert_eq!(fs::metadata(&staged).unwrap().len(), 0);
        folders.assert_outside_unchanged(&before);
        drop(staging);
        drop(checked);
        folders.assert_staging_gone(&before);
    }

    #[test]
    fn a_cancel_or_panic_at_any_step_leaves_nothing() {
        let vectors = dreamcast_vectors();
        let golden = listed(&vectors["sets"]["full"]["tree"]);
        let folders = Folders::new();
        let cancel = Cancel::default();
        // The extractor's rules on the set as its tests build it, and an installation's on the
        // set as a pressed disc's dump holds it, the check's read against the fingerprints
        // included.
        let trust = disc_trust();
        let runs = [
            (Strictness::Script, DiscSet::full(Edc::Zero)),
            (Strictness::Install(&trust), installable_disc()),
        ];
        for (mode, set) in &runs {
            let sheet = folders.disc(set);
            let name = mode.as_str();

            // A whole run, in blocks short enough that the boot area alone spans dozens, counts
            // its steps, its phases and its advances, and the bytes written by each advance.
            let recorded = Recorded::default();
            let job = Job {
                cancel: &cancel,
                progress: &recorded,
            };
            let (staging, _) =
                stage_dreamcast(&sheet, &folders.parent, mode, &DISC_SWEEP, &job).unwrap();
            drop(staging);
            let told = recorded.told();
            let written = written_after_each_advance(&told);
            let advances = written.len() - 1;
            let whole: u64 = golden.iter().map(|(_, size, _)| size).sum();
            assert_eq!(written[advances], whole, "{name}");
            assert!(advances > golden.len() * 2, "{name}: {advances} advances");
            // The advances the check tells as it reads the disc's files, before anything is
            // written: only an installation's, which holds them to their fingerprints.
            let checking = told
                .iter()
                .take_while(|told| **told != Told::Phase(UNPACK.0))
                .filter(|told| matches!(told, Told::Advance { .. }))
                .count();
            assert_eq!(
                checking > 0,
                matches!(mode, Strictness::Install(_)),
                "{name}"
            );

            // Asked to stop before the job begins, or after any of the check's advances but its
            // last, the check stops, and nothing is made. Asked after any later advance but the
            // last, the extraction stops before its next block, with only what it wrote in the
            // staging and nothing outside it changed; asked after the last, it is done, and the
            // tree is whole.
            for (at, &written_before) in written.iter().enumerate() {
                let before = folders.before();
                let cancel = Cancel::default();
                let progress = CancelAt::new(&cancel, at);
                let job = Job {
                    cancel: &cancel,
                    progress: &progress,
                };
                let check_stops = at < checking.max(1);
                let mut checked = match check_dreamcast(&sheet, mode, &job) {
                    Ok(checked) => {
                        assert!(!check_stops, "{name} {at}");
                        checked
                    }
                    Err(error) => {
                        // The disc's reader heeds the cancel as it opens the disc and as it reads
                        // each file, and its stop is the extraction's own.
                        let stopped = matches!(error, ExtractError::Cancelled);
                        assert!(check_stops && stopped, "{name} {at}: {error:?}");
                        folders.assert_untouched(&before);
                        continue;
                    }
                };
                let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
                let root = dreamcast_root();
                let extracted =
                    extract_dreamcast_with(&mut checked, &staging, &root, &DISC_SWEEP, &job);
                let tree = root.path_under(staging.path());
                if at < advances {
                    let error = extracted.unwrap_err();
                    let stopped = matches!(error, ExtractError::Cancelled);
                    assert!(stopped, "{name} {at}: {error:?}");
                    assert_eq!(bytes_under(&tree), written_before, "{name} {at}");
                } else {
                    extracted.unwrap();
                    assert_eq!(tree_listing(&tree), golden, "{name}");
                }
                folders.assert_outside_unchanged(&before);
                drop(staging);
                drop(checked);
                folders.assert_staging_gone(&before);
            }

            // A panic at any step unwinds through the check or the extraction, and the staging,
            // when one was made, goes as it unwinds: nothing is left of the run, in the staging
            // or outside it.
            for at in 1..=told.len() {
                let before = folders.before();
                let progress = PanicAt::new(at);
                let job = Job {
                    cancel: &cancel,
                    progress: &progress,
                };
                let parent = &folders.parent;
                assert_injected_panic(|| stage_dreamcast(&sheet, parent, mode, &DISC_SWEEP, &job));
                folders.assert_staging_gone(&before);
            }
        }
    }

    #[test]
    fn phases_follow_the_extraction() {
        let vectors = dreamcast_vectors();
        let full = &vectors["sets"]["full"];
        let disc_golden = full_golden(&vectors);
        let native_document = dreamcast_golden(&full["goldens"]["native-manifest.json"]);
        let native_golden: Value = serde_json::from_slice(native_document).unwrap();
        // The files each phase writes, in the extractor's order: those of no bytes tell no
        // advance.
        let held = |rows: &Value, folder: &str| -> Vec<String> {
            let rows = array(rows).iter().filter(|row| row["size"] != 0);
            rows.map(|row| format!("{folder}/{}", text(&row["path"])))
                .collect()
        };
        let mut unpacked = held(&disc_golden["files"], "disc");
        unpacked.extend([IP_BIN, DISC_MANIFEST].map(str::to_owned));
        unpacked.extend(held(&native_golden["files"], "native"));
        let whole: u64 = listed(&full["tree"]).iter().map(|(_, size, _)| size).sum();
        let checked_len = native_document.len() as u64;

        let folders = Folders::new();
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let cancel = Cancel::default();
        let script = Strictness::Script;
        for settings in [ExtractSettings::SHIPPED, DISC_SWEEP] {
            let progress = Recorded::default();
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let (staging, _) =
                stage_dreamcast(&sheet, &folders.parent, &script, &settings, &job).unwrap();
            drop(staging);
            let phases = phases_of(progress.told());
            let names: Vec<&str> = phases.iter().map(|(name, _)| *name).collect();
            assert_eq!(names, [READ_DISC.0, UNPACK.0, CHECK.0]);
            // The check reads the disc's head, its directories and SIZES.ROM, and tells no bytes.
            let (_, read) = &phases[0];
            assert!(read.is_empty(), "{read:?}");
            // In each phase that writes, what is done never goes back, every advance tells the
            // phase's total and the last ends at it: the bytes of every file before the native
            // manifest, then the native manifest's.
            let totals = [whole - checked_len, checked_len];
            for ((name, advances), total) in phases[1..].iter().zip(totals) {
                assert!(advances.iter().all(|(_, of, _)| *of == total), "{name}");
                let done: Vec<u64> = advances.iter().map(|(done, _, _)| *done).collect();
                assert!(done.windows(2).all(|pair| pair[0] <= pair[1]), "{name}");
                assert_eq!(done.last(), Some(&total), "{name}");
            }
            assert_eq!(items_of(&phases[1].1), unpacked);
            assert_eq!(items_of(&phases[2].1), [NATIVE_MANIFEST]);
        }
    }

    #[test]
    fn a_sizes_rom_that_changes_after_the_check_is_refused() {
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        let lba = disc_file_lba(&golden, SIZES_ROM);
        let (track, place) = sector_place(&golden, lba);
        // The dump lies apart from the test's dump folder, since the test changes it: only what
        // lies outside the staging is held to stay as it was.
        let dumps = tempfile::tempdir().unwrap();
        let sheet = DiscSet::full(Edc::Zero).write(dumps.path()).unwrap();
        let folders = Folders::new();
        let before = folders.before();
        // SIZES.ROM's sector, another table in it, under a header that places it where it is.
        let progress = ChangedAtUnpack {
            path: dumps.path().join(track),
            place,
            bytes: raw_sector(u32::try_from(lba).unwrap(), b"another table", Edc::Zero),
        };
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        let shipped = ExtractSettings::SHIPPED;
        let script = Strictness::Script;
        let error = stage_dreamcast(&sheet, &folders.parent, &script, &shipped, &job).unwrap_err();
        assert_eq!(error.rule(), "changed", "{error:?}");
        assert_eq!(
            error.to_string(),
            "the dump changed while it was read: disc/SIZES.ROM"
        );
        folders.assert_staging_gone(&before);
    }

    /// Progress that cuts the staged file at `path` to nothing once it is told an advance of the
    /// tree's file `after`: a DISK.ROM that changes in the staging between its writing and its
    /// reading back.
    struct CutAfter {
        path: PathBuf,
        after: &'static str,
    }

    impl Progress for CutAfter {
        fn phase(&self, _: PhaseId) {}

        fn advance(&self, _: u64, _: u64, item: Option<&RelPath>) {
            if item.map(RelPath::as_str) == Some(self.after) {
                let file = OpenOptions::new().write(true).open(&self.path).unwrap();
                file.set_len(0).unwrap();
            }
        }
    }

    #[test]
    fn a_disk_rom_cut_short_in_the_staging_is_refused() {
        let folders = Folders::new();
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let before = folders.before();
        let cancel = Cancel::default();
        let mut checked = check_dreamcast(&sheet, &Strictness::Script, &quiet(&cancel)).unwrap();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let root = dreamcast_root();
        // Cut once the disc manifest is written: after DISK.ROM, and before it is read back.
        let disk_rom = disc_path(&own_path(DISK_ROM));
        let progress = CutAfter {
            path: root.join(&disk_rom).path_under(staging.path()),
            after: DISC_MANIFEST,
        };
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        let shipped = ExtractSettings::SHIPPED;
        let error =
            extract_dreamcast_with(&mut checked, &staging, &root, &shipped, &job).unwrap_err();
        assert_eq!(error.rule(), "diskTruncated", "{error:?}");
        assert_eq!(error.to_string(), "Truncated DISK.ROM payload");
        folders.assert_outside_unchanged(&before);
        drop(staging);
        drop(checked);
        folders.assert_staging_gone(&before);
    }
}
