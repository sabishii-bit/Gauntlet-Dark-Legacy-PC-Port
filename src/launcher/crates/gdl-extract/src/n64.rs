//! The N64 tree, as the N64 extractor writes it: every file of the ROM's ROMDISK, decoded, at the
//! path its directory gives it; each executable overlay the boot table describes, under
//! `_program`; and `manifest.json`, which records the dump, each file with its digests and a WAD's
//! sections, each overlay and each map. [`check_n64`] reads, decodes and decides on the whole ROM
//! first and writes nothing, holding it to an installation's trust when asked to, so that a
//! staging is made only for a ROM that passed; [`extract_n64`] then writes its tree there.

use std::fmt;

use gdl_files::{DISC_NAMES, Job, RelPath, Staging};
use gdl_media::{
    ByteOrder, N64Rom, Overlay, RomdiskEntry, Sha256Digest, SourceName, WadSection, n64_verdict,
    wad_sections,
};

use crate::pyjson::{array, digest, text};
use crate::strictness::held;
use crate::tree::{SEPARATOR, TreePlan, TreeWriter};
use crate::{
    CHECK, ExtractError, ExtractSettings, JsonValue, READ_CARTRIDGE, Strictness, UNPACK, render,
};

/// The format the manifest names.
pub const N64_FORMAT: &str = "gauntlet-legends-n64-romdisk";

/// The version of that format the manifest names.
pub const N64_VERSION: u64 = 1;

/// The manifest's name, at the top of the tree.
pub const N64_MANIFEST: &str = "manifest.json";

/// What the name of a file the manifest describes as a native WAD ends with, in any letter case.
pub const WAD_SUFFIX: &str = ".wad";

/// What the path of a map's world file begins with, in this letter case.
pub const MAP_PREFIX: &str = "levels/";

/// What the path of a map's world file ends with, in this letter case.
pub const MAP_WORLD_SUFFIX: &str = "/worlds.wad";

/// The name of a map's objects file, in the folder of its world file.
pub const MAP_OBJECTS: &str = "objects.wad";

/// Whether the game runs a map of the tree, which the manifest records for each: none so far.
const RUNTIME_SUPPORTED: bool = false;

/// What `manifest.json` records of an N64 tree.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct N64Manifest {
    /// The dump the tree was extracted from.
    pub source: N64Source,
    /// Where the ROMDISK's header starts in the ROM.
    pub romdisk_offset: u32,
    /// Each file of the ROMDISK, in the order of its pointer table.
    pub files: Vec<N64FileRow>,
    /// Each executable overlay, in the order of the boot table.
    pub programs: Vec<ProgramRow>,
    /// Each map: a world file under `levels/`, in the order of the ROMDISK's files.
    pub maps: Vec<MapRow>,
}

/// What the manifest records of the dump.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct N64Source {
    /// The dump's file name as the system gave it.
    pub name: SourceName,
    /// How many bytes the dump holds.
    pub size: u64,
    /// The byte order the dump keeps the ROM in.
    pub byte_order: ByteOrder,
    /// The SHA-256 digest of the dump as it was given.
    pub sha256: Sha256Digest,
    /// The SHA-256 digest of the ROM in the big-endian byte order.
    pub canonical_sha256: Sha256Digest,
}

/// What the manifest records of a file of the ROMDISK.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct N64FileRow {
    /// Its path in the tree.
    pub path: RelPath,
    /// Where its record starts in the ROM.
    pub record_offset: u32,
    /// Where its stored bytes start in the ROM.
    pub rom_offset: u32,
    /// Its record's flags.
    pub flags: u32,
    /// How many bytes the ROM stores for it.
    pub stored_size: u32,
    /// How many bytes it holds once decoded.
    pub size: u32,
    /// Its record's last word, kept as it was found.
    pub unknown_word: u32,
    /// The SHA-256 digest of its stored bytes.
    pub stored_sha256: Sha256Digest,
    /// The SHA-256 digest of its decoded bytes, which the tree holds.
    pub sha256: Sha256Digest,
    /// Its sections, when it is a native WAD.
    pub wad_sections: Option<Vec<WadSection>>,
}

/// What the manifest records of an executable overlay.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProgramRow {
    /// Its path in the tree, under `_program`.
    pub path: RelPath,
    /// Where its extent starts in the ROM.
    pub rom_offset: u32,
    /// How many bytes its extent holds.
    pub stored_size: u32,
    /// The address it is loaded at.
    pub load_address: u32,
    /// How many bytes it holds once inflated, which the tree holds.
    pub size: u32,
    /// How much memory it takes once loaded.
    pub memory_size: u32,
    /// Where the zeroed memory after its bytes starts.
    pub bss_address: u32,
    /// How large that zeroed memory is.
    pub bss_size: u32,
    /// The SHA-256 digest of its inflated bytes.
    pub sha256: Sha256Digest,
}

/// What the manifest records of a map.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MapRow {
    /// The map's name: the second component of its world file's path.
    pub name: String,
    /// Its world file's path in the tree.
    pub world: String,
    /// Its objects file's path, beside its world file, whether or not the ROM holds one.
    pub objects: String,
}

impl N64Manifest {
    /// The manifest as the N64 extractor writes it, every key in the extractor's order.
    pub fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("format", text(N64_FORMAT)),
            ("version", JsonValue::Uint(N64_VERSION)),
            ("source", self.source.to_json()),
            ("romdisk_offset", word(self.romdisk_offset)),
            ("files", array(&self.files, N64FileRow::to_json)),
            ("programs", array(&self.programs, ProgramRow::to_json)),
            ("maps", array(&self.maps, MapRow::to_json)),
        ])
    }
}

impl N64Source {
    /// What the manifest writes of the dump: its name a unit at a time, as the system gave it.
    fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("name", JsonValue::Units(self.name.units().to_vec())),
            ("size", JsonValue::Uint(self.size)),
            ("byte_order", text(self.byte_order.as_str())),
            ("sha256", digest(&self.sha256)),
            ("canonical_sha256", digest(&self.canonical_sha256)),
        ])
    }
}

impl N64FileRow {
    /// What the manifest writes of the file: its sections last, and only for a WAD.
    fn to_json(&self) -> JsonValue {
        let mut members = vec![
            ("path", text(self.path.as_str())),
            ("record_offset", word(self.record_offset)),
            ("rom_offset", word(self.rom_offset)),
            ("flags", word(self.flags)),
            ("stored_size", word(self.stored_size)),
            ("size", word(self.size)),
            ("unknown_word", word(self.unknown_word)),
            ("stored_sha256", digest(&self.stored_sha256)),
            ("sha256", digest(&self.sha256)),
        ];
        if let Some(sections) = &self.wad_sections {
            members.push(("wad_sections", array(sections, section_json)));
        }
        JsonValue::Object(members)
    }
}

impl ProgramRow {
    /// What the manifest writes of the overlay.
    fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("path", text(self.path.as_str())),
            ("rom_offset", word(self.rom_offset)),
            ("stored_size", word(self.stored_size)),
            ("load_address", word(self.load_address)),
            ("size", word(self.size)),
            ("memory_size", word(self.memory_size)),
            ("bss_address", word(self.bss_address)),
            ("bss_size", word(self.bss_size)),
            ("sha256", digest(&self.sha256)),
        ])
    }
}

impl MapRow {
    /// What the manifest writes of the map.
    fn to_json(&self) -> JsonValue {
        JsonValue::Object(vec![
            ("name", text(&self.name)),
            ("world", text(&self.world)),
            ("objects", text(&self.objects)),
            ("runtime_supported", JsonValue::Bool(RUNTIME_SUPPORTED)),
        ])
    }
}

/// What the manifest writes of a WAD's section: its tag as text, NULs kept.
fn section_json(section: &WadSection) -> JsonValue {
    JsonValue::Object(vec![
        ("tag", JsonValue::Str(section.tag_text())),
        ("offset", word(section.offset)),
        ("size_or_count", word(section.size_or_count)),
        ("byte_order", text(section.byte_order.as_str())),
    ])
}

/// A word of the ROM, as the manifest writes it: in decimal.
fn word(value: u32) -> JsonValue {
    JsonValue::Uint(value.into())
}

/// An N64 ROM read and decided before anything is written: every file decoded and described, the
/// overlays read, every path of the tree planned and the manifest rendered.
pub struct CheckedRom {
    /// What the tree's manifest records.
    manifest: N64Manifest,
    /// Each file's decoded bytes, in the order of the manifest's files.
    files: Vec<Vec<u8>>,
    /// Each overlay's inflated bytes, in the order of the manifest's programs.
    programs: Vec<Vec<u8>>,
    /// The manifest as the tree holds it.
    document: Vec<u8>,
}

impl CheckedRom {
    /// What the tree's manifest records.
    pub fn manifest(&self) -> &N64Manifest {
        &self.manifest
    }

    /// How many bytes the tree holds: its files', its overlays' and its manifest's.
    pub fn bytes(&self) -> u64 {
        self.contents_len() + self.document.len() as u64
    }

    /// How many bytes the tree's files and overlays hold, which are written before the manifest.
    fn contents_len(&self) -> u64 {
        let contents = self.files.iter().chain(&self.programs);
        contents.map(|bytes| bytes.len() as u64).sum()
    }

    /// Each file and overlay of the tree, by its path, in the order the extractor writes them:
    /// the ROMDISK's files in the order of its pointer table, then the overlays in the order of
    /// the boot table.
    fn contents(&self) -> impl Iterator<Item = (&RelPath, &[u8])> {
        let files = self.manifest.files.iter().map(|row| &row.path);
        let programs = self.manifest.programs.iter().map(|row| &row.path);
        let bytes = self.files.iter().chain(&self.programs).map(Vec::as_slice);
        files.chain(programs).zip(bytes)
    }
}

impl fmt::Debug for CheckedRom {
    /// How much the tree holds, without its bytes.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        to.debug_struct("CheckedRom")
            .field("files", &self.files.len())
            .field("programs", &self.programs.len())
            .field("maps", &self.manifest.maps.len())
            .field("bytes", &self.bytes())
            .finish_non_exhaustive()
    }
}

/// What an N64 extraction wrote.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct N64Extraction {
    /// What the tree's manifest records.
    pub manifest: N64Manifest,
    /// The SHA-256 digest of `manifest.json` as it was written.
    pub manifest_sha256: Sha256Digest,
    /// How many bytes the tree holds.
    pub bytes: u64,
}

/// Reads `rom` as the N64 extractor reads it and decides on it, writing nothing, holding it as
/// `mode` says. In the phase [`READ_CARTRIDGE`], under [`Strictness::Install`], the ROM's identity
/// is first held to the trust's: its SHA-1 digest in the big-endian byte order must be the one the
/// trust names, and any other ROM is refused as [`ExtractError::Refused`], for what its header's
/// game code says of it, before a file is decoded. Then every file of its ROMDISK is decoded in the
/// order of the pointer table, the job's cancel looked at before each and its progress told the
/// stored bytes decoded after each; each file is then described, a WAD by its sections; the
/// overlays of the US ROM's boot table are read; and every path the tree will hold is planned,
/// before the manifest is rendered. The ROM is refused by the first rule of its format a part of it
/// breaks, in the order the extractor checks them: the ROMDISK's, a stream's that does not
/// inflate, a WAD's that does not parse, an overlay's. It is refused too when its tree would not
/// be the same on every system: two paths that differ only in letter case, among them two
/// overlays', a folder spelled in two letter cases, or a file that is also the folder above
/// another.
pub fn check_n64(
    rom: &N64Rom,
    mode: &Strictness<'_>,
    job: &Job<'_>,
) -> Result<CheckedRom, ExtractError> {
    check_n64_with(rom, mode, &ExtractSettings::SHIPPED, job)
}

/// Writes the tree of `checked` into the staging `into`, under `root` there: in the phase
/// [`UNPACK`], the ROMDISK's files in the order of its pointer table and then the overlays in the
/// order of the boot table, and in the phase [`CHECK`], `manifest.json`, last. Each file is made
/// new, so a name that something in the staging holds already refuses it, and each is written in
/// blocks of at most [`BLOCK_LEN`](gdl_files::BLOCK_LEN) bytes, the job's cancel looked at before
/// each and its progress told after. On a failure, what was written stays in the staging, for
/// its drop to remove.
pub fn extract_n64(
    checked: &CheckedRom,
    into: &Staging,
    root: &RelPath,
    job: &Job<'_>,
) -> Result<N64Extraction, ExtractError> {
    extract_n64_with(checked, into, root, &ExtractSettings::SHIPPED, job)
}

/// [`check_n64`], with the ROMDISK held to `settings`' limits and the overlays read through its
/// boot table.
pub(crate) fn check_n64_with(
    rom: &N64Rom,
    mode: &Strictness<'_>,
    settings: &ExtractSettings,
    job: &Job<'_>,
) -> Result<CheckedRom, ExtractError> {
    job.progress.phase(READ_CARTRIDGE);
    if let Strictness::Install(trust) = mode {
        let header = rom.header();
        held(n64_verdict(
            &header,
            rom.canonical_sha1(),
            trust.n64_canonical_sha1,
        ))?;
    }
    let romdisk = rom.romdisk(&settings.romdisk_limits)?;
    let entries = &romdisk.entries;
    // Every file is decoded before any is described, as the extractor decodes them, so that a
    // stream that does not inflate is refused before a WAD that does not parse.
    let total: u64 = entries
        .iter()
        .map(|entry| u64::from(entry.stored_size))
        .sum();
    let mut done = 0;
    let mut files = Vec::with_capacity(entries.len());
    for entry in entries {
        job.cancel.check()?;
        files.push(romdisk.decode(entry)?);
        done += u64::from(entry.stored_size);
        job.progress.advance(done, total, Some(&entry.path));
    }
    let mut rows = Vec::with_capacity(entries.len());
    let mut maps = Vec::new();
    for (entry, data) in entries.iter().zip(&files) {
        rows.push(file_row(entry, romdisk.stored(entry), data)?);
        maps.extend(map_row(&entry.path));
    }
    job.cancel.check()?;
    let overlays = rom.overlays(&settings.overlays)?;
    let programs: Vec<ProgramRow> = overlays.iter().map(program_row).collect();

    let mut plan = TreePlan::new();
    let paths = rows.iter().map(|row| &row.path);
    for path in paths.chain(programs.iter().map(|row| &row.path)) {
        plan.add(path)?;
    }
    plan.add(&manifest_path())?;
    plan.finish()?;

    let manifest = N64Manifest {
        source: N64Source {
            name: rom.source_name().clone(),
            size: rom.source_len(),
            byte_order: rom.byte_order(),
            sha256: rom.source_sha256(),
            canonical_sha256: rom.canonical_sha256(),
        },
        romdisk_offset: romdisk.offset,
        files: rows,
        programs,
        maps,
    };
    let document = render(&manifest.to_json());
    Ok(CheckedRom {
        manifest,
        files,
        programs: overlays.into_iter().map(|overlay| overlay.data).collect(),
        document,
    })
}

/// [`extract_n64`], writing in `settings`' blocks.
pub(crate) fn extract_n64_with(
    checked: &CheckedRom,
    into: &Staging,
    root: &RelPath,
    settings: &ExtractSettings,
    job: &Job<'_>,
) -> Result<N64Extraction, ExtractError> {
    job.progress.phase(UNPACK);
    let total = checked.contents_len();
    let mut writer = TreeWriter::new(into, root, job, total, settings.block_len);
    for (path, bytes) in checked.contents() {
        writer.write_bytes(path, bytes)?;
    }
    job.progress.phase(CHECK);
    let document = &checked.document;
    let total = document.len() as u64;
    let mut writer = TreeWriter::new(into, root, job, total, settings.block_len);
    let manifest_sha256 = writer.write_bytes(&manifest_path(), document)?;
    Ok(N64Extraction {
        manifest: checked.manifest.clone(),
        manifest_sha256,
        bytes: checked.bytes(),
    })
}

/// What the manifest records of `entry`, whose stored bytes are `stored` and decoded bytes `data`,
/// with its sections when its name ends in [`WAD_SUFFIX`] in any letter case: a WAD whose
/// directory does not parse refuses the ROM, naming the WAD.
fn file_row(entry: &RomdiskEntry, stored: &[u8], data: &[u8]) -> Result<N64FileRow, ExtractError> {
    let is_wad = entry
        .path
        .as_str()
        .to_ascii_lowercase()
        .ends_with(WAD_SUFFIX);
    let sections = if is_wad {
        Some(wad_sections(&entry.path, data)?)
    } else {
        None
    };
    Ok(N64FileRow {
        path: entry.path.clone(),
        record_offset: entry.record_offset,
        rom_offset: entry.rom_offset,
        flags: entry.flags,
        stored_size: entry.stored_size,
        size: entry.size,
        unknown_word: entry.unknown_word,
        stored_sha256: Sha256Digest::of(stored),
        sha256: Sha256Digest::of(data),
        wad_sections: sections,
    })
}

/// The map whose world file is at `path`, when it is one: a path that begins with [`MAP_PREFIX`]
/// and ends with [`MAP_WORLD_SUFFIX`], in that letter case. The map is named by the path's second
/// component, so a world file right under `levels/` names its map `worlds.wad`, and its objects
/// file is [`MAP_OBJECTS`] in the world file's folder.
fn map_row(path: &RelPath) -> Option<MapRow> {
    let world = path.as_str();
    if !(world.starts_with(MAP_PREFIX) && world.ends_with(MAP_WORLD_SUFFIX)) {
        return None;
    }
    let name = path.components().nth(1)?;
    let (folder, _) = world.rsplit_once(SEPARATOR)?;
    Some(MapRow {
        name: name.to_owned(),
        world: world.to_owned(),
        objects: format!("{folder}{SEPARATOR}{MAP_OBJECTS}"),
    })
}

/// What the manifest records of `overlay`.
fn program_row(overlay: &Overlay) -> ProgramRow {
    ProgramRow {
        path: overlay.path.clone(),
        rom_offset: overlay.rom_offset,
        stored_size: overlay.stored_size,
        load_address: overlay.load_address,
        size: u32::try_from(overlay.data.len())
            .expect("an overlay holds no more bytes than lie between two addresses of a word"),
        memory_size: overlay.memory_size,
        bss_address: overlay.bss_address,
        bss_size: overlay.bss_size,
        sha256: Sha256Digest::of(&overlay.data),
    }
}

/// The manifest's place in the tree, at its top.
fn manifest_path() -> RelPath {
    RelPath::parse(N64_MANIFEST, &DISC_NAMES).expect("the manifest's name is one a tree may hold")
}

#[cfg(test)]
mod tests {
    use std::array;
    use std::collections::BTreeSet;
    use std::ffi::{OsStr, OsString};
    use std::fs;

    use gdl_files::{Cancel, Job, ROMDISK_NAMES, Staging};
    use gdl_fixtures::{
        OverlaySpec, RomFile, WadOrder, default_rom_files, reorder, rom, rom_with_overlays,
        set_word, sha256_hex, unhex, wad,
    };
    use gdl_media::{
        COMPRESSED, MediaError, N64Rom, OverlayTable, PROGRAM_FOLDER, PROGRAM_SUFFIX,
        ROMDISK_LIMITS, RomdiskEntry, RomdiskLimits, Sha256Digest, SourceName, US_OVERLAYS,
    };
    use serde_json::Value;

    use super::{
        MapRow, N64_MANIFEST, N64Extraction, ProgramRow, check_n64, check_n64_with, extract_n64,
        extract_n64_with,
    };
    use crate::vectors::{
        CancelAt, Folders, Listed, N64_SWEEP, PanicAt, Recorded, STAGING_PREFIX, Told, array,
        assert_injected_panic, extraction_case, frozen, listed, n64_golden, n64_root, n64_vectors,
        phases_of, quiet, stage_n64, text, tree_listing,
    };
    use crate::{CHECK, ExtractError, ExtractSettings, READ_CARTRIDGE, Strictness, UNPACK, render};

    /// Where the refusal sweep keeps a WAD in its ROMs: named in capitals, which make it a WAD all
    /// the same, and named by its refusal.
    const WAD_PATH: &str = "a/x.WAD";

    /// Where a record of the ROMDISK keeps a file's size: its third word.
    const RECORD_SIZE_AT: usize = 8;

    /// The length of a word of the ROM.
    const WORD_LEN: usize = 4;

    /// The name of a dump's file called `text`.
    fn named(text: &str) -> SourceName {
        SourceName::from_units(text.encode_utf16().collect())
    }

    /// A number of the vectors.
    fn number(value: &Value) -> usize {
        usize::try_from(value.as_u64().expect("a number")).unwrap()
    }

    /// A number of the vectors that fits a word.
    fn word(value: &Value) -> u32 {
        u32::try_from(value.as_u64().expect("a number")).unwrap()
    }

    /// The files the ROMDISK of `image` lists.
    fn entries_of(image: &[u8]) -> Vec<RomdiskEntry> {
        let rom = N64Rom::from_bytes(named("source.z64"), image.to_vec()).unwrap();
        rom.romdisk(&ROMDISK_LIMITS).unwrap().entries
    }

    /// A ROM whose ROMDISK holds a plain file of one byte at each native path, as the extractor's
    /// tests build one for a path case. Plain files make the same bytes in either builder.
    fn paths_rom(paths: &[&str]) -> Vec<u8> {
        let files: Vec<RomFile> = paths
            .iter()
            .map(|path| RomFile::plain(path, b"x"))
            .collect();
        rom(&files).bytes
    }

    /// What a test's extraction of a dump wrote: its tree, its manifest's bytes, and what the
    /// extraction says it wrote.
    struct Written {
        listing: Vec<Listed>,
        manifest: Vec<u8>,
        extraction: N64Extraction,
    }

    /// Extracts `bytes`, given as the dump named `name`, as a player's extraction does: opened,
    /// checked, and only then written into a new staging. Checks that nothing outside the staging
    /// changed, the dump included, and that the staging goes once dropped.
    fn extract_file(name: &OsStr, bytes: &[u8]) -> Written {
        let folders = Folders::new();
        let dump = folders.dump(name, bytes);
        let before = folders.before();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let rom = N64Rom::open(&dump, &job).unwrap();
        let checked = check_n64(&rom, &Strictness::Script, &job).unwrap();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let extraction = extract_n64(&checked, &staging, &n64_root(), &job).unwrap();
        let tree = n64_root().path_under(staging.path());
        let written = Written {
            listing: tree_listing(&tree),
            manifest: fs::read(tree.join(N64_MANIFEST)).unwrap(),
            extraction,
        };
        assert_eq!(written.extraction.manifest, *checked.manifest());
        assert_eq!(written.extraction.bytes, checked.bytes());
        folders.assert_outside_unchanged(&before);
        drop(staging);
        folders.assert_staging_gone(&before);
        written
    }

    /// Checks that the extraction of the vectors' frozen image `image` writes the tree and the
    /// manifest the extractor wrote, and gives what it wrote.
    fn assert_golden_tree(image: &str) -> Written {
        let vectors = n64_vectors();
        let case = extraction_case(&vectors, image);
        let golden = n64_golden(&case["golden"]);
        let name = OsStr::new(text(&case["name"]));
        let written = extract_file(name, &frozen(&vectors, image));
        assert!(written.manifest == golden, "the manifest is not the golden");
        assert_eq!(written.listing, listed(&vectors["trees"][image]));
        // What the extraction says it wrote is what it wrote.
        let extraction = &written.extraction;
        assert_eq!(extraction.manifest_sha256, Sha256Digest::of(golden));
        let bytes: u64 = written.listing.iter().map(|(_, size, _)| size).sum();
        assert_eq!(extraction.bytes, bytes);
        assert!(render(&extraction.manifest.to_json()) == golden);
        written
    }

    /// A file name of `units` as Windows holds one: its UTF-16 units as they are.
    #[cfg(windows)]
    fn os_name(units: &[u16]) -> OsString {
        use std::os::windows::ffi::OsStringExt;

        OsString::from_wide(units)
    }

    /// A file name of `units` as Linux holds one: each run of characters in UTF-8, and each of
    /// the low surrogates U+DC80 to U+DCFF as the byte, no part of UTF-8, that it stands for in a
    /// name the extractor read.
    #[cfg(unix)]
    fn os_name(units: &[u16]) -> OsString {
        use std::os::unix::ffi::OsStringExt;

        /// What a byte that is no part of UTF-8 is added to, to become its low surrogate.
        const ESCAPED_BYTE_BASE: u16 = 0xDC00;

        let mut bytes = Vec::new();
        for decoded in char::decode_utf16(units.iter().copied()) {
            match decoded {
                Ok(character) => {
                    let mut encoded = [0; 4];
                    bytes.extend_from_slice(character.encode_utf8(&mut encoded).as_bytes());
                }
                Err(unpaired) => {
                    let unit = unpaired.unpaired_surrogate();
                    let byte = unit
                        .checked_sub(ESCAPED_BYTE_BASE)
                        .and_then(|byte| u8::try_from(byte).ok())
                        .filter(|byte| !byte.is_ascii())
                        .unwrap_or_else(|| panic!("{unit:#06x} stands for no byte"));
                    bytes.push(byte);
                }
            }
        }
        OsString::from_vec(bytes)
    }

    #[test]
    fn the_trees_own_names_are_the_ones_romdisk_paths_may_not_take() {
        // A ROMDISK path may begin with neither of the names the tree keeps for the extractor's
        // own files, so that no file of the ROM takes their place.
        assert_eq!(ROMDISK_NAMES.reserved_first, [PROGRAM_FOLDER, N64_MANIFEST]);
    }

    #[test]
    fn the_default_image_writes_the_golden_tree() {
        let written = assert_golden_tree("default");
        // The default image's WAD and texture, as the extractor's builder makes them, and no
        // overlay: the image is no US ROM.
        let digest_of = |path: &str| {
            let row = written.listing.iter().find(|(listed, _, _)| listed == path);
            row.map(|(_, _, digest)| digest.as_str()).unwrap()
        };
        let world = wad(WadOrder::Big, *b"TEST");
        assert_eq!(digest_of("levels/test/worlds.wad"), sha256_hex(&world));
        assert_eq!(digest_of("texture.bin"), sha256_hex(b"native"));
        let manifest = &written.extraction.manifest;
        assert!(manifest.programs.is_empty());
        let map = MapRow {
            name: "test".to_owned(),
            world: "levels/test/worlds.wad".to_owned(),
            objects: "levels/test/objects.wad".to_owned(),
        };
        assert_eq!(manifest.maps, [map]);
    }

    #[test]
    fn the_mixed_image_writes_the_golden_tree() {
        // A little-endian WAD named in capitals with a tag that begins with a NUL, an empty file
        // and a plain WAD.
        let written = assert_golden_tree("mixed");
        let files = &written.extraction.manifest.files;
        let sections: Vec<bool> = files.iter().map(|row| row.wad_sections.is_some()).collect();
        assert_eq!(sections, [true, false, true]);
        assert_eq!(files[1].size, 0);
    }

    #[test]
    fn every_byte_order_writes_its_golden() {
        let vectors = n64_vectors();
        let default = frozen(&vectors, "default");
        let default_tree = listed(&vectors["trees"]["default"]);
        let cases = array(&vectors["byteOrders"]);
        assert!(!cases.is_empty());
        for case in cases {
            let source = reorder(&default, text(&case["transform"]));
            let golden = n64_golden(&case["golden"]);
            let written = extract_file(OsStr::new(text(&case["name"])), &source);
            assert!(written.manifest == golden, "{case}");
            // The default image's tree, but for the manifest, which records the dump as given.
            let expected: Vec<Listed> = default_tree
                .iter()
                .map(|(path, size, digest)| {
                    if path == N64_MANIFEST {
                        (path.clone(), golden.len() as u64, sha256_hex(golden))
                    } else {
                        (path.clone(), *size, digest.clone())
                    }
                })
                .collect();
            assert_eq!(written.listing, expected, "{case}");
            let recorded = &written.extraction.manifest.source;
            assert_eq!(recorded.byte_order.as_str(), case["byteOrder"], "{case}");
            assert_eq!(recorded.sha256, Sha256Digest::of(&source));
            assert_eq!(recorded.canonical_sha256, Sha256Digest::of(&default));
        }
    }

    #[test]
    fn source_names_reach_the_manifest_as_the_script_writes_them() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        let golden = n64_golden(&extraction_case(&vectors, "default")["golden"]);
        let golden_lines: Vec<&str> = str::from_utf8(golden).unwrap().split('\n').collect();
        let at = golden_lines
            .iter()
            .position(|line| *line == "  \"source\": {")
            .unwrap()
            + 1;
        let cases = array(&vectors["sourceNames"]);
        assert!(!cases.is_empty());
        // A name in Unicode, and one that is not: a real file of each name on every system.
        for case in cases {
            let units: Vec<u16> = array(&case["units"])
                .iter()
                .map(|unit| u16::try_from(unit.as_u64().unwrap()).unwrap())
                .collect();
            let written = extract_file(&os_name(&units), &image);
            assert_eq!(written.extraction.manifest.source.name.units(), units);
            // The manifest is the default image's but for the name, which it writes as the
            // script does, a unit at a time.
            let mut expected = golden_lines.clone();
            expected[at] = text(&case["line"]);
            let manifest = str::from_utf8(&written.manifest).unwrap();
            assert_eq!(manifest.split('\n').collect::<Vec<_>>(), expected, "{case}");
        }
    }

    /// A refusal of the vectors as the check meets it: the ROM it is refused in, the limits the
    /// ROM is held to, the rule it is refused by and, where the case fixes them, the words the
    /// refusal's detail ends with.
    struct Refusal {
        case: String,
        image: Vec<u8>,
        limits: RomdiskLimits,
        rule: String,
        ending: Option<String>,
    }

    /// Every refusal of the N64 vectors that a ROM brings to the check: its paths, collisions,
    /// record and header words and limit, its WADs kept in a ROM as plain files, and its streams
    /// kept as ROMDISK files, which allow nothing after a stream's end.
    fn check_refusals(vectors: &Value) -> Vec<Refusal> {
        let default = frozen(vectors, "default");
        let records: Vec<usize> = entries_of(&default)
            .iter()
            .map(|entry| entry.record_offset as usize)
            .collect();
        let mut refusals = Vec::new();
        let mut push = |case: &Value, image, limits, ending| {
            if let Some(rule) = case["rule"].as_str() {
                refusals.push(Refusal {
                    case: case.to_string(),
                    image,
                    limits,
                    rule: rule.to_owned(),
                    ending,
                });
            }
        };
        for case in array(&vectors["paths"]) {
            // The rule of names a path breaks closes the launcher's words for it.
            let ending = case["nameRule"].as_str().map(|rule| format!("({rule})"));
            push(
                case,
                paths_rom(&[text(&case["path"])]),
                ROMDISK_LIMITS,
                ending,
            );
        }
        for case in array(&vectors["collisions"]) {
            let paths: Vec<&str> = array(&case["paths"]).iter().map(text).collect();
            push(case, paths_rom(&paths), ROMDISK_LIMITS, None);
        }
        for case in array(&vectors["recordWords"]) {
            let mut image = default.clone();
            let at = records[number(&case["index"])] + WORD_LEN * number(&case["word"]);
            set_word(&mut image, at, word(&case["value"]));
            push(case, image, ROMDISK_LIMITS, None);
        }
        for case in array(&vectors["headerWords"]) {
            let mut image = default.clone();
            match case.get("copy") {
                // A pointer made a copy of another, `[to, from]`.
                Some(copy) => {
                    let (to, from) = (number(&copy[0]), number(&copy[1]));
                    image.copy_within(from..from + WORD_LEN, to);
                }
                None => set_word(&mut image, number(&case["offset"]), word(&case["value"])),
            }
            push(case, image, ROMDISK_LIMITS, None);
        }
        for case in array(&vectors["limits"]) {
            let limits = RomdiskLimits {
                max_total: case["maxTotal"].as_u64().unwrap(),
                ..ROMDISK_LIMITS
            };
            push(case, default.clone(), limits, None);
        }
        for case in array(&vectors["wads"]) {
            let file = RomFile::plain(&format!("/{WAD_PATH}"), &unhex(&case["hex"]));
            let ending = Some(format!(": {WAD_PATH}"));
            push(case, rom(&[file]).bytes, ROMDISK_LIMITS, ending);
        }
        for case in array(&vectors["deflate"]) {
            if case["trailer"] != "none" {
                continue;
            }
            // The stream kept as a plain file's bytes, then flagged compressed and given the
            // case's size.
            let built = rom(&[RomFile::plain("/x.bin", &unhex(&case["stream"]))]);
            let mut image = built.bytes;
            set_word(&mut image, built.records[0], COMPRESSED);
            set_word(
                &mut image,
                built.records[0] + RECORD_SIZE_AT,
                word(&case["size"]),
            );
            push(case, image, ROMDISK_LIMITS, None);
        }
        refusals
    }

    #[test]
    fn the_check_refuses_before_any_staging_is_made() {
        let vectors = n64_vectors();
        let folders = Folders::new();
        let script = Strictness::Script;

        // The extractor's own case: the default image's first stored byte made 0xFF, which no
        // raw DEFLATE stream begins with.
        let mut image = frozen(&vectors, "default");
        let first = entries_of(&image)[0].rom_offset as usize;
        image[first] = 0xFF;
        let error = folders.refused_rom(&image, &script, &ExtractSettings::SHIPPED);
        assert!(
            matches!(error, ExtractError::Media(MediaError::Rom { .. })),
            "{error:?}"
        );
        assert_eq!(error.rule(), "deflate");

        // Every other refusal of the vectors that a ROM brings to the check.
        let refusals = check_refusals(&vectors);
        let rules: BTreeSet<&str> = refusals
            .iter()
            .map(|refusal| refusal.rule.as_str())
            .collect();
        for rule in [
            "leadingSlash",
            "unsafePath",
            "duplicatePath",
            "collision",
            "treeFolderCase",
            "flags",
            "outsideData",
            "fileSize",
            "fileOverlap",
            "directoryCount",
            "recordPlace",
            "expandedSize",
            "wadHeader",
            "wadDirectory",
            "wadSection",
            "deflate",
            "deflateEnd",
            "deflateTrailer",
        ] {
            assert!(rules.contains(rule), "no refusal by {rule}");
        }
        for refusal in &refusals {
            let settings = ExtractSettings {
                romdisk_limits: refusal.limits,
                ..ExtractSettings::SHIPPED
            };
            let error = folders.refused_rom(&refusal.image, &script, &settings);
            assert_eq!(error.rule(), refusal.rule, "{}", refusal.case);
            if let Some(ending) = &refusal.ending {
                let detail = error.to_string();
                assert!(
                    detail.ends_with(ending.as_str()),
                    "{detail}: {}",
                    refusal.case
                );
            }
        }
    }

    #[test]
    fn a_folder_spelled_two_ways_in_the_romdisk_is_refused() {
        let vectors = n64_vectors();
        let case = array(&vectors["collisions"])
            .iter()
            .find(|case| case["rule"] == "treeFolderCase")
            .unwrap();
        // The extractor takes it and writes one folder on Windows and two on Linux; the reader
        // takes it too, and the plan of the tree refuses it before anything is written.
        assert!(
            case["python"].is_null() && case["romdiskRule"].is_null(),
            "{case}"
        );
        let paths: Vec<&str> = array(&case["paths"]).iter().map(text).collect();
        let cancel = Cancel::default();
        let cases: [(&[&str], &str); 3] = [
            (&paths, "a folder is spelled Font and font"),
            (&["/font/b", "/Font/a"], "a folder is spelled font and Font"),
            (
                &["/a/Font/x", "/a/font/y"],
                "a folder is spelled a/Font and a/font",
            ),
        ];
        for (paths, detail) in cases {
            let image = paths_rom(paths);
            let rom = N64Rom::from_bytes(named("source.z64"), image).unwrap();
            rom.romdisk(&ROMDISK_LIMITS).unwrap();
            let error = check_n64(&rom, &Strictness::Script, &quiet(&cancel)).unwrap_err();
            assert_eq!(error.rule(), "treeFolderCase", "{paths:?}");
            assert_eq!(error.to_string(), detail);
        }
    }

    /// Four overlays for a boot table, named `names` in order: of different lengths, loaded at
    /// different addresses, with zeroed memory after them or none and every amount of padding a
    /// reader takes.
    fn overlay_specs(names: [&'static str; 4]) -> [OverlaySpec; 4] {
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
    fn rom_with_table(specs: &[OverlaySpec; 4]) -> (N64Rom, ExtractSettings) {
        let built = rom_with_overlays(&default_rom_files(), specs.clone());
        let rom = N64Rom::from_bytes(named("overlays.z64"), built.bytes).unwrap();
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

    /// The text the manifest gives `row`, as one of its programs.
    fn program_text(row: &ProgramRow) -> String {
        format!(
            concat!(
                "    {{\n",
                "      \"path\": \"{}\",\n",
                "      \"rom_offset\": {},\n",
                "      \"stored_size\": {},\n",
                "      \"load_address\": {},\n",
                "      \"size\": {},\n",
                "      \"memory_size\": {},\n",
                "      \"bss_address\": {},\n",
                "      \"bss_size\": {},\n",
                "      \"sha256\": \"{}\"\n",
                "    }}",
            ),
            row.path.as_str(),
            row.rom_offset,
            row.stored_size,
            row.load_address,
            row.size,
            row.memory_size,
            row.bss_address,
            row.bss_size,
            row.sha256,
        )
    }

    #[test]
    fn overlays_join_the_tree_under_program() {
        let specs = overlay_specs(["game", "menu", "tower", "boss"]);
        let (rom, settings) = rom_with_table(&specs);
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let checked = check_n64_with(&rom, &Strictness::Script, &settings, &job).unwrap();
        // Each overlay is recorded as its record describes it and its stream inflates.
        let overlays = rom.overlays(&settings.overlays).unwrap();
        let programs = &checked.manifest().programs;
        assert_eq!(programs.len(), specs.len());
        for ((row, overlay), spec) in programs.iter().zip(&overlays).zip(&specs) {
            let path = format!("{PROGRAM_FOLDER}/{}{PROGRAM_SUFFIX}", spec.name);
            assert_eq!(row.path.as_str(), path);
            assert_eq!(row.path, overlay.path);
            assert_eq!(row.rom_offset, overlay.rom_offset, "{path}");
            assert_eq!(row.stored_size, overlay.stored_size, "{path}");
            assert_eq!(row.load_address, spec.load_address, "{path}");
            assert_eq!(row.size as usize, spec.data.len(), "{path}");
            assert_eq!(row.memory_size, overlay.memory_size, "{path}");
            assert_eq!(row.bss_address, overlay.bss_address, "{path}");
            assert_eq!(row.bss_size, spec.bss_size, "{path}");
            assert_eq!(row.sha256, Sha256Digest::of(&spec.data), "{path}");
        }

        // They are written after the ROMDISK's files, in the boot table's order, and the
        // manifest records them in the extractor's order of keys.
        let folders = Folders::new();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let progress = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        extract_n64_with(&checked, &staging, &n64_root(), &settings, &job).unwrap();
        let tree = n64_root().path_under(staging.path());
        for spec in &specs {
            let file = tree
                .join(PROGRAM_FOLDER)
                .join(format!("{}{PROGRAM_SUFFIX}", spec.name));
            assert!(fs::read(file).unwrap() == spec.data, "{}", spec.name);
        }
        let mut written: Vec<String> = Vec::new();
        for told in progress.told() {
            if let Told::Advance {
                item: Some(item), ..
            } = told
                && written.last() != Some(&item)
            {
                written.push(item);
            }
        }
        let files = ["levels/test/worlds.wad", "texture.bin"].map(str::to_owned);
        let overlay_paths = programs.iter().map(|row| row.path.as_str().to_owned());
        let order: Vec<String> = files
            .into_iter()
            .chain(overlay_paths)
            .chain([N64_MANIFEST.to_owned()])
            .collect();
        assert_eq!(written, order);
        let document = fs::read_to_string(tree.join(N64_MANIFEST)).unwrap();
        for row in programs {
            assert!(document.contains(&program_text(row)), "{document}");
        }

        // Under the shipped boot table the same ROM has none: its digest is not the US ROM's.
        let shipped = check_n64(&rom, &Strictness::Script, &quiet(&cancel)).unwrap();
        assert!(shipped.manifest().programs.is_empty());
    }

    #[test]
    fn overlays_named_alike_are_refused_before_anything_is_written() {
        let folders = Folders::new();
        // Their reader does not hold the overlays' names to one another, so the plan of the tree
        // does: two named alike, or alike but for letter case, would be one file on a file system
        // that folds letter case.
        for (names, detail) in [
            (
                ["game", "menu", "game", "boss"],
                "duplicate case-insensitive tree path: _program/game.bin",
            ),
            (
                ["game", "GAME", "menu", "boss"],
                "duplicate case-insensitive tree path: _program/GAME.bin",
            ),
        ] {
            let specs = overlay_specs(names);
            let (rom, settings) = rom_with_table(&specs);
            assert_eq!(rom.overlays(&settings.overlays).unwrap().len(), 4);
            let error = folders.refused_rom(rom.canonical(), &Strictness::Script, &settings);
            assert_eq!(error.rule(), "treeDuplicate", "{names:?}");
            assert_eq!(error.to_string(), detail);
        }
    }

    #[test]
    fn maps_follow_the_extractors_spelling() {
        let world = wad(WadOrder::Big, *b"TEST");
        let paths = [
            "/levels/a/worlds.wad",
            "/levels/worlds.wad",
            "/levels/b/c/worlds.wad",
            // A WAD all the same, but no map: the world file's name is matched in its letter
            // case, and so is the folder it lies under.
            "/levels/e/WORLDS.WAD",
            "/other/levels/f/worlds.wad",
            "/levelsx/g/worlds.wad",
            // No WAD, and no map.
            "/levels/h/worlds.wadx",
        ];
        let files: Vec<RomFile> = paths
            .iter()
            .map(|path| RomFile::plain(path, &world))
            .collect();
        let cartridge = N64Rom::from_bytes(named("maps.z64"), rom(&files).bytes).unwrap();
        let cancel = Cancel::default();
        let script = Strictness::Script;
        let checked = check_n64(&cartridge, &script, &quiet(&cancel)).unwrap();
        let manifest = checked.manifest();
        let map = |name: &str, world: &str, objects: &str| MapRow {
            name: name.to_owned(),
            world: world.to_owned(),
            objects: objects.to_owned(),
        };
        // A map is named by its world file's second component, so one right under `levels/` is
        // named by the file, and its objects lie beside its world file.
        let maps = [
            map("a", "levels/a/worlds.wad", "levels/a/objects.wad"),
            map("worlds.wad", "levels/worlds.wad", "levels/objects.wad"),
            map("b", "levels/b/c/worlds.wad", "levels/b/c/objects.wad"),
        ];
        assert_eq!(manifest.maps, maps);
        let wads: Vec<bool> = manifest
            .files
            .iter()
            .map(|row| row.wad_sections.is_some())
            .collect();
        assert_eq!(wads, [true, true, true, true, true, true, false]);

        // A folder named in capitals is no map's either.
        let capitals = [RomFile::plain("/Levels/d/worlds.wad", &world)];
        let cartridge = N64Rom::from_bytes(named("maps.z64"), rom(&capitals).bytes).unwrap();
        let checked = check_n64(&cartridge, &script, &quiet(&cancel)).unwrap();
        assert!(checked.manifest().maps.is_empty());
    }

    #[test]
    fn phases_and_progress_follow_the_extraction() {
        let vectors = n64_vectors();
        let golden = n64_golden(&extraction_case(&vectors, "default")["golden"]);
        let image = frozen(&vectors, "default");
        let rom = N64Rom::from_bytes(named("source.z64"), image).unwrap();
        let folders = Folders::new();
        let cancel = Cancel::default();
        for settings in [ExtractSettings::SHIPPED, N64_SWEEP] {
            let progress = Recorded::default();
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let checked = check_n64_with(&rom, &Strictness::Script, &settings, &job).unwrap();
            let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
            extract_n64_with(&checked, &staging, &n64_root(), &settings, &job).unwrap();
            let phases = phases_of(progress.told());
            let names: Vec<&str> = phases.iter().map(|(name, _)| *name).collect();
            assert_eq!(names, [READ_CARTRIDGE.0, UNPACK.0, CHECK.0]);
            // In each phase, what is done never goes back, every advance tells the same total,
            // and the last ends at it.
            for (name, advances) in &phases {
                let total = advances.first().map(|(_, total, _)| *total).unwrap();
                assert!(advances.iter().all(|(_, of, _)| *of == total), "{name}");
                let done: Vec<u64> = advances.iter().map(|(done, _, _)| *done).collect();
                assert!(done.windows(2).all(|pair| pair[0] <= pair[1]), "{name}");
                assert_eq!(done.last(), Some(&total), "{name}");
            }
            // The ROM's files are read a file at a time, counting the bytes stored for each; the
            // tree's files are written counting their own bytes, and the manifest last.
            let (_, read) = &phases[0];
            let map = "levels/test/worlds.wad".to_owned();
            let texture = "texture.bin".to_owned();
            assert_eq!(*read, [(23, 29, map.clone()), (29, 29, texture.clone())]);
            let (_, unpacked) = &phases[1];
            assert!(unpacked.iter().all(|(_, total, _)| *total == 30));
            let items: Vec<&String> = unpacked.iter().map(|(_, _, item)| item).collect();
            let first_texture = items.iter().position(|item| **item == texture).unwrap();
            assert!(items[..first_texture].iter().all(|item| **item == map));
            assert!(items[first_texture..].iter().all(|item| **item == texture));
            let (_, checked_phase) = &phases[2];
            let total = golden.len() as u64;
            assert!(
                checked_phase
                    .iter()
                    .all(|(_, of, item)| *of == total && item == N64_MANIFEST)
            );
            drop(staging);
        }
    }

    #[test]
    fn a_check_stops_when_its_job_is_asked_to() {
        let vectors = n64_vectors();
        let rom = N64Rom::from_bytes(named("source.z64"), frozen(&vectors, "default")).unwrap();
        // Asked before it begins, after the first file is decoded, or after the last, before the
        // overlays are read: the check stops, and nothing is decided.
        for at in 0..=2 {
            let cancel = Cancel::default();
            let progress = CancelAt::new(&cancel, at);
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let error = check_n64(&rom, &Strictness::Script, &job).unwrap_err();
            assert!(matches!(error, ExtractError::Cancelled), "{at}: {error:?}");
        }
    }

    #[test]
    fn a_panic_at_any_step_leaves_nothing() {
        let vectors = n64_vectors();
        let folders = Folders::new();
        let dump = folders.dump(OsStr::new("source.z64"), &frozen(&vectors, "default"));
        let cancel = Cancel::default();

        // A whole run, in blocks short enough that each file spans several, counts its steps:
        // its phases and its advances.
        let counted = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &counted,
        };
        let script = Strictness::Script;
        let (staging, _) = stage_n64(&dump, &folders.parent, &script, &N64_SWEEP, &job).unwrap();
        drop(staging);
        let steps = counted.told().len();
        assert!(steps > 3 * 4, "{steps} steps");

        // A panic at any of them unwinds through the extraction, and the staging, when one was
        // made, goes as it unwinds: nothing is left of the run, in the staging or outside it.
        for at in 1..=steps {
            let before = folders.before();
            let progress = PanicAt::new(at);
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let parent = &folders.parent;
            assert_injected_panic(|| stage_n64(&dump, parent, &script, &N64_SWEEP, &job));
            folders.assert_staging_gone(&before);
        }
    }
}
