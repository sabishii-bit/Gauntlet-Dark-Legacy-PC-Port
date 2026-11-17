//! SIZES.ROM, the table of the files DISK.ROM packs, read as the Dreamcast extractor reads it: a
//! count of rows, then a row for each file, of its size, where its bytes start in DISK.ROM, or -1
//! where the disc keeps it outside DISK.ROM, and where each component of its path starts in the
//! pool of names that follows the whole table, the first negative place ending them; then that
//! pool, each name ended by a NUL. Each component is judged by itself, as one name the extracted
//! tree may hold, before it is joined to the others, so that no component names a place outside
//! the tree or stands for a folder and the file within it; and no two paths may share a name on a
//! case-insensitive file system or be a file and a folder at once.

use std::array;

use gdl_files::{DISC_NAMES, NameSet, RelPath, check_component};

use super::unsafe_name;
use crate::error::quoted;
use crate::{DiscRule, MediaError};

/// The most bytes SIZES.ROM may hold for its table to be read: far more than any disc's table of
/// files needs, so that a disc cannot have its reader take more than this into memory at once.
pub const MAX_SIZES_ROM_LEN: u32 = 16 << 20;

/// How many bytes a row of the table takes: its file's size, its file's place in DISK.ROM, and
/// where each component of its path starts in the pool.
pub const SIZES_ROW_LEN: usize = 16;

/// How many components a row may give its path, each by where it starts in the pool.
pub const SIZES_COMPONENTS: usize = 4;

/// How many bytes the count of rows takes, which the table begins with, little-endian.
const COUNT_LEN: usize = size_of::<u32>();

/// Where a row gives its file's size, unsigned and little-endian.
const SIZE_AT: usize = 0;

/// Where a row gives its file's place in DISK.ROM, signed and little-endian.
const OFFSET_AT: usize = 4;

/// Where a row gives where its components start in the pool, each signed and little-endian.
const COMPONENTS_AT: usize = 8;

/// How many bytes the place of each component takes.
const COMPONENT_PLACE_LEN: usize = size_of::<i16>();

/// The place a row gives a file that the disc keeps outside DISK.ROM, or not at all.
const EXTERNAL: i32 = -1;

/// The byte that ends each name of the pool.
const NAME_END: u8 = 0;

/// What comes between a path's components.
const PATH_SEPARATOR: &str = "/";

/// What the Dreamcast extractor answers for a SIZES.ROM shorter than its count of rows.
const TRUNCATED_HEADER: &str = "Truncated SIZES.ROM header";

/// What it answers for a table that counts no row, or more rows than its file holds.
const TABLE_OUTSIDE: &str = "SIZES.ROM table lies outside its file";

/// What it answers for a component that starts past the pool's end, or that no NUL ends within
/// the pool.
const NAME_OUTSIDE: &str = "SIZES.ROM name lies outside its string pool";

/// What it answers for a row that gives its path no component.
const NO_NAME: &str = "SIZES.ROM entry has no name";

/// What it answers for a path a row before gives already.
const DUPLICATE_PATH: &str = "Duplicate SIZES.ROM path";

/// What it answers, before the path, for a file whose bytes do not lie within DISK.ROM.
const EXTENT_OUTSIDE: &str = "SIZES.ROM extent lies outside DISK.ROM";

/// What it answers for a path that is also the folder above another.
const PATH_CONFLICT: &str = "SIZES.ROM file/directory path conflict";

/// What the reader answers, before the name, for a component that holds a byte outside ASCII.
const NON_ASCII_NAME: &str = "SIZES.ROM name holds a byte outside ASCII";

/// Where a file SIZES.ROM lists keeps its bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PackedPlace {
    /// In DISK.ROM, every byte of the file within it.
    Disk {
        /// Where the file's bytes start in DISK.ROM.
        offset: u64,
    },
    /// Outside DISK.ROM: the disc keeps the file elsewhere, or not at all, so the table only names
    /// it, and its size is not bounded by DISK.ROM's.
    External,
}

/// A file SIZES.ROM lists: its path in the tree the native files are restored to, where it keeps
/// its bytes, and how many there are.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PackedFile {
    /// Its path, every component of it a name the extracted tree may hold.
    pub path: RelPath,
    /// Where it keeps its bytes.
    pub place: PackedPlace,
    /// How many bytes it holds, as its row gives them.
    pub size: u32,
}

/// The files `sizes`, the bytes of SIZES.ROM, lists, in its order, read as the Dreamcast extractor
/// reads the table against a DISK.ROM of `disk_len` bytes, or the first rule the table breaks. The
/// table must count a row or more, and no more rows than its file holds. Each row's components,
/// up to the first negative place, are judged in turn: each must start within the pool and end at
/// a NUL there, hold only ASCII, and be a name the extracted tree may hold by itself. A row must
/// give its path a component; the path, its components joined by `/`, must be no longer than a
/// path the tree may hold and one no row before it gives in any letter case; and a file DISK.ROM
/// holds must lie within it. Once every row is read, no path may be the folder above another, a
/// file's outside DISK.ROM included. The table is read from memory, so a caller holds it to
/// [`MAX_SIZES_ROM_LEN`] before reading it from the disc.
pub fn packed_files(sizes: &[u8], disk_len: u64) -> Result<Vec<PackedFile>, MediaError> {
    let (count, rest) = sizes
        .split_first_chunk::<COUNT_LEN>()
        .ok_or_else(|| MediaError::disc(DiscRule::SizesHeader, TRUNCATED_HEADER))?;
    let count = usize::try_from(u32::from_le_bytes(*count))
        .ok()
        .filter(|&count| count != 0 && count <= rest.len() / SIZES_ROW_LEN)
        .ok_or_else(|| MediaError::disc(DiscRule::SizesTable, TABLE_OUTSIDE))?;
    // The pool follows the whole table, and its places count from its own start.
    let (table, pool) = rest.split_at(count * SIZES_ROW_LEN);
    let (rows, _) = table.as_chunks::<SIZES_ROW_LEN>();
    let mut paths = NameSet::default();
    let mut files = Vec::new();
    for row in rows {
        files.push(packed_file(row, pool, disk_len, &mut paths)?);
    }
    paths
        .conflicts()
        .map_err(|_| MediaError::disc(DiscRule::SizesConflict, PATH_CONFLICT))?;
    Ok(files)
}

/// The file `row` lists, its components read from `pool` and its bytes held to a DISK.ROM of
/// `disk_len` bytes, with its path taken into `paths`, the paths of the rows before it.
fn packed_file(
    row: &[u8; SIZES_ROW_LEN],
    pool: &[u8],
    disk_len: u64,
    paths: &mut NameSet,
) -> Result<PackedFile, MediaError> {
    let size = u32::from_le_bytes(bytes_at(row, SIZE_AT));
    let offset = i32::from_le_bytes(bytes_at(row, OFFSET_AT));
    // The first negative place ends the components, and the places after it are never read.
    let places = (0..SIZES_COMPONENTS)
        .map(|component| component_place(row, component))
        .map_while(|at| usize::try_from(at).ok());
    let mut components = Vec::with_capacity(SIZES_COMPONENTS);
    for at in places {
        components.push(component(pool, at)?);
    }
    if components.is_empty() {
        return Err(MediaError::disc(DiscRule::SizesNoName, NO_NAME));
    }
    let joined = components.join(PATH_SEPARATOR);
    let path = RelPath::parse(&joined, &DISC_NAMES)
        .map_err(|refused| unsafe_name(DiscRule::SizesNameUnsafe, &joined, refused.rule))?;
    paths
        .insert(path.clone())
        .map_err(|_| MediaError::disc(DiscRule::SizesDuplicate, DUPLICATE_PATH))?;
    let place = place(offset, size, disk_len).ok_or_else(|| {
        let detail = format!("{EXTENT_OUTSIDE}: {}", path.as_str());
        MediaError::disc(DiscRule::SizesExtent, detail)
    })?;
    Ok(PackedFile { path, place, size })
}

/// The component that starts `at` in `pool`, up to the NUL that ends it. Refused, in this order,
/// when it starts at or past the pool's end or no NUL ends it within the pool, when it holds a byte
/// outside ASCII, and when it is no name the extracted tree may hold by itself.
fn component(pool: &[u8], at: usize) -> Result<&str, MediaError> {
    let rest = pool.get(at..).unwrap_or_default();
    let end = rest
        .iter()
        .position(|&byte| byte == NAME_END)
        .ok_or_else(|| MediaError::disc(DiscRule::SizesName, NAME_OUTSIDE))?;
    let bytes = &rest[..end];
    let name = str::from_utf8(bytes)
        .ok()
        .filter(|name| name.is_ascii())
        .ok_or_else(|| non_ascii(bytes))?;
    check_component(name, &DISC_NAMES)
        .map_err(|refused| unsafe_name(DiscRule::SizesNameUnsafe, name, refused.rule))?;
    Ok(name)
}

/// Where a row that gives its file of `size` bytes the place `offset` puts the file, against a
/// DISK.ROM of `disk_len` bytes: outside DISK.ROM at [`EXTERNAL`], whatever its size; in DISK.ROM
/// at a place of nought or more where every byte of the file lies within it; and nowhere at any
/// other place.
fn place(offset: i32, size: u32, disk_len: u64) -> Option<PackedPlace> {
    if offset == EXTERNAL {
        return Some(PackedPlace::External);
    }
    let offset = u64::try_from(offset).ok()?;
    let within = offset <= disk_len && u64::from(size) <= disk_len - offset;
    within.then_some(PackedPlace::Disk { offset })
}

/// Where the component `component` of `row`'s path starts in the pool, or a negative number where
/// the components end before it.
fn component_place(row: &[u8; SIZES_ROW_LEN], component: usize) -> i16 {
    i16::from_le_bytes(bytes_at(
        row,
        COMPONENTS_AT + component * COMPONENT_PLACE_LEN,
    ))
}

/// The `N` bytes of `row` from `at`.
fn bytes_at<const N: usize>(row: &[u8; SIZES_ROW_LEN], at: usize) -> [u8; N] {
    array::from_fn(|byte| row[at + byte])
}

/// The refusal of `name`, a component that holds a byte outside ASCII and so reads as no name:
/// quoted as UTF-8 reads it, with U+FFFD for each run of bytes that is no UTF-8, up to the bound
/// every refusal quotes to.
fn non_ascii(name: &[u8]) -> MediaError {
    let detail = quoted(NON_ASCII_NAME, &String::from_utf8_lossy(name));
    MediaError::disc(DiscRule::SizesName, detail)
}

#[cfg(test)]
mod tests {
    use gdl_files::{DISC_NAMES, RelPath};
    use gdl_fixtures::{sizes_rom, unhex};
    use serde_json::{Value, json};

    use super::{MAX_SIZES_ROM_LEN, PackedPlace, SIZES_COMPONENTS, SIZES_ROW_LEN, packed_files};
    use crate::MediaError;
    use crate::error::MAX_QUOTED_CHARS;
    use crate::vectors::{dreamcast_golden, dreamcast_vectors, manifest_restored, packed_rows};
    use crate::vectors::{restored, text};

    /// The most bytes of SIZES.ROM the launcher reads. The format's facts are given here apart from
    /// the reader's own constants, so that a reader that looks elsewhere is caught.
    const MOST_TABLE_BYTES: u32 = 16 * 1024 * 1024;

    /// How many bytes a row of the table takes, as the disc's own program reads one: a size and a
    /// place of four bytes each, and a place of two bytes for each of its components.
    const FORMAT_ROW_LEN: usize = 16;

    /// How many components a row gives places for.
    const FORMAT_COMPONENTS: usize = 4;

    /// The place a row gives a file the disc keeps outside DISK.ROM.
    const OUTSIDE: i32 = -1;

    /// The place of a component that ends a row's components before it.
    const NO_COMPONENT: i16 = -1;

    /// The one row of the table the shared vectors give a name in: five bytes at DISK.ROM's start,
    /// whose path is the name alone, at the pool's start.
    const NAME_TABLE_ROW: (u32, i32, [i16; 4]) = (5, 0, [0, NO_COMPONENT, 0, 0]);

    /// The length of the DISK.ROM the shared vectors read a name's table against.
    const NAME_DISK_LEN: u64 = 5;

    /// What the extractor answers for a table shorter than its count of rows.
    const HEADER_WORDS: &str = "Truncated SIZES.ROM header";

    /// What it answers for a table that counts no row, or more rows than its file holds.
    const TABLE_WORDS: &str = "SIZES.ROM table lies outside its file";

    /// What it answers for a component that starts past the pool or that no NUL ends there.
    const POOL_WORDS: &str = "SIZES.ROM name lies outside its string pool";

    /// What it answers for a row that gives its path no component.
    const NO_NAME_WORDS: &str = "SIZES.ROM entry has no name";

    /// What it answers for a path a row before gives already.
    const DUPLICATE_WORDS: &str = "Duplicate SIZES.ROM path";

    /// What it answers for a path that is also the folder above another.
    const CONFLICT_WORDS: &str = "SIZES.ROM file/directory path conflict";

    /// What it answers, before the path, for a file whose bytes lie outside DISK.ROM.
    const EXTENT_WORDS: &str = "SIZES.ROM extent lies outside DISK.ROM";

    /// What the launcher answers, before the name, for a component that holds a byte outside
    /// ASCII.
    const NON_ASCII_WORDS: &str = "SIZES.ROM name holds a byte outside ASCII";

    /// What the reader gives a table: its files as the vectors list them, or the rule and the
    /// words it refuses the table with.
    type Outcome = Result<Value, (&'static str, String)>;

    /// What the reader gives `table`, read against a DISK.ROM of `disk_len` bytes: by a rule of
    /// the disc's format when it refuses it, never by a failure to read.
    fn outcome(table: &[u8], disk_len: u64) -> Outcome {
        match packed_files(table, disk_len) {
            Ok(files) => Ok(packed_rows(&files)),
            Err(error) => {
                assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
                Err((error.rule(), error.to_string()))
            }
        }
    }

    /// The refusal by `rule` with `words`.
    fn refused(rule: &'static str, words: &str) -> Outcome {
        Err((rule, words.to_owned()))
    }

    /// The refusal of `name`, a component or a path, which breaks the rule of names `name_rule`,
    /// in the launcher's own words.
    fn unsafe_name(name: &str, name_rule: &str) -> Outcome {
        Err((
            "sizesNameUnsafe",
            format!("Unsafe disc name: {name:?} ({name_rule})"),
        ))
    }

    /// The refusal of the file at `path`, whose bytes lie outside DISK.ROM.
    fn outside_disk(path: &str) -> Outcome {
        Err(("sizesExtent", format!("{EXTENT_WORDS}: {path}")))
    }

    /// The refusal of `name`, a component outside ASCII, which the launcher quotes as UTF-8 reads
    /// it.
    fn non_ascii(name: &[u8]) -> Outcome {
        let name = String::from_utf8_lossy(name);
        Err(("sizesName", format!("{NON_ASCII_WORDS}: {name:?}")))
    }

    /// The table of [`NAME_TABLE_ROW`] alone, whose one component is `name`, as the shared vectors
    /// give a name in it.
    fn name_table(name: &[u8]) -> Vec<u8> {
        sizes_rom(&[NAME_TABLE_ROW], &[name, b"\0"].concat())
    }

    /// A table of one row giving a file of `size` bytes at the place `offset`, whose path's
    /// components are `names`, in turn in the pool.
    fn path_table(names: &[&str], size: u32, offset: i32) -> Vec<u8> {
        let mut places = [NO_COMPONENT; 4];
        let mut pool = Vec::new();
        for (place, name) in places.iter_mut().zip(names) {
            *place = i16::try_from(pool.len()).unwrap();
            pool.extend_from_slice(name.as_bytes());
            pool.push(0);
        }
        sizes_rom(&[(size, offset, places)], &pool)
    }

    /// The files the vectors list for a table of one file at `path`, `size` bytes at `offset`.
    fn one_file(path: &str, offset: i64, size: u32) -> Outcome {
        Ok(json!([[path, offset, size]]))
    }

    #[test]
    fn the_tables_rows_and_its_bound_are_the_formats() {
        assert_eq!(
            (SIZES_ROW_LEN, SIZES_COMPONENTS),
            (FORMAT_ROW_LEN, FORMAT_COMPONENTS)
        );
        assert_eq!(MAX_SIZES_ROM_LEN, MOST_TABLE_BYTES);
        // A row gives its file's size first and its place next, each little-endian: the bytes 1
        // to 4 and 5 to 8, read against the largest DISK.ROM.
        let fields = path_table(&["a"], 0x0403_0201, 0x0807_0605);
        assert_eq!(fields[4..12], [1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(
            outcome(&fields, u64::MAX),
            one_file("a", 0x0807_0605, 0x0403_0201)
        );
    }

    #[test]
    fn tables_follow_the_shared_vectors() {
        let vectors = dreamcast_vectors();
        let sizes = &vectors["sizes"];
        // The extractor's tests' tables, its unpack test's among them, then the others.
        let named = [
            "firstNegativeComponent",
            "externalFile",
            "outOfBoundsPayload",
            "outOfBoundsName",
            "truncatedTable",
            "duplicateNames",
            "fileDirectoryConflict",
            "unpack",
        ];
        assert_eq!(sizes.as_object().unwrap().len(), named.len() + 1);
        let extras = sizes["extras"].as_array().unwrap();
        assert!(!extras.is_empty());
        let cases = named.iter().map(|name| &sizes[*name]).chain(extras);
        let (mut rules, mut deviations) = (Vec::new(), Vec::new());
        for case in cases {
            let table = unhex(&case["hex"]);
            let disk_len = case["diskLen"].as_u64().unwrap();
            let python = &case["python"];
            match (outcome(&table, disk_len), case["rule"].as_str()) {
                (Ok(files), None) => {
                    assert!(python.is_null(), "{case}");
                    assert_eq!(files, case["files"], "{case}");
                }
                (Err((rule, detail)), Some(expected)) => {
                    assert_eq!(rule, expected, "{case}");
                    match case["nameRule"].as_str() {
                        // The launcher's own words, which quote the name and name the rule of names
                        // it breaks.
                        Some(name_rule) => {
                            assert!(detail.starts_with("Unsafe disc name: "), "{detail}");
                            assert!(detail.ends_with(&format!(" ({name_rule})")), "{detail}");
                        }
                        // Every other refusal keeps the extractor's own words.
                        None => assert_eq!(detail, text(python), "{case}"),
                    }
                    // Where the extractor takes a table the launcher refuses, a deviation says why.
                    assert!(!python.is_null() || case["deviation"].is_u64(), "{case}");
                    rules.push(rule);
                }
                (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
            }
            deviations.extend(case["deviation"].as_u64());
        }
        rules.sort_unstable();
        rules.dedup();
        let every_rule = [
            "sizesConflict",
            "sizesDuplicate",
            "sizesExtent",
            "sizesHeader",
            "sizesName",
            "sizesNameUnsafe",
            "sizesNoName",
            "sizesTable",
        ];
        assert_eq!(rules, every_rule);
        // The tables the extractor takes and the launcher refuses: a path longer than the tree
        // may hold.
        assert_eq!(deviations, [2]);

        // The unpack test's table, read against its own DISK.ROM, places its file there where the
        // native manifest the extractor wrote for it records the file, and lists the one outside
        // DISK.ROM as that does.
        let unpack = &sizes["unpack"];
        let disk_len = unhex(&unpack["disk"]).len() as u64;
        assert_eq!(unpack["diskLen"], disk_len);
        let files = packed_files(&unhex(&unpack["hex"]), disk_len).unwrap();
        let golden = dreamcast_golden(&unpack["golden"]);
        assert_eq!(restored(&files), manifest_restored(&golden));
    }

    #[test]
    fn names_follow_the_shared_vectors() {
        let vectors = dreamcast_vectors();
        // The vectors' table for a name is the one the reader is given here.
        let extras = vectors["sizes"]["extras"].as_array().unwrap();
        let separator = extras
            .iter()
            .find(|case| case["name"] == "separatorInComponent")
            .unwrap();
        assert_eq!(name_table(b"a/b"), unhex(&separator["hex"]));

        let cases = vectors["names"].as_array().unwrap();
        assert!(!cases.is_empty());
        let mut deviations = Vec::new();
        for case in cases {
            let name = text(&case["name"]);
            let sizes = &case["sizes"];
            let (size, offset, _) = NAME_TABLE_ROW;
            match (
                outcome(&name_table(name.as_bytes()), NAME_DISK_LEN),
                sizes["rule"].as_str(),
            ) {
                (Ok(files), None) => {
                    assert!(sizes["python"].is_null(), "{case}");
                    assert_eq!(files, json!([[name, offset, size]]), "{case}");
                }
                (Err((rule, detail)), Some(expected)) => {
                    assert_eq!(rule, expected, "{case}");
                    let words = match sizes["nameRule"].as_str() {
                        // The launcher's own words, which quote the name and name the rule of
                        // names it breaks.
                        Some(name_rule) => format!("Unsafe disc name: {name:?} ({name_rule})"),
                        // A name outside ASCII reads as no name at all, so it is refused before
                        // any rule of names is judged.
                        None => format!("{NON_ASCII_WORDS}: {name:?}"),
                    };
                    assert_eq!(detail, words, "{case}");
                }
                (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
            }
            deviations.extend(sizes["deviation"].as_u64());
        }
        // The names the extractor takes and the launcher refuses: a delete character, a name of
        // more than 200 characters, the alpha installer's names, and a device name followed by
        // spaces. A name with a superscript digit is outside ASCII, which both refuse here.
        deviations.sort_unstable();
        deviations.dedup();
        assert_eq!(deviations, [1, 2, 3, 4]);
    }

    #[test]
    fn externals_count_in_duplicates_and_conflicts() {
        // A file outside DISK.ROM takes its path as one in it does: two of one path in two letter
        // cases, whichever is outside, are one path twice.
        let duplicate = refused("sizesDuplicate", DUPLICATE_WORDS);
        let places = [(OUTSIDE, 0), (0, OUTSIDE), (OUTSIDE, OUTSIDE)];
        for (first, second) in places {
            let rows = [(5, first, [0, -1, 0, 0]), (5, second, [5, -1, 0, 0])];
            let table = sizes_rom(&rows, b"file\0FILE\0");
            assert_eq!(outcome(&table, 5), duplicate, "{first} {second}");
        }
        // A path that is also the folder above another, whichever is outside DISK.ROM.
        let conflict = refused("sizesConflict", CONFLICT_WORDS);
        for (first, second) in places {
            let rows = [(5, first, [0, -1, 0, 0]), (5, second, [0, 2, -1, 0])];
            let table = sizes_rom(&rows, b"a\0b\0");
            assert_eq!(outcome(&table, 5), conflict, "{first} {second}");
        }
        // In another letter case too, and with the folder's file listed after the file in it.
        let rows = [(5, 0, [2, 0, -1, 0]), (5, OUTSIDE, [4, -1, 0, 0])];
        let table = sizes_rom(&rows, b"b\0a\0A\0");
        assert_eq!(outcome(&table, 5), conflict);

        // The paths are held apart once every row is read: a table whose first two rows conflict
        // is refused for what a later row breaks first.
        let rows = [
            (5, OUTSIDE, [0, -1, 0, 0]),
            (5, 0, [0, 2, -1, 0]),
            (5, 0, [0, 2, -1, 0]),
        ];
        assert_eq!(outcome(&sizes_rom(&rows, b"a\0b\0"), 5), duplicate);
        let rows = [
            (5, OUTSIDE, [0, -1, 0, 0]),
            (5, 0, [0, 2, -1, 0]),
            (6, 0, [4, -1, 0, 0]),
        ];
        assert_eq!(
            outcome(&sizes_rom(&rows, b"a\0b\0c\0"), 5),
            outside_disk("c")
        );
        // Files outside DISK.ROM beside each other, of any size, are listed in the table's order.
        let rows = [
            (u32::MAX, OUTSIDE, [0, -1, 0, 0]),
            (0, OUTSIDE, [2, -1, 0, 0]),
        ];
        let listed = json!([["b", -1, u32::MAX], ["a", -1, 0]]);
        assert_eq!(outcome(&sizes_rom(&rows, b"b\0a\0"), 0), Ok(listed));
    }

    #[test]
    fn names_outside_ascii_are_refused() {
        // Every byte past ASCII's, alone, between two letters, or ending a name, read as UTF-8
        // reads it.
        for byte in 0x80..=u8::MAX {
            for name in [vec![byte], vec![b'a', byte, b'b'], vec![b'a', b'b', byte]] {
                let given = outcome(&name_table(&name), NAME_DISK_LEN);
                assert_eq!(given, non_ascii(&name), "{byte:#x}");
            }
        }
        // A character outside ASCII written in UTF-8 is quoted as it is written.
        let name = "trak\u{e9}.bin";
        let given = outcome(&name_table(name.as_bytes()), NAME_DISK_LEN);
        assert_eq!(
            given,
            refused("sizesName", &format!("{NON_ASCII_WORDS}: {name:?}"))
        );
        // A name outside ASCII is refused for that before the rules of names are judged, whatever
        // else it breaks.
        for name in ["\u{e9}/..", "..\u{e9}", "CON\u{e9}"] {
            let given = outcome(&name_table(name.as_bytes()), NAME_DISK_LEN);
            assert_eq!(given, non_ascii(name.as_bytes()), "{name:?}");
        }
        // A long name is told by its length and its start.
        let long = "\u{e9}".repeat(MAX_QUOTED_CHARS + 1);
        let start = "\u{e9}".repeat(MAX_QUOTED_CHARS);
        let words = format!(
            "{NON_ASCII_WORDS} of {} characters beginning {start:?}",
            MAX_QUOTED_CHARS + 1
        );
        let given = outcome(&name_table(long.as_bytes()), NAME_DISK_LEN);
        assert_eq!(given, refused("sizesName", &words));
        // The components are judged in turn: one outside ASCII after an unsafe one, and before.
        let after = path_table(&["..", "\u{e9}"], 5, 0);
        assert_eq!(outcome(&after, 5), unsafe_name("..", "dot"));
        let before = path_table(&["\u{e9}", ".."], 5, 0);
        assert_eq!(outcome(&before, 5), non_ascii("\u{e9}".as_bytes()));
    }

    #[test]
    fn components_are_judged_alone_and_in_turn_and_paths_by_their_length() {
        // A component that holds a separator is one name, refused, never a folder holding a file;
        // two components are a folder and its file.
        let separator = path_table(&["a/b"], 5, 0);
        assert_eq!(outcome(&separator, 5), unsafe_name("a/b", "forbidden"));
        assert_eq!(
            outcome(&path_table(&["a", "b"], 5, 0), 5),
            one_file("a/b", 0, 5)
        );
        // Four components make a path of four; a name of the pool may be named twice.
        let four = path_table(&["a", "b", "c", "d"], 5, 0);
        assert_eq!(outcome(&four, 5), one_file("a/b/c/d", 0, 5));
        let twice = sizes_rom(&[(5, 0, [0, 0, 0, 0])], b"x\0");
        assert_eq!(outcome(&twice, 5), one_file("x/x/x/x", 0, 5));
        // The first negative place ends the components, whatever the places after it hold: one
        // past the pool, an unsafe name's, or another negative place.
        let ended = sizes_rom(&[(5, 0, [0, i16::MIN, 1_000, 3])], b"ok\0..\0");
        assert_eq!(outcome(&ended, 5), one_file("ok", 0, 5));
        let ended = sizes_rom(&[(5, 0, [0, 3, -2, 1_000])], b"ok\0go\0");
        assert_eq!(outcome(&ended, 5), one_file("ok/go", 0, 5));
        // With none before it, the row gives its path no component.
        for first in [NO_COMPONENT, i16::MIN] {
            let unnamed = sizes_rom(&[(5, 0, [first, 0, 0, 0])], b"x\0");
            assert_eq!(
                outcome(&unnamed, 5),
                refused("sizesNoName", NO_NAME_WORDS),
                "{first}"
            );
        }
        // Each component is judged before the next is looked for: an unsafe one before one past the
        // pool, and one past the pool before an unsafe one.
        let unsafe_first = sizes_rom(&[(5, 0, [0, 1_000, -1, 0])], b"..\0");
        assert_eq!(outcome(&unsafe_first, 5), unsafe_name("..", "dot"));
        let outside_first = sizes_rom(&[(5, 0, [1_000, 0, -1, 0])], b"..\0");
        assert_eq!(outcome(&outside_first, 5), refused("sizesName", POOL_WORDS));
        // The rules of names judge each component as the tree must hold it.
        let cases = [
            ("", "empty"),
            ("a\tb", "control"),
            ("a\u{7f}b", "control"),
            ("installation.json", "alphaName"),
            ("aux.wad", "deviceName"),
            ("a.", "trailingDotOrSpace"),
        ];
        for (name, name_rule) in cases {
            let table = path_table(&["levels", name], 5, 0);
            assert_eq!(outcome(&table, 5), unsafe_name(name, name_rule), "{name:?}");
        }
        // The joined path may be as long as one the tree holds, 220 characters, and no longer.
        let folders = ["a".repeat(54), "b".repeat(54), "c".repeat(54)];
        for (chars, file) in [(220, "d".repeat(55)), (221, "d".repeat(56))] {
            let names = [&folders[0], &folders[1], &folders[2], &file].map(String::as_str);
            let path = names.join("/");
            assert_eq!(path.len(), chars);
            let given = outcome(&path_table(&names, 5, 0), 5);
            if chars == 220 {
                assert_eq!(given, one_file(&path, 0, 5));
            } else {
                assert_eq!(given, unsafe_name(&path, "pathTooLong"));
            }
        }
    }

    #[test]
    fn a_rows_rules_are_judged_in_the_extractors_order() {
        // The last row of each table below also places its file past DISK.ROM's end, which is
        // judged last: after the row's components, after whether it has one, and after whether a
        // row before gives its path.
        let rows = [(6, 0, [0, -1, 0, 0])];
        assert_eq!(
            outcome(&sizes_rom(&rows, b"..\0"), 5),
            unsafe_name("..", "dot")
        );
        let rows = [(6, 0, [NO_COMPONENT, 0, 0, 0])];
        assert_eq!(
            outcome(&sizes_rom(&rows, b"x\0"), 5),
            refused("sizesNoName", NO_NAME_WORDS)
        );
        let rows = [(5, 0, [0, -1, 0, 0]), (6, 0, [2, -1, 0, 0])];
        assert_eq!(
            outcome(&sizes_rom(&rows, b"x\0X\0"), 5),
            refused("sizesDuplicate", DUPLICATE_WORDS)
        );
        // A row that breaks no other rule is refused for its place.
        let rows = [(6, 0, [0, -1, 0, 0])];
        assert_eq!(outcome(&sizes_rom(&rows, b"x\0"), 5), outside_disk("x"));
    }

    #[test]
    fn names_lie_in_the_pool_after_the_whole_table() {
        let outside = refused("sizesName", POOL_WORDS);
        // A name starts within the pool, which follows the last row, and a NUL there ends it.
        let pool = b"ab\0";
        assert_eq!(
            outcome(&sizes_rom(&[(5, 0, [0, -1, 0, 0])], pool), 5),
            one_file("ab", 0, 5)
        );
        assert_eq!(
            outcome(&sizes_rom(&[(5, 0, [1, -1, 0, 0])], pool), 5),
            one_file("b", 0, 5)
        );
        // At the NUL itself the name is empty, at the pool's end or past it there is none, and
        // without a NUL after it none ends.
        let empty = sizes_rom(&[(5, 0, [2, -1, 0, 0])], pool);
        assert_eq!(outcome(&empty, 5), unsafe_name("", "empty"));
        for at in [3, 4, i16::MAX] {
            let past = sizes_rom(&[(5, 0, [at, -1, 0, 0])], pool);
            assert_eq!(outcome(&past, 5), outside, "{at}");
        }
        let unended = sizes_rom(&[(5, 0, [0, -1, 0, 0])], b"ab");
        assert_eq!(outcome(&unended, 5), outside);
        let none = sizes_rom(&[(5, 0, [0, -1, 0, 0])], b"");
        assert_eq!(outcome(&none, 5), outside);
        // The places count from the pool's start, past every row: the second row's bytes are no
        // part of the first's names.
        let rows = [(5, 0, [0, -1, 0, 0]), (0, 5, [2, -1, 0, 0])];
        let table = sizes_rom(&rows, b"a\0b\0");
        assert_eq!(outcome(&table, 5), Ok(json!([["a", 0, 5], ["b", 5, 0]])));
        // Bytes past the last NUL are no part of any name.
        let trailing = sizes_rom(&[(5, 0, [0, -1, 0, 0])], b"a\0\xff\xfe");
        assert_eq!(outcome(&trailing, 5), one_file("a", 0, 5));
    }

    #[test]
    fn the_count_must_fit_the_file() {
        // Fewer bytes than the count takes.
        for len in 0..4 {
            assert_eq!(
                outcome(&vec![1; len], 5),
                refused("sizesHeader", HEADER_WORDS),
                "{len}"
            );
        }
        let table = |count: u32, bytes: usize| {
            let mut table = count.to_le_bytes().to_vec();
            table.resize(4 + bytes, 0);
            table
        };
        let outside = refused("sizesTable", TABLE_WORDS);
        // No row, or more rows than the file holds after the count, the largest count included.
        for (count, bytes) in [(0, 0), (0, 16), (1, 15), (2, 31), (u32::MAX, 64)] {
            assert_eq!(outcome(&table(count, bytes), 5), outside, "{count} {bytes}");
        }
        // Rows that fill the file leave an empty pool, where a row's first component finds no name.
        for count in [1, 2] {
            let given = outcome(&table(count, count as usize * FORMAT_ROW_LEN), 5);
            assert_eq!(given, refused("sizesName", POOL_WORDS), "{count}");
        }
    }

    #[test]
    fn extents_lie_within_disk_rom_and_externals_anywhere() {
        let file = |size: u32, offset: i32, disk_len: u64| {
            outcome(&path_table(&["levels", "a.wad"], size, offset), disk_len)
        };
        let path = "levels/a.wad";
        // A file DISK.ROM holds lies within it, up to its last byte; one of no bytes may lie at its
        // end, of a DISK.ROM of no bytes too.
        let within = [
            (5, 0, 5),
            (0, 5, 5),
            (1, 4, 5),
            (0, 0, 0),
            (u32::MAX, 0, u64::MAX),
        ];
        for (size, offset, disk_len) in within {
            let expected = one_file(path, i64::from(offset), size);
            assert_eq!(
                file(size, offset, disk_len),
                expected,
                "{size} {offset} {disk_len}"
            );
        }
        let past = [
            (6, 0, 5),
            (1, 5, 5),
            (0, 6, 5),
            (2, 4, 5),
            (u32::MAX, i32::MAX, 0),
        ];
        for (size, offset, disk_len) in past {
            assert_eq!(
                file(size, offset, disk_len),
                outside_disk(path),
                "{size} {offset} {disk_len}"
            );
        }
        // The largest place and size, past the most a smaller DISK.ROM holds.
        let largest = u64::from(u32::MAX) + u64::from(i32::MAX.unsigned_abs());
        assert!(file(u32::MAX, i32::MAX, largest).is_ok());
        assert_eq!(file(u32::MAX, i32::MAX, largest - 1), outside_disk(path));
        // A file the disc keeps outside DISK.ROM is listed by its declared size, whatever it is.
        for (size, disk_len) in [(0, 0), (u32::MAX, 0), (7, 5)] {
            assert_eq!(
                file(size, OUTSIDE, disk_len),
                one_file(path, -1, size),
                "{size}"
            );
        }
        // No other negative place is one.
        for offset in [-2, i32::MIN] {
            assert_eq!(file(5, offset, 5), outside_disk(path), "{offset}");
        }
        let place = packed_files(&path_table(&["x"], 5, OUTSIDE), 0).unwrap()[0].place;
        assert_eq!(place, PackedPlace::External);
    }

    #[test]
    fn every_byte_flip_is_refused_or_harmless() {
        // The full set's table, every byte of it changed in each bit in turn: refused, or read as
        // files whose paths the tree may hold and whose bytes lie within DISK.ROM. It never panics.
        let table = sizes_rom(
            &[
                (5, 0, [0, 7, -1, 0]),
                (0, 5, [0, 13, -1, 0]),
                (7, OUTSIDE, [19, -1, 0, 0]),
            ],
            b"levels\0a.wad\0b.rom\0ext.str\0",
        );
        let disk_len = 5;
        assert!(packed_files(&table, disk_len).is_ok());
        for at in 0..table.len() {
            for bit in 0..u8::BITS {
                let mut flipped = table.clone();
                flipped[at] ^= 1 << bit;
                let Ok(files) = packed_files(&flipped, disk_len) else {
                    continue;
                };
                for file in files {
                    let path = file.path.as_str();
                    assert_eq!(RelPath::parse(path, &DISC_NAMES).as_ref(), Ok(&file.path));
                    if let PackedPlace::Disk { offset } = file.place {
                        assert!(
                            offset + u64::from(file.size) <= disk_len,
                            "{at} {bit} {path}"
                        );
                    }
                }
            }
        }
    }
}
