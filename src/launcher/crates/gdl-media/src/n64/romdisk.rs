//! The ROMDISK, the file system the N64 ROM keeps the game's files in, read as the N64 extractor
//! reads it: one header, found by its magic, then a directory of a pointer table and, for each
//! file, a record of five words (its flags, its stored and expanded sizes, where its bytes lie and
//! a last word kept as it is found) followed by its path. Every path is checked before it is kept,
//! and no two may share a name on a case-insensitive file system or be a file and a folder at once.
//! A file's bytes are looked at only as it is decoded: as they are stored, or inflated when its
//! record flags them compressed.

use std::fmt;

use gdl_files::{NameSet, ROMDISK_NAMES, RelPath};

use super::inflate::{Trailer, inflate};
use super::{MAX_ROM_LEN, WORD_PLACE_LEN, checked_end, too_large, word_at, words};
use crate::{MediaError, RomRule};

/// The magic a ROMDISK's header begins with, its NUL included, which a ROM holds exactly once.
pub const ROMDISK_MAGIC: &[u8; 33] = b"-==ROMDISK Header - HERE I AM==-\0";

/// The length of a ROMDISK's header. Its directory and its files' bytes lie past it, at offsets
/// counted from where it starts.
pub const ROMDISK_HEADER_LEN: u64 = 48;

/// Where the header keeps its four words, counted from its start: the directory's length, where
/// its pointer table starts, how many files it lists, and a reserved zero.
pub const HEADER_WORDS_AT: u64 = 32;

/// The record flag of a file the ROMDISK keeps raw-DEFLATEd. A file without flags is kept as it
/// is.
pub const COMPRESSED: u32 = 0x2000_0000;

/// The length of a record's five words, which its path follows.
pub const RECORD_WORDS_LEN: u64 = 20;

/// The most files a ROMDISK may list, and the most they may expand to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RomdiskLimits {
    /// The most files the directory may list.
    pub max_entries: u32,
    /// The most bytes one file may expand to.
    pub max_file: u64,
    /// The most bytes all the files together may expand to.
    pub max_total: u64,
}

/// The limits the N64 extractor holds a ROMDISK to: 65,536 files, of at most 64 MiB each and
/// 512 MiB in all.
pub const ROMDISK_LIMITS: RomdiskLimits = RomdiskLimits {
    max_entries: 65_536,
    max_file: 64 << 20,
    max_total: 512 << 20,
};

/// A file the ROMDISK lists, as its record describes it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RomdiskEntry {
    /// Its path, without the `/` the format begins every path with.
    pub path: RelPath,
    /// Where its record starts in the ROM.
    pub record_offset: u32,
    /// Its record's flags: [`COMPRESSED`], or none.
    pub flags: u32,
    /// How many bytes the ROM stores for it.
    pub stored_size: u32,
    /// How many bytes it holds once expanded.
    pub size: u32,
    /// Where its stored bytes start in the ROM.
    pub rom_offset: u32,
    /// Its record's last word, which the format leaves unexplained and the extractor keeps as it
    /// finds it.
    pub unknown_word: u32,
}

/// A ROM's ROMDISK: where its header lies, and the files its directory lists.
pub struct Romdisk<'a> {
    /// Where the header starts in the ROM.
    pub offset: u32,
    /// The files, in the order of the pointer table.
    pub entries: Vec<RomdiskEntry>,
    /// The ROM in the big-endian byte order, which holds the files' bytes.
    rom: &'a [u8],
}

impl Romdisk<'_> {
    /// The bytes the ROM stores for `entry`, one of this ROMDISK's entries: the file as it is, or
    /// its raw DEFLATE stream when its record flags it [`COMPRESSED`]. Panics when the entry
    /// reaches past the ROM, as none of the ROMDISK's own does.
    pub fn stored(&self, entry: &RomdiskEntry) -> &[u8] {
        let start = entry.rom_offset as usize;
        &self.rom[start..start + entry.stored_size as usize]
    }

    /// The bytes `entry`, one of this ROMDISK's entries, holds, as the extracted tree keeps them:
    /// its stored bytes when it is kept as it is, or, when its record flags it [`COMPRESSED`], its
    /// raw DEFLATE stream inflated to exactly the size its record gives, with nothing after the
    /// stream's end, or the rule the stream breaks. Panics as [`stored`](Self::stored) does.
    pub fn decode(&self, entry: &RomdiskEntry) -> Result<Vec<u8>, MediaError> {
        let stored = self.stored(entry);
        if entry.flags == COMPRESSED {
            inflate(stored, entry.size as usize, Trailer::None)
        } else {
            Ok(stored.to_vec())
        }
    }
}

impl fmt::Debug for Romdisk<'_> {
    /// Where the ROMDISK lies and what it lists, without the ROM's bytes.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        to.debug_struct("Romdisk")
            .field("offset", &self.offset)
            .field("entries", &self.entries)
            .finish_non_exhaustive()
    }
}

/// The byte a record's path ends with.
const PATH_END: u8 = 0;

/// The one `/` every ROMDISK path begins with.
const PATH_START: char = '/';

/// What the header is called where it reaches past the ROM's end.
const HEADER_PART: &str = "ROMDISK header";

/// What the directory is called where it reaches past the ROM's end.
const DIRECTORY_PART: &str = "ROMDISK directory";

/// What the pointer table is called where it reaches past the directory's end.
const POINTER_TABLE_PART: &str = "ROMDISK pointer table";

/// What a record's words are called where they reach past the directory's end.
const ENTRY_PART: &str = "ROMDISK entry";

/// What the N64 extractor answers for a ROM without the ROMDISK's magic, or with it more than
/// once.
const NOT_ONE_HEADER: &str = "expected exactly one native ROMDISK header";

/// What it answers for a header whose reserved word is not zero, or that lists no file or more
/// than the most a ROMDISK may list.
const UNSUPPORTED_COUNT: &str = "unsupported ROMDISK header or directory count";

/// What it answers for a pointer table that starts inside the header.
const TABLE_IN_HEADER: &str = "ROMDISK directory overlaps its header";

/// What it answers for a record that starts before the one listed before it ends, or off a word.
const MISPLACED_RECORD: &str = "ROMDISK records overlap or are not word aligned";

/// What it answers for a path that does not end within the directory.
const UNTERMINATED_PATH: &str = "unterminated ROMDISK path";

/// What it answers for a path that holds a byte outside ASCII.
const NON_ASCII_PATH: &str = "non-ASCII ROMDISK path";

/// What it answers for a path that does not begin with exactly one `/`.
const NOT_ONE_SLASH: &str = "ROMDISK paths must have one leading slash";

/// What it answers for files that together expand past the most they may.
const EXPANDED_TOO_FAR: &str = "ROMDISK expanded size exceeds extraction limit";

/// Reads the ROMDISK that `rom`, a ROM in the big-endian byte order, holds, held to `limits`, and
/// refuses it by the first rule it breaks, in the order the N64 extractor checks them, once a ROM
/// longer than the largest cartridge has been refused. As in the extractor, files whose bytes
/// overlap or are shared, compressed sizes and where in a word a file's bytes start are not
/// checked.
pub(super) fn read<'a>(rom: &'a [u8], limits: &RomdiskLimits) -> Result<Romdisk<'a>, MediaError> {
    // Every place in a ROM no longer than the largest cartridge fits the word an entry keeps it
    // in.
    let len = rom.len() as u64;
    if len > MAX_ROM_LEN {
        return Err(too_large());
    }
    let base = find_magic(rom, 0)
        .filter(|&base| find_magic(rom, base + 1).is_none())
        .ok_or_else(|| MediaError::rom(RomRule::RomdiskHeader, NOT_ONE_HEADER))?;
    let base = base as u64;
    checked_end(base, ROMDISK_HEADER_LEN, len, HEADER_PART)?;
    let [directory_len, table_offset, count, reserved] = words(rom, base + HEADER_WORDS_AT);
    if reserved != 0 || count == 0 || count > limits.max_entries {
        return Err(MediaError::rom(RomRule::DirectoryCount, UNSUPPORTED_COUNT));
    }
    let table = base + u64::from(table_offset);
    let end = checked_end(table, u64::from(directory_len), len, DIRECTORY_PART)?;
    let pointers_len = WORD_PLACE_LEN * u64::from(count);
    let records_start = checked_end(table, pointers_len, end, POINTER_TABLE_PART)?;
    if u64::from(table_offset) < ROMDISK_HEADER_LEN {
        return Err(MediaError::rom(RomRule::DirectoryOverlap, TABLE_IN_HEADER));
    }

    let mut entries = Vec::new();
    let mut names = NameSet::default();
    let mut total = 0;
    // Where the record listed last ends: the next may not start before it.
    let mut floor = records_start;
    for pointer in 0..u64::from(count) {
        let record = base + u64::from(word_at(rom, table + WORD_PLACE_LEN * pointer));
        if record < floor || !record.is_multiple_of(WORD_PLACE_LEN) {
            return Err(MediaError::rom(RomRule::RecordPlace, MISPLACED_RECORD));
        }
        let name_start = checked_end(record, RECORD_WORDS_LEN, end, ENTRY_PART)?;
        let name = &rom[name_start as usize..end as usize];
        let name_len = name
            .iter()
            .position(|&byte| byte == PATH_END)
            .ok_or_else(|| MediaError::rom(RomRule::UnterminatedPath, UNTERMINATED_PATH))?;
        // The record ends past its path's end, padded to a word.
        floor = (name_start + name_len as u64 + 1).next_multiple_of(WORD_PLACE_LEN);
        let path = checked_path(&name[..name_len])?;
        names.insert(path.clone()).map_err(|duplicate| {
            let detail = format!(
                "duplicate case-insensitive ROMDISK path: {}",
                duplicate.name
            );
            MediaError::rom(RomRule::DuplicatePath, detail)
        })?;

        let [flags, stored_size, size, data_offset, unknown_word] = words(rom, record);
        let named = path.as_str();
        if flags != 0 && flags != COMPRESSED {
            let detail = format!("unsupported ROMDISK flags 0x{flags:x}: {named}");
            return Err(MediaError::rom(RomRule::Flags, detail));
        }
        if u64::from(size) > limits.max_file || (flags == 0 && stored_size != size) {
            let detail = format!("invalid ROMDISK file size: {named}");
            return Err(MediaError::rom(RomRule::FileSize, detail));
        }
        if u64::from(data_offset) < ROMDISK_HEADER_LEN {
            let detail = format!("ROMDISK file overlaps its header: {named}");
            return Err(MediaError::rom(RomRule::FileOverlap, detail));
        }
        // A file's bytes end before the pointer table, where the directory starts.
        let data = base + u64::from(data_offset);
        let stored = u64::from(stored_size);
        checked_end(data, stored, table, format_args!("ROMDISK file {named}"))?;
        total += u64::from(size);
        if total > limits.max_total {
            return Err(MediaError::rom(RomRule::ExpandedSize, EXPANDED_TOO_FAR));
        }
        entries.push(RomdiskEntry {
            path,
            record_offset: place(record),
            flags,
            stored_size,
            size,
            rom_offset: place(data),
            unknown_word,
        });
    }
    names.conflicts().map_err(|both| {
        let detail = format!("ROMDISK file/directory collision: {}", both.name);
        MediaError::rom(RomRule::Collision, detail)
    })?;
    Ok(Romdisk {
        offset: place(base),
        entries,
        rom,
    })
}

/// Where the ROMDISK's magic first starts in `rom`, at `from` or after it.
fn find_magic(rom: &[u8], from: usize) -> Option<usize> {
    let after = rom.get(from..)?;
    let at = after
        .windows(ROMDISK_MAGIC.len())
        .position(|window| window == ROMDISK_MAGIC)?;
    Some(from + at)
}

/// The path a record's name spells, without its leading `/`: ASCII, beginning with exactly one
/// `/`, and a name the extracted tree may hold under the ROMDISK's rules.
fn checked_path(name: &[u8]) -> Result<RelPath, MediaError> {
    let native = str::from_utf8(name)
        .ok()
        .filter(|native| native.is_ascii())
        .ok_or_else(|| MediaError::rom(RomRule::NonAsciiPath, NON_ASCII_PATH))?;
    let rest = native
        .strip_prefix(PATH_START)
        .filter(|rest| !rest.starts_with(PATH_START))
        .ok_or_else(|| MediaError::rom(RomRule::LeadingSlash, NOT_ONE_SLASH))?;
    RelPath::parse(rest, &ROMDISK_NAMES).map_err(|refused| {
        let detail = format!("unsafe ROMDISK path: {native:?} ({})", refused.rule);
        MediaError::rom(RomRule::UnsafePath, detail)
    })
}

/// `at`, a place in a ROM no longer than the largest cartridge, as the word an entry keeps it in.
fn place(at: u64) -> u32 {
    u32::try_from(at).expect("a place in a ROM of at most 64 MiB fits a word")
}

#[cfg(test)]
mod tests {
    use gdl_files::{ROMDISK_NAMES, RelPath};
    use gdl_fixtures::{BUILDER_UNKNOWN_WORD, RomFile, reorder, rom, set_word};

    use super::{
        COMPRESSED, HEADER_WORDS_AT, RECORD_WORDS_LEN, ROMDISK_HEADER_LEN, ROMDISK_LIMITS,
        ROMDISK_MAGIC, Romdisk, RomdiskLimits, read,
    };
    use crate::vectors::{frozen, n64_golden, n64_vectors, named, number, text, word};
    use crate::{MAX_ROM_LEN, MediaError, N64Rom, Sha256Digest};

    /// How the extractor names the default image's map in its refusals.
    const MAP: &str = "levels/test/worlds.wad";

    /// A ROM whose ROMDISK holds a plain file of one byte at each native path, as the extractor's
    /// tests build one for a path case. Plain files make the same bytes in either builder.
    fn paths_rom(paths: &[&str]) -> Vec<u8> {
        let files: Vec<RomFile> = paths
            .iter()
            .map(|path| RomFile::plain(path, b"x"))
            .collect();
        rom(&files).bytes
    }

    /// The paths a ROMDISK lists, in its order.
    fn paths_of(romdisk: &Romdisk<'_>) -> Vec<String> {
        romdisk
            .entries
            .iter()
            .map(|entry| entry.path.as_str().to_owned())
            .collect()
    }

    /// Why the ROMDISK `image` holds is refused under the extractor's limits: by a rule of the
    /// format, never by a failure to read.
    fn refusal(image: &[u8]) -> MediaError {
        let error = read(image, &ROMDISK_LIMITS).unwrap_err();
        assert!(matches!(error, MediaError::Rom { .. }), "{error:?}");
        error
    }

    /// What a case is expected to give: the rule and the words it is refused with, or none when it
    /// is taken.
    type Expected<'a> = Option<(&'a str, &'a str)>;

    /// What a file decodes to, or the rule and the words its decoding is refused with.
    type Decoded = Result<Vec<u8>, (&'static str, String)>;

    /// Checks that `outcome` is the refusal `expected` gives, by its rule and its words, or that
    /// it is taken when `expected` is none.
    fn assert_outcome(outcome: Result<Romdisk<'_>, MediaError>, expected: Expected<'_>) {
        match (outcome, expected) {
            (Ok(_), None) => {}
            (Err(error), Some((rule, detail))) => {
                assert_eq!(error.rule(), rule, "{error}");
                assert_eq!(error.to_string(), detail);
            }
            (outcome, expected) => panic!("{outcome:?} where {expected:?} was expected"),
        }
    }

    /// Checks that what a ROMDISK of `image` lists is safe to extract: names the tree may hold, of
    /// files within the limits whose bytes lie within the ROM.
    fn assert_harmless(romdisk: &Romdisk<'_>, image: &[u8]) {
        let mut total = 0;
        for entry in &romdisk.entries {
            let path = entry.path.as_str();
            let parsed = RelPath::parse(path, &ROMDISK_NAMES);
            assert_eq!(parsed.as_ref(), Ok(&entry.path));
            assert!(entry.flags == 0 || entry.flags == COMPRESSED, "{path}");
            assert!(u64::from(entry.size) <= ROMDISK_LIMITS.max_file, "{path}");
            let stored = romdisk.stored(entry);
            assert_eq!(stored.len(), entry.stored_size as usize, "{path}");
            assert!(entry.rom_offset as usize + stored.len() <= image.len());
            total += u64::from(entry.size);
        }
        assert!(total <= ROMDISK_LIMITS.max_total);
    }

    #[test]
    fn the_default_image_lists_its_two_files() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        let romdisk = read(&image, &ROMDISK_LIMITS).unwrap();
        assert_eq!(romdisk.offset, 64);
        assert_eq!(paths_of(&romdisk), [MAP, "texture.bin"]);
        let [map, texture] = &romdisk.entries[..] else {
            panic!("{romdisk:?}");
        };
        // The map is kept raw-DEFLATEd, its stream right past the header; the texture as it is.
        assert_eq!((map.record_offset, map.rom_offset), (152, 113));
        assert_eq!((map.flags, map.stored_size, map.size), (COMPRESSED, 23, 24));
        assert_eq!((texture.record_offset, texture.rom_offset), (196, 136));
        assert_eq!(
            (texture.flags, texture.stored_size, texture.size),
            (0, 6, 6)
        );
        // Each record's last word is kept as it is found.
        assert_eq!(
            [map.unknown_word, texture.unknown_word],
            [BUILDER_UNKNOWN_WORD; 2]
        );
        assert_eq!(romdisk.stored(map), &image[113..136]);
        assert_eq!(romdisk.stored(texture), b"native");
        // Its debugging form tells of the directory, not of the ROM's bytes, the first of which
        // are 128, 55, 18 and 64.
        let shown = format!("{romdisk:?}");
        assert!(shown.contains(MAP) && !shown.contains("128, 55"), "{shown}");

        // The ROM's ROMDISK is read in the cartridge's own order, whatever order its dump keeps.
        for transform in ["pairs", "words"] {
            let dump = reorder(&image, transform);
            let cartridge = N64Rom::from_bytes(named("dump.v64"), dump).unwrap();
            let again = cartridge.romdisk(&ROMDISK_LIMITS).unwrap();
            assert_eq!(again.offset, romdisk.offset, "{transform}");
            assert_eq!(again.entries, romdisk.entries, "{transform}");
        }

        // The magic may lie anywhere, and every place the ROMDISK gives counts from it.
        let mut shifted = vec![0; 8];
        shifted.extend_from_slice(&image);
        let moved = read(&shifted, &ROMDISK_LIMITS).unwrap();
        assert_eq!(moved.offset, 72);
        assert_eq!(moved.entries.len(), romdisk.entries.len());
        for (entry, moved_entry) in romdisk.entries.iter().zip(&moved.entries) {
            assert_eq!(moved_entry.path, entry.path);
            assert_eq!(moved_entry.record_offset, entry.record_offset + 8);
            assert_eq!(moved_entry.rom_offset, entry.rom_offset + 8);
            assert_eq!(moved.stored(moved_entry), romdisk.stored(entry));
        }
    }

    #[test]
    fn every_entry_is_what_the_extractors_manifest_records() {
        let vectors = n64_vectors();
        let extractions = vectors["extractions"].as_array().unwrap();
        // Both frozen images, each as the extractor read it.
        let images: Vec<&str> = extractions
            .iter()
            .map(|extraction| text(&extraction["image"]))
            .collect();
        assert_eq!(images, ["default", "mixed"]);
        for extraction in extractions {
            let image = frozen(&vectors, text(&extraction["image"]));
            let golden = n64_golden(&extraction["golden"]);
            let name = named(text(&extraction["name"]));
            let cartridge = N64Rom::from_bytes(name, image).unwrap();
            let romdisk = cartridge.romdisk(&ROMDISK_LIMITS).unwrap();
            assert_eq!(golden["romdisk_offset"], romdisk.offset);
            let rows = golden["files"].as_array().unwrap();
            assert_eq!(rows.len(), romdisk.entries.len());
            for (row, entry) in rows.iter().zip(&romdisk.entries) {
                let path = entry.path.as_str();
                assert_eq!(row["path"], path);
                assert_eq!(row["record_offset"], entry.record_offset, "{path}");
                assert_eq!(row["rom_offset"], entry.rom_offset, "{path}");
                assert_eq!(row["flags"], entry.flags, "{path}");
                assert_eq!(row["stored_size"], entry.stored_size, "{path}");
                assert_eq!(row["size"], entry.size, "{path}");
                assert_eq!(row["unknown_word"], entry.unknown_word, "{path}");
                let stored = Sha256Digest::of(romdisk.stored(entry));
                assert_eq!(row["stored_sha256"], stored.hex(), "{path}");
                // Decoded, each file is the bytes the extractor wrote, compressed or not.
                let decoded = romdisk.decode(entry).unwrap();
                assert_eq!(row["size"], decoded.len(), "{path}");
                let digest = Sha256Digest::of(&decoded);
                assert_eq!(row["sha256"], digest.hex(), "{path}");
            }
        }
    }

    #[test]
    fn a_file_decodes_to_exactly_the_size_its_record_gives() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        // Where the map's and the texture's records start, and where the map's stream does.
        let (map, texture, stream) = (152, 196, 113);
        let with = |changes: &[(usize, u32)]| {
            let mut changed = image.clone();
            for &(at, value) in changes {
                set_word(&mut changed, at, value);
            }
            changed
        };
        // What each file decodes to. The directory is read whatever the streams hold: a file's
        // bytes are looked at only as it is decoded.
        let decoded = |changed: &[u8]| -> Vec<Decoded> {
            let romdisk = read(changed, &ROMDISK_LIMITS).unwrap();
            romdisk
                .entries
                .iter()
                .map(|entry| {
                    let outcome = romdisk.decode(entry);
                    outcome.map_err(|error| (error.rule(), error.to_string()))
                })
                .collect()
        };
        let texture_bytes: Decoded = Ok(b"native".to_vec());

        // The map inflates to its 24 bytes; the texture is kept as it is stored.
        let outcomes = decoded(&image);
        assert_eq!(outcomes[0].as_ref().map(Vec::len), Ok(24));
        assert_eq!(outcomes[1], texture_bytes);

        // A size short of what the stream holds or past it, a stream cut short of its end, and
        // one followed by the texture's bytes are refused in the extractor's words.
        let wrong_end = (
            "deflateEnd",
            "DEFLATE output size or end marker does not match its directory",
        );
        let after_end = (
            "deflateTrailer",
            "unexpected bytes after ROMDISK DEFLATE stream",
        );
        let cases = [
            (map + 8, 23, wrong_end),
            (map + 8, 25, wrong_end),
            (map + 4, 22, wrong_end),
            (map + 4, 24, after_end),
            (map + 4, 30, after_end),
        ];
        for (at, value, (rule, detail)) in cases {
            let outcomes = decoded(&with(&[(at, value)]));
            assert_eq!(outcomes[0], Err((rule, detail.to_owned())), "{at} {value}");
            assert_eq!(outcomes[1], texture_bytes);
        }

        // A stream whose first block is of no type is refused in the launcher's own words, which
        // give the reason the decoder gives.
        let no_type = "invalid raw DEFLATE stream: deflate decompression error: invalid block type";
        let refused = Err(("deflate", no_type.to_owned()));
        let mut damaged = image.clone();
        damaged[stream] = 0xFF;
        let outcomes = decoded(&damaged);
        assert_eq!(outcomes[0], refused);
        assert_eq!(outcomes[1], texture_bytes);
        // The texture flagged compressed: its stored bytes are no stream.
        let outcomes = decoded(&with(&[(texture, COMPRESSED)]));
        assert_eq!(outcomes[0].as_ref().map(Vec::len), Ok(24));
        assert_eq!(outcomes[1], refused);
        // The map flagged plain, storing its size: its stream is kept as it is, with the texture's
        // first byte after it.
        let outcomes = decoded(&with(&[(map, 0), (map + 4, 24)]));
        assert_eq!(outcomes[0], Ok(image[stream..stream + 24].to_vec()));
        assert_eq!(outcomes[1], texture_bytes);
    }

    #[test]
    fn one_header_and_only_one() {
        let vectors = n64_vectors();
        let mut twice = frozen(&vectors, "default");
        twice.extend_from_slice(ROMDISK_MAGIC);
        for image in [vec![0; 512], twice] {
            let error = refusal(&image);
            assert_eq!(error.rule(), "romdiskHeader");
            assert_eq!(
                error.to_string(),
                "expected exactly one native ROMDISK header"
            );
        }

        // The header holds its words: a magic a byte short of room for them reaches past the ROM,
        // and one with room for them is read, here its words all zero, so that it lists no file.
        let mut short = vec![0; 64];
        short.extend_from_slice(ROMDISK_MAGIC);
        short.resize(64 + ROMDISK_HEADER_LEN as usize - 1, 0);
        let error = refusal(&short);
        assert_eq!(error.rule(), "outsideData");
        assert_eq!(
            error.to_string(),
            "ROMDISK header extends outside its containing data"
        );
        short.push(0);
        assert_eq!(refusal(&short).rule(), "directoryCount");
    }

    #[test]
    fn paths_follow_the_shared_vectors() {
        let vectors = n64_vectors();
        let cases = vectors["paths"].as_array().unwrap();
        assert!(!cases.is_empty());
        let mut deviations = Vec::new();
        for case in cases {
            let path = text(&case["path"]);
            let image = paths_rom(&[path]);
            match case["rule"].as_str() {
                None => {
                    // Taken, as the extractor takes it, without its leading slash.
                    assert!(case["python"].is_null(), "{case}");
                    let romdisk = read(&image, &ROMDISK_LIMITS)
                        .unwrap_or_else(|error| panic!("{path:?}: {error}"));
                    assert_eq!(paths_of(&romdisk), [&path[1..]]);
                }
                Some(rule) => {
                    let error = refusal(&image);
                    assert_eq!(error.rule(), rule, "{case}");
                    let detail = error.to_string();
                    match case["nameRule"].as_str() {
                        // The launcher's own words, which name the rule of names the path breaks.
                        Some(name_rule) => {
                            let said = format!("unsafe ROMDISK path: {path:?} ({name_rule})");
                            assert_eq!(detail, said, "{case}");
                        }
                        // The extractor's own words, which are fixed.
                        None => assert_eq!(case["python"], detail, "{case}"),
                    }
                }
            }
            deviations.extend(case["deviation"].as_u64());
        }
        // The paths the extractor takes and the launcher refuses: a delete character, an alpha
        // installer's name, a device name followed by spaces, and names longer than a disc's.
        deviations.sort_unstable();
        deviations.dedup();
        assert_eq!(deviations, [1, 2, 3, 4]);
    }

    #[test]
    fn case_aliases_and_file_folder_collisions_are_refused() {
        let vectors = n64_vectors();
        let cases = vectors["collisions"].as_array().unwrap();
        assert!(!cases.is_empty());
        for case in cases {
            let paths: Vec<&str> = case["paths"].as_array().unwrap().iter().map(text).collect();
            let kept: Vec<&str> = paths.iter().map(|path| &path[1..]).collect();
            let image = paths_rom(&paths);
            // What the reader decides; the extraction then plans the tree from the files it takes.
            match case["romdiskRule"].as_str() {
                None => {
                    let romdisk = read(&image, &ROMDISK_LIMITS).unwrap();
                    assert_eq!(paths_of(&romdisk), kept);
                }
                Some(rule) => {
                    let error = refusal(&image);
                    assert_eq!(error.rule(), rule, "{case}");
                    let detail = error.to_string();
                    if rule == "duplicatePath" {
                        assert_eq!(case["python"], detail, "{case}");
                    } else {
                        // The refusal names the path that would have to be a file and a folder.
                        let both = detail.strip_prefix("ROMDISK file/directory collision: ");
                        assert!(both.is_some_and(|both| kept.contains(&both)), "{detail}");
                    }
                }
            }
        }

        // Whichever comes first, in any letter case and at any depth. A folder spelled in several
        // cases, and names that only begin alike, are the reader's to take.
        let collision = "collision";
        let duplicate = "duplicatePath";
        let more: [(&[&str], Expected<'_>); 8] = [
            (
                &["/LIB", "/lib/a.so"],
                Some((collision, "ROMDISK file/directory collision: LIB")),
            ),
            (
                &["/lib/a.so", "/LIB"],
                Some((collision, "ROMDISK file/directory collision: LIB")),
            ),
            (
                &["/a", "/A/b/c/d"],
                Some((collision, "ROMDISK file/directory collision: a")),
            ),
            (
                &["/folder/file", "/folder"],
                Some((collision, "ROMDISK file/directory collision: folder")),
            ),
            (
                &["/x", "/X"],
                Some((duplicate, "duplicate case-insensitive ROMDISK path: X")),
            ),
            (
                &["/a/b", "/A/B"],
                Some((duplicate, "duplicate case-insensitive ROMDISK path: A/B")),
            ),
            (&["/Font/a", "/FONT/b", "/font/c"], None),
            (&["/lib", "/lib-x/a", "/library/a", "/lib.so"], None),
        ];
        for (paths, expected) in more {
            assert_outcome(read(&paths_rom(paths), &ROMDISK_LIMITS), expected);
        }
    }

    #[test]
    fn record_words_follow_the_shared_vectors() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        let entries = read(&image, &ROMDISK_LIMITS).unwrap().entries;
        let cases = vectors["recordWords"].as_array().unwrap();
        assert!(!cases.is_empty());
        for case in cases {
            let record = entries[number(&case["index"])].record_offset as usize;
            let mut changed = image.clone();
            set_word(
                &mut changed,
                record + 4 * number(&case["word"]),
                word(&case["value"]),
            );
            let error = refusal(&changed);
            assert_eq!(case["rule"], error.rule(), "{case}");
            assert_eq!(case["python"], error.to_string(), "{case}");
        }
    }

    #[test]
    fn record_words_are_taken_up_to_their_bounds() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        // Where the map's and the texture's records start; the pointer table starts at 144.
        let (map, texture, table) = (152, 196, 144);
        let with = |changes: &[(usize, u32)]| {
            let mut changed = image.clone();
            for &(at, value) in changes {
                set_word(&mut changed, at, value);
            }
            changed
        };
        let taken = |changed: &[u8]| {
            let romdisk = read(changed, &ROMDISK_LIMITS).unwrap();
            assert_harmless(&romdisk, changed);
            romdisk.entries
        };

        // A record's last word is kept whatever it holds.
        let entries = taken(&with(&[(map + 16, 0xDEAD_BEEF)]));
        assert_eq!(entries[0].unknown_word, 0xDEAD_BEEF);
        // A file flagged compressed is left to its decoding.
        let changed = with(&[(texture, COMPRESSED)]);
        let romdisk = read(&changed, &ROMDISK_LIMITS).unwrap();
        assert_eq!(romdisk.entries[1].flags, COMPRESSED);
        assert_eq!(romdisk.stored(&romdisk.entries[1]), b"native");
        // A file's bytes may start right past the header, and end right at the pointer table,
        // there with none stored.
        let entries = taken(&with(&[(map + 12, ROMDISK_HEADER_LEN as u32)]));
        assert_eq!(entries[0].rom_offset, 112);
        let changed = with(&[(map + 12, table - 64), (map + 4, 0)]);
        let romdisk = read(&changed, &ROMDISK_LIMITS).unwrap();
        assert_eq!(romdisk.entries[0].rom_offset, table);
        assert!(romdisk.stored(&romdisk.entries[0]).is_empty());
        let entries = taken(&with(&[(map + 4, table - 113)]));
        assert_eq!(entries[0].stored_size, table - 113);

        // A byte further reaches into the table, and a map kept as it is must store its size.
        let outside = format!("ROMDISK file {MAP} extends outside its containing data");
        let further = with(&[(map + 4, table - 113 + 1)]);
        let expected = Some(("outsideData", outside.as_str()));
        assert_outcome(read(&further, &ROMDISK_LIMITS), expected);
        let plain = format!("invalid ROMDISK file size: {MAP}");
        let expected = Some(("fileSize", plain.as_str()));
        assert_outcome(read(&with(&[(map, 0)]), &ROMDISK_LIMITS), expected);
    }

    #[test]
    fn the_expansion_limit_is_injectable() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        let cases = vectors["limits"].as_array().unwrap();
        assert!(!cases.is_empty());
        for case in cases {
            let limits = RomdiskLimits {
                max_total: case["maxTotal"].as_u64().unwrap(),
                ..ROMDISK_LIMITS
            };
            let error = read(&image, &limits).unwrap_err();
            assert_eq!(case["rule"], error.rule(), "{case}");
            assert_eq!(case["python"], error.to_string(), "{case}");
        }

        // The extractor's limits, each taken at its bound and injectable alike: the image's two
        // files expand to 30 bytes in all, the larger of them to 24.
        assert_eq!(
            ROMDISK_LIMITS,
            RomdiskLimits {
                max_entries: 65_536,
                max_file: 64 << 20,
                max_total: 512 << 20,
            }
        );
        let too_far = "ROMDISK expanded size exceeds extraction limit";
        let too_large = "invalid ROMDISK file size: levels/test/worlds.wad";
        let too_many = "unsupported ROMDISK header or directory count";
        let bounds = [
            (
                RomdiskLimits {
                    max_total: 30,
                    ..ROMDISK_LIMITS
                },
                None,
            ),
            (
                RomdiskLimits {
                    max_total: 29,
                    ..ROMDISK_LIMITS
                },
                Some(("expandedSize", too_far)),
            ),
            (
                RomdiskLimits {
                    max_file: 24,
                    ..ROMDISK_LIMITS
                },
                None,
            ),
            (
                RomdiskLimits {
                    max_file: 23,
                    ..ROMDISK_LIMITS
                },
                Some(("fileSize", too_large)),
            ),
            (
                RomdiskLimits {
                    max_entries: 2,
                    ..ROMDISK_LIMITS
                },
                None,
            ),
            (
                RomdiskLimits {
                    max_entries: 1,
                    ..ROMDISK_LIMITS
                },
                Some(("directoryCount", too_many)),
            ),
        ];
        for (limits, expected) in bounds {
            assert_outcome(read(&image, &limits), expected);
        }
    }

    #[test]
    fn header_words_follow_the_shared_vectors() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        let cases = vectors["headerWords"].as_array().unwrap();
        assert!(!cases.is_empty());
        for case in cases {
            let mut changed = image.clone();
            match case.get("copy") {
                // A pointer made a copy of another, `[to, from]`.
                Some(copy) => {
                    let (to, from) = (number(&copy[0]), number(&copy[1]));
                    changed.copy_within(from..from + 4, to);
                }
                None => set_word(&mut changed, number(&case["offset"]), word(&case["value"])),
            }
            let error = refusal(&changed);
            assert_eq!(case["rule"], error.rule(), "{case}");
            assert_eq!(case["python"], error.to_string(), "{case}");
        }
    }

    #[test]
    fn the_pointer_table_lies_past_the_header_and_within_the_directory() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        // Where the header keeps the directory's length, the table's offset and the count of
        // files, from 96, and where the first pointer lies.
        let length_at = 64 + HEADER_WORDS_AT as usize;
        let (table_at, count_at, first_pointer_at) = (length_at + 4, length_at + 8, 144);
        let entry = "ROMDISK entry extends outside its containing data";
        let cases = [
            // A table inside the header, checked once the table is known to lie in the ROM.
            (
                table_at,
                40,
                Some(("directoryOverlap", "ROMDISK directory overlaps its header")),
            ),
            (
                table_at,
                0,
                Some(("directoryOverlap", "ROMDISK directory overlaps its header")),
            ),
            // A table right past the header, whose first pointer is then a word of the map's
            // stream.
            (
                table_at,
                ROMDISK_HEADER_LEN as u32,
                Some(("outsideData", entry)),
            ),
            // A directory too short for its pointers, or for the record they point at.
            (
                length_at,
                4,
                Some((
                    "outsideData",
                    "ROMDISK pointer table extends outside its containing data",
                )),
            ),
            (length_at, 8, Some(("outsideData", entry))),
            (
                count_at,
                0,
                Some((
                    "directoryCount",
                    "unsupported ROMDISK header or directory count",
                )),
            ),
            // A record off a word: the first record, at 152, counted from the header, and two.
            (
                first_pointer_at,
                152 - 64 + 2,
                Some((
                    "recordPlace",
                    "ROMDISK records overlap or are not word aligned",
                )),
            ),
        ];
        for (at, value, expected) in cases {
            let mut changed = image.clone();
            set_word(&mut changed, at, value);
            assert_outcome(read(&changed, &ROMDISK_LIMITS), expected);
        }
    }

    #[test]
    fn an_unterminated_name_is_refused() {
        let built = rom(&[RomFile::plain("/x", b"data")]);
        // The name's NUL and padding overwritten, so that it runs to the ROM's end.
        let mut image = built.bytes.clone();
        let len = image.len();
        image[len - 2..].copy_from_slice(b"xx");
        let error = refusal(&image);
        assert_eq!(error.rule(), "unterminatedPath");
        assert_eq!(error.to_string(), "unterminated ROMDISK path");

        // The name must end within the directory, which ROMs of plain files lay out alike in
        // either builder: its pointer, then its record's words, then the name, a NUL and padding.
        // A NUL past the directory's end does not end it.
        let name_at = built.records[0] - built.table + RECORD_WORDS_LEN as usize;
        let with_directory = |directory_len: usize| {
            let mut cut = built.bytes.clone();
            let at = built.base + HEADER_WORDS_AT as usize;
            set_word(&mut cut, at, u32::try_from(directory_len).unwrap());
            cut
        };
        let unterminated = Some(("unterminatedPath", "unterminated ROMDISK path"));
        let entry = "ROMDISK entry extends outside its containing data";
        let cases = [
            (name_at + 4, None),
            (name_at + 3, None),
            (name_at + 2, unterminated),
            (name_at, unterminated),
            (name_at - 1, Some(("outsideData", entry))),
        ];
        for (directory_len, expected) in cases {
            assert_outcome(
                read(&with_directory(directory_len), &ROMDISK_LIMITS),
                expected,
            );
        }
    }

    #[test]
    fn a_non_ascii_name_is_refused() {
        // Every byte outside ASCII, wherever it stands in the name, the leading slash's place
        // included, since a name is read as ASCII before its slash is looked at.
        let built = rom(&[RomFile::plain("/cafe", b"x")]);
        let name_at = built.records[0] + RECORD_WORDS_LEN as usize;
        for at in name_at..name_at + "/cafe".len() {
            for byte in 0x80..=0xFF {
                let mut image = built.bytes.clone();
                image[at] = byte;
                let error = refusal(&image);
                assert_eq!(error.rule(), "nonAsciiPath", "{at} {byte:#x}");
                assert_eq!(error.to_string(), "non-ASCII ROMDISK path");
            }
        }
        // A character outside ASCII written as UTF-8, and one with no slash before it.
        for path in ["/caf\u{e9}", "/\u{1d11e}", "\u{e9}"] {
            let error = refusal(&rom(&[RomFile::plain(path, b"x")]).bytes);
            assert_eq!(error.rule(), "nonAsciiPath", "{path}");
        }
    }

    #[test]
    fn every_byte_flip_in_the_directory_is_refused_or_harmless() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        assert_eq!(image.len(), 232);
        let mut refused = 0;
        for at in 64..image.len() {
            let mut flipped = image.clone();
            flipped[at] ^= 0x01;
            match read(&flipped, &ROMDISK_LIMITS) {
                Ok(romdisk) => assert_harmless(&romdisk, &flipped),
                Err(error) => {
                    assert!(matches!(error, MediaError::Rom { .. }), "{at}: {error:?}");
                    refused += 1;
                }
            }
        }
        // The magic's bytes alone refuse a ROM when flipped.
        assert!(refused >= ROMDISK_MAGIC.len(), "{refused}");
    }

    #[test]
    fn every_word_of_the_directory_at_a_bound_is_refused_or_harmless() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        let len = u32::try_from(image.len()).unwrap();
        let bounds = [
            0,
            1,
            3,
            4,
            47,
            48,
            49,
            len - 1,
            len,
            len + 1,
            0x7FFF_FFFF,
            0x8000_0000,
            0xFFFF_FFFC,
            0xFFFF_FFFF,
        ];
        // Each word from the header's to the last record's, set to each value.
        let header_words = 64 + HEADER_WORDS_AT as usize;
        for at in (header_words..image.len()).step_by(4) {
            for value in bounds {
                let mut changed = image.clone();
                set_word(&mut changed, at, value);
                match read(&changed, &ROMDISK_LIMITS) {
                    Ok(romdisk) => assert_harmless(&romdisk, &changed),
                    Err(error) => assert!(matches!(error, MediaError::Rom { .. }), "{error:?}"),
                }
            }
        }
    }

    #[test]
    fn a_rom_longer_than_the_largest_cartridge_is_refused() {
        // Zeros the reader never looks at: their length refuses them first.
        let long = vec![0; MAX_ROM_LEN as usize + 4];
        let error = read(&long, &ROMDISK_LIMITS).unwrap_err();
        assert_eq!(error.rule(), "romTooLarge");
        assert_eq!(
            error.to_string(),
            "ROM is longer than 67108864 bytes, the most an N64 cartridge holds"
        );
    }
}
