//! The directory of a native WAD, as the N64 extractor describes it in its manifest: a header of
//! two words, where the directory starts and how many rows it holds, then from there a row for
//! each section, its four-byte tag, where its bytes start and a last word that is their size or a
//! count of the typed rows they hold. The words are big-endian or little-endian, whichever alone
//! describes a directory that ends where the WAD does, since one WAD of the US ROM keeps its words
//! in the other order from the rest. Only the directory is read: a WAD's bytes are kept as they
//! are, never put into another order.

use std::ops::RangeInclusive;

use gdl_files::RelPath;

use super::WORD_LEN;
use crate::{MediaError, RomRule};

/// The least place a WAD's directory may start: right past the header's two words. A section's
/// bytes may start there too, and no later than the directory.
pub const MIN_WAD_DIRECTORY: u32 = 8;

/// The length of a row of a WAD's directory: a section's tag, where its bytes start, and their
/// size or count.
pub const WAD_ROW_LEN: u64 = 12;

/// The most rows a WAD's directory may hold.
pub const MAX_WAD_SECTIONS: u32 = 65_536;

/// How many words the header holds: where the directory starts, and how many rows it holds.
const HEADER_WORDS: usize = 2;

/// How many words a row of the directory holds.
const ROW_WORDS: usize = WAD_ROW_LEN as usize / WORD_LEN;

/// The orders a WAD may keep its words in.
const ORDERS: [WadByteOrder; 2] = [WadByteOrder::BigEndian, WadByteOrder::LittleEndian];

/// The bytes a tag holds as the printable ASCII characters they are, from the space to the tilde.
const TAG_CHARS: RangeInclusive<u8> = b' '..=b'~';

/// The one other byte a tag may hold, anywhere in it, as the native tag `\0TOC` begins with.
const TAG_NUL: u8 = 0;

/// What the N64 extractor answers for a WAD too short to hold its header.
const TRUNCATED_HEADER: &str = "truncated WAD header";

/// What it answers for a header that describes a directory ending where the WAD does in neither
/// byte order, or in both, which leaves the order unknown.
const NOT_ONE_DIRECTORY: &str = "invalid or ambiguous native WAD directory";

/// What it answers for a row whose tag holds a byte other than printable ASCII or NUL, or whose
/// section starts before the header's end or past the directory's start.
const INVALID_SECTION: &str = "invalid native WAD section";

/// The byte order a WAD keeps its words in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WadByteOrder {
    /// Big-endian, the ROM's own order, in which nearly every WAD keeps them.
    BigEndian,
    /// Little-endian, in which one WAD of the US ROM keeps them.
    LittleEndian,
}

impl WadByteOrder {
    /// The order's name, as the N64 extractor's manifest writes it.
    pub fn as_str(self) -> &'static str {
        match self {
            WadByteOrder::BigEndian => "big-endian",
            WadByteOrder::LittleEndian => "little-endian",
        }
    }

    /// The word `bytes` hold, read in this order.
    fn word(self, bytes: [u8; WORD_LEN]) -> u32 {
        match self {
            WadByteOrder::BigEndian => u32::from_be_bytes(bytes),
            WadByteOrder::LittleEndian => u32::from_le_bytes(bytes),
        }
    }
}

/// A section of a native WAD, as its row of the directory describes it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WadSection {
    /// The section's tag: printable ASCII and NULs.
    pub tag: [u8; 4],
    /// Where the section's bytes start in the WAD: past the header, and no later than the
    /// directory.
    pub offset: u32,
    /// The row's last word: the section's size in bytes, or the count of the typed rows it holds,
    /// which the row does not tell apart. It is kept as it is found.
    pub size_or_count: u32,
    /// The order the WAD keeps its words in, the same for every section.
    pub byte_order: WadByteOrder,
}

impl WadSection {
    /// The tag as text, each byte the character of its code, NULs kept, as the N64 extractor's
    /// manifest writes it.
    pub fn tag_text(&self) -> String {
        self.tag.iter().copied().map(char::from).collect()
    }
}

/// The sections the native WAD `data` describes, in the order of its directory, read as the N64
/// extractor reads them, or the first rule of the format the WAD breaks, in the extractor's words
/// followed by `path`, the WAD's place in the tree, which they leave out. The header's two words
/// must describe, in exactly one byte order, a directory that starts on a word past the header
/// and holds at most [`MAX_WAD_SECTIONS`] rows, which end where the WAD does. Each row's tag must
/// be printable ASCII and NULs, and its section must start past the header and no later than the
/// directory. As in the extractor, a row's last word, and sections that overlap, are not checked.
pub fn wad_sections(path: &RelPath, data: &[u8]) -> Result<Vec<WadSection>, MediaError> {
    let (words, _) = data.as_chunks::<WORD_LEN>();
    let Some(&header) = words.first_chunk::<HEADER_WORDS>() else {
        return Err(refused(RomRule::WadHeader, TRUNCATED_HEADER, path));
    };
    // A header that both orders read as a directory, alike or each its own way, leaves unknown
    // which order the WAD keeps.
    let mut directories = ORDERS
        .into_iter()
        .filter_map(|order| directory_start(order, header, data.len()).map(|start| (order, start)));
    let (Some((order, start)), None) = (directories.next(), directories.next()) else {
        return Err(refused(RomRule::WadDirectory, NOT_ONE_DIRECTORY, path));
    };
    // The directory starts on a word and ends where the WAD does: it is a whole number of rows.
    let (rows, _) = words[start as usize / WORD_LEN..].as_chunks::<ROW_WORDS>();
    rows.iter()
        .map(|&[tag, offset, size_or_count]| {
            let offset = order.word(offset);
            let tagged = tag
                .iter()
                .all(|&byte| byte == TAG_NUL || TAG_CHARS.contains(&byte));
            let placed = (MIN_WAD_DIRECTORY..=start).contains(&offset);
            if tagged && placed {
                Ok(WadSection {
                    tag,
                    offset,
                    size_or_count: order.word(size_or_count),
                    byte_order: order,
                })
            } else {
                Err(refused(RomRule::WadSection, INVALID_SECTION, path))
            }
        })
        .collect()
}

/// Where the directory starts that `header`, the first words of a WAD of `len` bytes, describes
/// when its words are read in `order`, if they describe one: starting on a word no sooner than
/// [`MIN_WAD_DIRECTORY`], and holding at most [`MAX_WAD_SECTIONS`] rows, which end where the WAD
/// ends.
fn directory_start(
    order: WadByteOrder,
    [start, rows]: [[u8; WORD_LEN]; HEADER_WORDS],
    len: usize,
) -> Option<u32> {
    let (start, rows) = (order.word(start), order.word(rows));
    let described = start >= MIN_WAD_DIRECTORY
        && start.is_multiple_of(WORD_LEN as u32)
        && rows <= MAX_WAD_SECTIONS
        && u64::from(start) + WAD_ROW_LEN * u64::from(rows) == len as u64;
    described.then_some(start)
}

/// The refusal of the WAD at `path` by `rule`, in the extractor's `words` for it, followed by the
/// path, which they leave out.
fn refused(rule: RomRule, words: &str, path: &RelPath) -> MediaError {
    MediaError::rom(rule, format!("{words}: {}", path.as_str()))
}

#[cfg(test)]
mod tests {
    use gdl_files::{ROMDISK_NAMES, RelPath};
    use gdl_fixtures::{WadOrder, unhex, wad};
    use serde_json::{Value, json};

    use super::{
        MAX_WAD_SECTIONS, MIN_WAD_DIRECTORY, ORDERS, WAD_ROW_LEN, WadByteOrder, WadSection,
        directory_start, wad_sections,
    };
    use crate::vectors::{frozen, n64_golden, n64_vectors, named, text};
    use crate::{MediaError, N64Rom, ROMDISK_LIMITS};

    /// Where the default image keeps its WAD: a place in the tree, which a refusal names.
    const MAP: &str = "levels/test/worlds.wad";

    /// Where the mixed image keeps its little-endian WAD, in the letter case its ROMDISK gives.
    const CAPITALS: &str = "a/x.WAD";

    /// What the extractor answers for a WAD too short to hold its header.
    const TRUNCATED: &str = "truncated WAD header";

    /// What it answers for a header that describes no one directory.
    const NOT_ONE: &str = "invalid or ambiguous native WAD directory";

    /// What it answers for a row that describes no section it allows.
    const INVALID: &str = "invalid native WAD section";

    /// The orders the builders write a WAD's words in.
    const BUILT_ORDERS: [WadOrder; 2] = [WadOrder::Big, WadOrder::Little];

    /// What the reader gives a WAD, as the vectors write it: its sections, or the rule and the
    /// words it is refused with.
    type Outcome = Result<Vec<Value>, (&'static str, String)>;

    /// The place in the tree `path` names.
    fn at(path: &str) -> RelPath {
        RelPath::parse(path, &ROMDISK_NAMES).unwrap()
    }

    /// `section` as the extractor's manifest and the shared vectors write it.
    fn written(section: &WadSection) -> Value {
        json!({
            "tag": section.tag_text(),
            "offset": section.offset,
            "size_or_count": section.size_or_count,
            "byte_order": section.byte_order.as_str(),
        })
    }

    /// A section of a WAD whose words are in `order`, as the vectors write it.
    fn section(tag: &str, offset: u32, size_or_count: u32, order: WadOrder) -> Value {
        let order = match order {
            WadOrder::Big => "big-endian",
            WadOrder::Little => "little-endian",
        };
        json!({
            "tag": tag,
            "offset": offset,
            "size_or_count": size_or_count,
            "byte_order": order,
        })
    }

    /// What the reader gives the WAD `data`, which lies at `path`: by a rule of the format when it
    /// refuses it, never by a failure to read.
    fn outcome(path: &str, data: &[u8]) -> Outcome {
        match wad_sections(&at(path), data) {
            Ok(sections) => Ok(sections.iter().map(written).collect()),
            Err(error) => {
                assert!(matches!(error, MediaError::Rom { .. }), "{error:?}");
                Err((error.rule(), error.to_string()))
            }
        }
    }

    /// The refusal of the WAD at `path` by `rule`: the extractor's `words`, then the path, which
    /// they leave out.
    fn refusal(rule: &'static str, words: &str, path: &str) -> Outcome {
        Err((rule, format!("{words}: {path}")))
    }

    /// `value` as the word a WAD whose words are in `order` keeps.
    fn word_in(order: WadByteOrder, value: u32) -> [u8; 4] {
        match order {
            WadByteOrder::BigEndian => value.to_be_bytes(),
            WadByteOrder::LittleEndian => value.to_le_bytes(),
        }
    }

    /// `values` as words in `order`, one after another.
    fn words(order: WadOrder, values: &[u32]) -> Vec<u8> {
        values
            .iter()
            .flat_map(|&value| match order {
                WadOrder::Big => value.to_be_bytes(),
                WadOrder::Little => value.to_le_bytes(),
            })
            .collect()
    }

    /// A WAD whose words are in `order`: a header that puts its directory at `start`, no sooner
    /// than the header's end, and gives it a row for each of `rows`, then zeros up to `start`,
    /// then the rows, each a tag, where its section starts and the row's last word.
    fn built(order: WadOrder, start: u32, rows: &[([u8; 4], u32, u32)]) -> Vec<u8> {
        let count = u32::try_from(rows.len()).unwrap();
        let mut bytes = words(order, &[start, count]);
        bytes.resize(start as usize, 0);
        for &(tag, offset, size_or_count) in rows {
            bytes.extend_from_slice(&tag);
            bytes.extend(words(order, &[offset, size_or_count]));
        }
        bytes
    }

    #[test]
    fn wads_follow_the_shared_vectors() {
        let vectors = n64_vectors();
        let cases = vectors["wads"].as_array().unwrap();
        assert!(!cases.is_empty());
        let (mut rules, mut orders) = (Vec::new(), Vec::new());
        for case in cases {
            // The launcher decides every WAD as the extractor does.
            assert!(case.get("deviation").is_none(), "{case}");
            let data = unhex(&case["hex"]);
            // Wherever the WAD lies, it gives the same, and a refusal names where, in the letter
            // case the ROMDISK gives.
            for path in [MAP, CAPITALS] {
                match (outcome(path, &data), case["rule"].as_str()) {
                    (Ok(sections), None) => {
                        assert!(case["python"].is_null(), "{case}");
                        orders.extend(sections.iter().map(|section| section["byte_order"].clone()));
                        assert_eq!(Value::from(sections), case["sections"], "{case}");
                    }
                    (Err((rule, detail)), Some(expected)) => {
                        assert_eq!(rule, expected, "{case}");
                        // The extractor's own words, which are fixed, then the WAD's place.
                        let said = format!("{}: {path}", text(&case["python"]));
                        assert_eq!(detail, said, "{case}");
                        rules.push(rule);
                    }
                    (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
                }
            }
        }
        rules.sort_unstable();
        rules.dedup();
        assert_eq!(rules, ["wadDirectory", "wadHeader", "wadSection"]);
        // A WAD of either order is among them.
        for order in ["big-endian", "little-endian"] {
            assert!(
                orders.contains(&Value::from(order)),
                "{order} among {orders:?}"
            );
        }
    }

    #[test]
    fn two_valid_orders_are_ambiguous() {
        // A directory at 65,792 that holds no row, the WAD's end: the header's words read alike
        // in either order, so each order describes the directory, and neither can be chosen.
        let header = [[0x00, 0x01, 0x01, 0x00], [0; 4]];
        let len = 65_792;
        let mut data = header.concat();
        data.resize(len, 0);
        for order in ORDERS {
            assert_eq!(
                directory_start(order, header, len),
                Some(65_792),
                "{order:?}"
            );
        }
        assert_eq!(outcome(MAP, &data), refusal("wadDirectory", NOT_ONE, MAP));

        // With a row, the count reads as 1 one way and 16,777,216 the other, more than a
        // directory may hold: the order is known, and the WAD is read in it.
        data[4..8].copy_from_slice(&words(WadOrder::Big, &[1]));
        data.extend_from_slice(b"TEST");
        data.extend(words(WadOrder::Big, &[8, 4]));
        let read = Ok(vec![section("TEST", 8, 4, WadOrder::Big)]);
        assert_eq!(outcome(MAP, &data), read);
    }

    #[test]
    fn the_directory_starts_on_a_word_past_the_header() {
        for order in BUILT_ORDERS {
            // One row, after zeros from the header to wherever the directory starts.
            for start in MIN_WAD_DIRECTORY..=16 {
                let data = built(order, start, &[(*b"TEST", 8, 4)]);
                let expected = if start.is_multiple_of(4) {
                    Ok(vec![section("TEST", 8, 4, order)])
                } else {
                    refusal("wadDirectory", NOT_ONE, MAP)
                };
                assert_eq!(outcome(MAP, &data), expected, "{order:?} {start}");
            }
            // A directory that would start within the header, though the WAD holds its row.
            for start in [0, 4] {
                let mut data = words(order, &[start, 1]);
                data.resize(start as usize + WAD_ROW_LEN as usize, 0);
                let expected = refusal("wadDirectory", NOT_ONE, MAP);
                assert_eq!(outcome(MAP, &data), expected, "{order:?} {start}");
            }
            // A directory of no row may start where the WAD ends.
            for start in [8, 12] {
                assert_eq!(outcome(MAP, &built(order, start, &[])), Ok(vec![]));
            }
        }
    }

    #[test]
    fn a_section_starts_past_the_header_and_no_later_than_the_directory() {
        // The directory starts at 12, past four bytes of the sections'.
        let offsets = [
            (0, false),
            (7, false),
            (8, true),
            (11, true),
            (12, true),
            (13, false),
            (u32::MAX, false),
        ];
        for order in BUILT_ORDERS {
            for (offset, taken) in offsets {
                let data = built(order, 12, &[(*b"TEST", offset, 4)]);
                let expected = if taken {
                    Ok(vec![section("TEST", offset, 4, order)])
                } else {
                    refusal("wadSection", INVALID, MAP)
                };
                assert_eq!(outcome(MAP, &data), expected, "{order:?} {offset}");
            }
        }
    }

    #[test]
    fn a_directory_holds_at_most_65536_rows() {
        // The bound is the header's alone, so it is judged on a WAD's length, with no bytes made
        // for its rows. A header that gives a directory of the most rows, ending where the WAD
        // does, describes it in its own order and not in the other; one a row longer describes it
        // in neither, so that the reader refuses it as no one directory.
        assert_eq!(MAX_WAD_SECTIONS, 65_536);
        let len = |rows: u32| {
            let len = u64::from(MIN_WAD_DIRECTORY) + WAD_ROW_LEN * u64::from(rows);
            usize::try_from(len).unwrap()
        };
        let cases = [
            (MAX_WAD_SECTIONS, Some(MIN_WAD_DIRECTORY)),
            (MAX_WAD_SECTIONS + 1, None),
        ];
        for order in ORDERS {
            for (rows, start) in cases {
                let header = [word_in(order, MIN_WAD_DIRECTORY), word_in(order, rows)];
                for reading in ORDERS {
                    let expected = start.filter(|_| reading == order);
                    let read = directory_start(reading, header, len(rows));
                    assert_eq!(read, expected, "{order:?} {rows} read {reading:?}");
                }
            }
        }
    }

    #[test]
    fn every_tag_byte_but_printable_ascii_and_nul_is_refused() {
        for order in BUILT_ORDERS {
            for position in 0..4 {
                for byte in 0..=u8::MAX {
                    let mut tag = *b"TEST";
                    tag[position] = byte;
                    let data = wad(order, tag);
                    let read = wad_sections(&at(MAP), &data);
                    if byte == 0 || (b' '..=b'~').contains(&byte) {
                        let sections = read.unwrap();
                        assert_eq!(sections.len(), 1);
                        assert_eq!(sections[0].tag, tag);
                        // Written a character a byte, a NUL as a NUL.
                        assert_eq!(sections[0].tag_text().as_bytes(), tag, "{byte:#x}");
                    } else {
                        let error = read.unwrap_err();
                        assert_eq!(error.rule(), "wadSection", "{position} {byte:#x}");
                        assert_eq!(error.to_string(), format!("{INVALID}: {MAP}"));
                    }
                }
            }
        }
    }

    #[test]
    fn a_wad_cut_short_or_made_longer_is_refused() {
        for order in BUILT_ORDERS {
            let whole = wad(order, *b"TEST");
            assert_eq!(whole.len(), 24);
            for len in 0..whole.len() {
                let expected = if len < MIN_WAD_DIRECTORY as usize {
                    refusal("wadHeader", TRUNCATED, MAP)
                } else {
                    refusal("wadDirectory", NOT_ONE, MAP)
                };
                assert_eq!(outcome(MAP, &whole[..len]), expected, "{order:?} {len}");
            }
            for more in 1..=16 {
                let mut longer = whole.clone();
                longer.resize(whole.len() + more, 0);
                let expected = refusal("wadDirectory", NOT_ONE, MAP);
                assert_eq!(outcome(MAP, &longer), expected, "{order:?} {more}");
            }
        }
    }

    #[test]
    fn sections_keep_their_order_and_last_words_in_either_byte_order() {
        // Three rows from 44, the first two sections' bytes before them.
        let rows = [(*b"ABCD", 8, 0), ([0; 4], 12, u32::MAX), (*b" ~ ~", 44, 7)];
        for order in BUILT_ORDERS {
            let data = built(order, 44, &rows);
            assert_eq!(data.len(), 80);
            let expected = vec![
                section("ABCD", 8, 0, order),
                section("\0\0\0\0", 12, u32::MAX, order),
                section(" ~ ~", 44, 7, order),
            ];
            assert_eq!(outcome(MAP, &data), Ok(expected), "{order:?}");

            // A row that describes no section refuses the whole WAD, wherever it stands: nothing
            // is given of the rows before it.
            for bad in 0..rows.len() {
                let mut past = rows;
                past[bad].1 = 45;
                let mut untagged = rows;
                untagged[bad].0[1] = 0x7F;
                for damaged in [past, untagged] {
                    let refused = refusal("wadSection", INVALID, MAP);
                    let data = built(order, 44, &damaged);
                    assert_eq!(outcome(MAP, &data), refused, "{order:?} {bad}");
                }
            }
        }
    }

    #[test]
    fn the_frozen_images_wads_are_what_the_extractors_manifests_record() {
        let vectors = n64_vectors();
        let extractions = vectors["extractions"].as_array().unwrap();
        // Both frozen images, each as the extractor read it.
        let images: Vec<&str> = extractions
            .iter()
            .map(|extraction| text(&extraction["image"]))
            .collect();
        assert_eq!(images, ["default", "mixed"]);
        let mut read = Vec::new();
        for extraction in extractions {
            let image = frozen(&vectors, text(&extraction["image"]));
            let cartridge = N64Rom::from_bytes(named(text(&extraction["name"])), image).unwrap();
            let romdisk = cartridge.romdisk(&ROMDISK_LIMITS).unwrap();
            let golden = n64_golden(&extraction["golden"]);
            let rows = golden["files"].as_array().unwrap();
            assert_eq!(rows.len(), romdisk.entries.len());
            for (row, entry) in rows.iter().zip(&romdisk.entries) {
                let path = entry.path.as_str();
                // A file whose name ends in `.wad`, in any letter case, is a WAD, as the
                // extractor takes it.
                if !path.to_ascii_lowercase().ends_with(".wad") {
                    assert!(row.get("wad_sections").is_none(), "{path}");
                    continue;
                }
                let data = romdisk.decode(entry).unwrap();
                let sections = wad_sections(&entry.path, &data).unwrap();
                let described: Vec<Value> = sections.iter().map(written).collect();
                assert_eq!(row["wad_sections"], Value::from(described), "{path}");
                read.extend(
                    sections
                        .iter()
                        .map(|section| (path.to_owned(), section.byte_order)),
                );
            }
        }
        // Among them the mixed image's little-endian WAD, named in capitals, and a big-endian one.
        let little = (CAPITALS.to_owned(), WadByteOrder::LittleEndian);
        assert!(read.contains(&little), "{read:?}");
        let big = (MAP.to_owned(), WadByteOrder::BigEndian);
        assert!(read.contains(&big), "{read:?}");
    }
}
