//! N64 ROMs for the tests of the launcher's N64 reader and extraction, laid out as
//! tests/scripts/legends_fixtures.py lays out the N64 extractor's: a header whose first word is
//! the big-endian order's, then a ROMDISK of files kept plain or raw-DEFLATEd, and, for the tests
//! of the executable overlays, a table of four overlays where the US ROM keeps its own. A ROM of
//! plain files has the bytes that builder makes. A compressed file is deflated here, into a stream
//! that may differ from the one Python's deflater gives, in its length too, which moves every part
//! after it; a test that must match the extractor byte for byte, or the places of its parts, reads
//! the images the shared vectors freeze instead.

use std::io::Write;

use flate2::Compression;
use flate2::write::DeflateEncoder;

/// The last word of each record the builder writes, which the format leaves unexplained and the
/// extractor keeps as it finds it.
pub const BUILDER_UNKNOWN_WORD: u32 = 0x1234_5678;

/// The length of an N64 ROM's header, which the ROMDISK follows here.
pub const ROM_HEADER_LEN: usize = 64;

/// The first word of a ROM in the big-endian byte order, the one the extractor reads as it is.
pub const BIG_ENDIAN_FIRST_WORD: [u8; 4] = [0x80, 0x37, 0x12, 0x40];

/// Where the US ROM keeps its table of executable overlays, and where [`rom_with_overlays`]
/// writes its own.
pub const OVERLAY_TABLE_OFFSET: usize = 0x12E0;

/// The magic a ROMDISK begins with, its NUL included.
const ROMDISK_MAGIC: &[u8; 33] = b"-==ROMDISK Header - HERE I AM==-\0";

/// The zero bytes the builder puts after the magic. The header's four words are written over the
/// magic's NUL and the first 15 of them, so the first file's bytes start one byte past those words.
const HEADER_ZEROS: usize = 16;

/// Where the ROMDISK header keeps its four words, counted from the magic: the length of the
/// directory, where its pointer table starts, how many files it holds, and a reserved zero.
const HEADER_WORDS_AT: usize = 32;

/// The record flag of a file the ROMDISK keeps raw-DEFLATEd.
const COMPRESSED: u32 = 0x2000_0000;

/// The length of a word of the ROM, which the ROMDISK's offsets, records and names are aligned to.
const WORD_LEN: usize = 4;

/// The level the extractor tests' builder deflates at, Python's default.
const DEFLATE_LEVEL: u32 = 6;

/// The length of a WAD's header: where its directory starts, then how many rows it holds.
const WAD_HEADER_LEN: usize = 8;

/// What the builder's WAD holds between its header and its directory: the one section's bytes.
const WAD_DATA: &[u8; 4] = b"data";

/// The length of a record of the overlay table: the overlay's name, then eight words.
const OVERLAY_RECORD_LEN: usize = 48;

/// The length of an overlay's name in its record, padded with NULs.
const OVERLAY_NAME_LEN: usize = 16;

/// The records of the overlay table that the US ROM's four overlays fill, in the order
/// [`rom_with_overlays`] takes them.
const OVERLAY_INDICES: [usize; 4] = [0, 1, 2, 4];

/// The record between theirs that no overlay fills.
const UNUSED_OVERLAY_INDEX: usize = 3;

/// What fills the unused record, so that a reader that took it would refuse its name and its
/// reserved words.
const UNUSED_RECORD_BYTE: u8 = 0xFF;

/// Where the overlays' streams start: past the overlay table's last record.
const OVERLAYS_AT: usize = 0x2000;

/// What follows an overlay's stream inside its extent: bytes that are not zeros, as the US ROM's
/// overlays, whose extents are aligned for DMA, keep bytes after their streams.
const OVERLAY_FILLER: u8 = 0xA5;

/// A file of a ROM's ROMDISK. Every field may be changed to make the file a test needs, an unsafe
/// one included.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RomFile {
    /// Its native path, which begins with the one `/` the format gives every path, such as
    /// `/levels/test/worlds.wad`. Its UTF-8 bytes are written as they are.
    pub path: String,
    /// What it holds, before any compression.
    pub payload: Vec<u8>,
    /// Whether the ROMDISK keeps it raw-DEFLATEd, flagged compressed in its record.
    pub compressed: bool,
}

impl RomFile {
    /// A file the ROMDISK keeps as it is.
    pub fn plain(path: &str, payload: &[u8]) -> Self {
        RomFile {
            path: path.to_owned(),
            payload: payload.to_vec(),
            compressed: false,
        }
    }

    /// A file the ROMDISK keeps raw-DEFLATEd.
    pub fn compressed(path: &str, payload: &[u8]) -> Self {
        RomFile {
            compressed: true,
            ..RomFile::plain(path, payload)
        }
    }
}

/// A ROM a builder made, and where its ROMDISK's parts lie in it, so that a test can change a word
/// of them with [`set_word`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BuiltRom {
    /// The ROM, in the big-endian byte order.
    pub bytes: Vec<u8>,
    /// Where the ROMDISK's magic starts, the base its offsets count from.
    pub base: usize,
    /// Where the ROMDISK's pointer table starts.
    pub table: usize,
    /// Where each file's record starts, in the order of the files.
    pub records: Vec<usize>,
}

/// The files of the extractor tests' default ROM: [`wad`]'s big-endian WAD tagged `TEST`, kept
/// raw-DEFLATEd at `/levels/test/worlds.wad`, and `native`, kept plain at `/texture.bin`.
pub fn default_rom_files() -> Vec<RomFile> {
    vec![
        RomFile::compressed("/levels/test/worlds.wad", &wad(WadOrder::Big, *b"TEST")),
        RomFile::plain("/texture.bin", b"native"),
    ]
}

/// A ROM of `files`, laid out as the extractor tests' builder lays one out: the 64-byte header,
/// zeros but for its first word, so that its game code is four NULs; the ROMDISK's magic and 16
/// zero bytes; each file's stored bytes, padded with zeros to a word; the pointer table; then each
/// file's record, five big-endian words (its flags, stored size, size, offset from the base and
/// [`BUILDER_UNKNOWN_WORD`]) followed by its path and a NUL, padded to a word. The header's four
/// words are written last. Panics when a length or an offset does not fit a word.
pub fn rom(files: &[RomFile]) -> BuiltRom {
    let mut bytes = vec![0; ROM_HEADER_LEN];
    bytes[..BIG_ENDIAN_FIRST_WORD.len()].copy_from_slice(&BIG_ENDIAN_FIRST_WORD);
    let base = bytes.len();
    bytes.extend_from_slice(ROMDISK_MAGIC);
    bytes.resize(bytes.len() + HEADER_ZEROS, 0);
    // Each file's record words before the last: its flags, stored size, size and offset.
    let mut records_words = Vec::with_capacity(files.len());
    for file in files {
        let (flags, stored) = if file.compressed {
            (COMPRESSED, raw_deflate(&file.payload))
        } else {
            (0, file.payload.clone())
        };
        records_words.push([
            flags,
            word(stored.len(), "a file's stored size"),
            word(file.payload.len(), "a file's size"),
            word(bytes.len() - base, "a file's offset"),
        ]);
        bytes.extend_from_slice(&stored);
        pad_to_word(&mut bytes);
    }
    let table = bytes.len();
    bytes.resize(table + WORD_LEN * files.len(), 0);
    let mut records = Vec::with_capacity(files.len());
    for (pointer, (file, words)) in files.iter().zip(records_words).enumerate() {
        let record = bytes.len();
        set_word(
            &mut bytes,
            table + WORD_LEN * pointer,
            word(record - base, "a record's offset"),
        );
        for value in words.into_iter().chain([BUILDER_UNKNOWN_WORD]) {
            bytes.extend_from_slice(&value.to_be_bytes());
        }
        bytes.extend_from_slice(file.path.as_bytes());
        bytes.push(0);
        pad_to_word(&mut bytes);
        records.push(record);
    }
    let header = [
        word(bytes.len() - table, "the directory's length"),
        word(table - base, "the pointer table's offset"),
        word(files.len(), "the count of files"),
        0,
    ];
    for (index, value) in header.into_iter().enumerate() {
        set_word(&mut bytes, base + HEADER_WORDS_AT + WORD_LEN * index, value);
    }
    BuiltRom {
        bytes,
        base,
        table,
        records,
    }
}

/// `data` as a raw DEFLATE stream, with no header or checksum around it, deflated at the level of
/// the extractor tests' builder.
pub fn raw_deflate(data: &[u8]) -> Vec<u8> {
    let mut encoder = DeflateEncoder::new(Vec::new(), Compression::new(DEFLATE_LEVEL));
    encoder
        .write_all(data)
        .expect("deflating into memory cannot fail");
    encoder.finish().expect("deflating into memory cannot fail")
}

/// The byte order of a WAD's words.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WadOrder {
    /// Big-endian, the ROM's own order.
    Big,
    /// Little-endian, as one WAD of the US ROM keeps its words.
    Little,
}

/// The extractor tests' WAD, 24 bytes with its words in `order`: a header that puts the directory
/// at 12 and gives it one row, the 4 bytes `data`, then that row, a section tagged `tag` of 4 bytes
/// at 8.
pub fn wad(order: WadOrder, tag: [u8; 4]) -> Vec<u8> {
    let rows = [(tag, WAD_HEADER_LEN, WAD_DATA.len())];
    let put = |bytes: &mut Vec<u8>, value: usize| {
        let value = word(value, "a WAD's word");
        bytes.extend_from_slice(&match order {
            WadOrder::Big => value.to_be_bytes(),
            WadOrder::Little => value.to_le_bytes(),
        });
    };
    let mut bytes = Vec::new();
    put(&mut bytes, WAD_HEADER_LEN + WAD_DATA.len());
    put(&mut bytes, rows.len());
    bytes.extend_from_slice(WAD_DATA);
    for (tag, offset, len) in rows {
        bytes.extend_from_slice(&tag);
        put(&mut bytes, offset);
        put(&mut bytes, len);
    }
    bytes
}

/// Writes `value` as the big-endian word at `at`, as a test changes one word of a header, a table
/// or a record. Panics when the word runs past `bytes`.
pub fn set_word(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + WORD_LEN].copy_from_slice(&value.to_be_bytes());
}

/// An executable overlay of [`rom_with_overlays`]'s table.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OverlaySpec {
    /// The name its record holds, at most 16 bytes, padded with NULs.
    pub name: &'static str,
    /// What it holds, which the ROM keeps raw-DEFLATEd.
    pub data: Vec<u8>,
    /// The address it is loaded at.
    pub load_address: u32,
    /// The size of the zeroed memory that follows it once it is loaded.
    pub bss_size: u32,
    /// How many filler bytes follow its stream inside its extent, of which a reader takes at most
    /// 15.
    pub padding: usize,
}

/// A ROM of `files` with a table of executable overlays where the US ROM keeps its own: [`rom`]'s
/// ROM padded with zeros to 0x2000 bytes, then each overlay's raw DEFLATE stream and its filler,
/// each extent starting at a word. The records 0, 1, 2 and 4 of the table, 48 bytes each from
/// [`OVERLAY_TABLE_OFFSET`], describe `overlays` in order: the name, then eight big-endian words,
/// which are where the extent starts, its length, the load address, the memory the overlay takes
/// (its data and the zeroed memory after it), where that zeroed memory starts, its size, and two
/// reserved zeros. Record 3 is all 0xFF. The ROMDISK's magic occurs once. Panics when the ROMDISK
/// reaches the table, a name is longer than 16 bytes, or a value does not fit its word.
pub fn rom_with_overlays(files: &[RomFile], overlays: [OverlaySpec; 4]) -> BuiltRom {
    let mut built = rom(files);
    let bytes = &mut built.bytes;
    assert!(
        bytes.len() <= OVERLAY_TABLE_OFFSET,
        "a ROMDISK that ends at {} reaches the overlay table at {OVERLAY_TABLE_OFFSET:#x}",
        bytes.len()
    );
    bytes.resize(OVERLAYS_AT, 0);
    let unused = OVERLAY_TABLE_OFFSET + UNUSED_OVERLAY_INDEX * OVERLAY_RECORD_LEN;
    bytes[unused..unused + OVERLAY_RECORD_LEN].fill(UNUSED_RECORD_BYTE);
    for (index, overlay) in OVERLAY_INDICES.into_iter().zip(overlays) {
        let name = overlay.name.as_bytes();
        assert!(
            name.len() <= OVERLAY_NAME_LEN,
            "an overlay's name holds at most {OVERLAY_NAME_LEN} bytes, not {:?}",
            overlay.name
        );
        let offset = bytes.len();
        bytes.extend_from_slice(&raw_deflate(&overlay.data));
        bytes.resize(bytes.len() + overlay.padding, OVERLAY_FILLER);
        let stored = bytes.len() - offset;
        pad_to_word(bytes);
        let len = word(overlay.data.len(), "an overlay's size");
        let memory = len
            .checked_add(overlay.bss_size)
            .expect("an overlay's memory size fits a word");
        let bss = overlay
            .load_address
            .checked_add(len)
            .expect("an overlay's zeroed memory starts at an address that fits a word");
        let record = OVERLAY_TABLE_OFFSET + index * OVERLAY_RECORD_LEN;
        let mut field = [0; OVERLAY_NAME_LEN];
        field[..name.len()].copy_from_slice(name);
        bytes[record..record + OVERLAY_NAME_LEN].copy_from_slice(&field);
        let words = [
            word(offset, "an overlay's offset"),
            word(stored, "an overlay's extent"),
            overlay.load_address,
            memory,
            bss,
            overlay.bss_size,
            0,
            0,
        ];
        for (at, value) in words.into_iter().enumerate() {
            set_word(bytes, record + OVERLAY_NAME_LEN + WORD_LEN * at, value);
        }
    }
    let magics = bytes
        .windows(ROMDISK_MAGIC.len())
        .filter(|window| *window == ROMDISK_MAGIC)
        .count();
    assert_eq!(
        magics, 1,
        "the ROM holds the ROMDISK's magic {magics} times"
    );
    built
}

/// `value`, `what` the builder writes, as a word of 32 bits.
fn word(value: usize, what: &str) -> u32 {
    u32::try_from(value)
        .unwrap_or_else(|_| panic!("{what}, {value}, does not fit a word of 32 bits"))
}

/// Pads `bytes` with zeros to a whole number of words.
fn pad_to_word(bytes: &mut Vec<u8>) {
    bytes.resize(bytes.len().next_multiple_of(WORD_LEN), 0);
}

#[cfg(test)]
mod tests {
    use std::array;

    use flate2::{Decompress, FlushDecompress, Status};
    use serde_json::Value;

    use super::{
        BIG_ENDIAN_FIRST_WORD, BUILDER_UNKNOWN_WORD, OVERLAY_FILLER, OVERLAY_TABLE_OFFSET,
        OverlaySpec, ROM_HEADER_LEN, ROMDISK_MAGIC, RomFile, WadOrder, default_rom_files,
        raw_deflate, rom, rom_with_overlays, set_word, wad,
    };
    use crate::runtime::{hex, sha256_hex};

    /// The N64 cases the extractor's tests and the launcher's read alike.
    const VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/legends/n64-vectors.json"
    ));

    /// More than any stream of these tests inflates to.
    const INFLATED_MOST: usize = 1 << 16;

    fn vectors() -> Value {
        serde_json::from_str(VECTORS).expect("the N64 vectors are JSON")
    }

    /// The bytes a vector's hexadecimal text spells.
    fn unhex(text: &Value) -> Vec<u8> {
        let text = text.as_str().expect("hexadecimal text");
        assert_eq!(text.len() % 2, 0, "{text}");
        (0..text.len())
            .step_by(2)
            .map(|at| u8::from_str_radix(&text[at..at + 2], 16).expect("hexadecimal digits"))
            .collect()
    }

    /// The big-endian word at `at`.
    fn word_at(bytes: &[u8], at: usize) -> u32 {
        u32::from_be_bytes(bytes[at..at + 4].try_into().unwrap())
    }

    /// Where a ROM's ROMDISK keeps its magic, its pointer table and its records, read back from its
    /// header's words and its table: the table's offset from the magic is the second word, 36
    /// bytes in, the count of files the third, and each pointer an offset from the magic.
    fn layout(bytes: &[u8]) -> (usize, usize, Vec<usize>) {
        let base = bytes
            .windows(ROMDISK_MAGIC.len())
            .position(|window| window == ROMDISK_MAGIC)
            .expect("a ROMDISK's magic");
        let table = base + word_at(bytes, base + 36) as usize;
        let count = word_at(bytes, base + 40) as usize;
        let records = (0..count)
            .map(|pointer| base + word_at(bytes, table + 4 * pointer) as usize)
            .collect();
        (base, table, records)
    }

    /// What the raw DEFLATE stream that begins `extent` inflates to, and the bytes of `extent`
    /// after the stream's end.
    fn inflate(extent: &[u8]) -> (Vec<u8>, &[u8]) {
        let mut decoder = Decompress::new(false);
        let mut data = Vec::with_capacity(INFLATED_MOST);
        let status = decoder
            .decompress_vec(extent, &mut data, FlushDecompress::Finish)
            .expect("a raw DEFLATE stream");
        assert_eq!(
            status,
            Status::StreamEnd,
            "the stream ends inside its extent"
        );
        (data, &extent[decoder.total_in() as usize..])
    }

    /// Whether every one of `bytes` is `value`.
    fn all_are(bytes: &[u8], value: u8) -> bool {
        bytes.iter().all(|&byte| byte == value)
    }

    /// Bytes that are not all alike, so that their stream takes more than one code.
    fn noise(len: usize) -> Vec<u8> {
        (0..len).map(|index| (index * 151 % 251) as u8).collect()
    }

    /// Four overlays unlike one another: of noise, empty, of zeros and of text, one of them named
    /// with all 16 bytes, followed by none, one, all 15 and four filler bytes.
    fn overlays() -> [OverlaySpec; 4] {
        [
            OverlaySpec {
                name: "first",
                data: noise(3000),
                load_address: 0x8000_0400,
                bss_size: 0x40,
                padding: 0,
            },
            OverlaySpec {
                name: "second",
                data: Vec::new(),
                load_address: 0x8010_0000,
                bss_size: 0x1000,
                padding: 1,
            },
            OverlaySpec {
                name: "overlay-of-16-by",
                data: vec![0; 64],
                load_address: 0x8020_0000,
                bss_size: 0,
                padding: 15,
            },
            OverlaySpec {
                name: "fourth",
                data: b"overlay text ".repeat(40),
                load_address: 0x8030_0010,
                bss_size: 7,
                padding: 4,
            },
        ]
    }

    #[test]
    fn a_plain_rom_is_the_python_builders() {
        let vectors = vectors();
        let plain = &vectors["plainRom"];
        let files: Vec<RomFile> = plain["files"]
            .as_array()
            .unwrap()
            .iter()
            .map(|file| {
                // Plain files alone make bytes that both builders agree on.
                assert_eq!(file[2], false, "{file}");
                RomFile::plain(file[0].as_str().unwrap(), &unhex(&file[1]))
            })
            .collect();
        assert!(!files.is_empty());
        let built = rom(&files);
        assert_eq!(hex(&built.bytes), plain["hex"].as_str().unwrap());
        assert_eq!(
            layout(&built.bytes),
            (built.base, built.table, built.records.clone())
        );
    }

    #[test]
    fn records_and_table_are_where_the_builder_says() {
        let vectors = vectors();
        let frozen = unhex(&vectors["images"]["default"]);
        let (base, table, records) = layout(&frozen);
        assert_eq!((base, table, records.clone()), (64, 144, vec![152, 196]));
        let files = default_rom_files();

        // The frozen image holds the default files, the map deflated by the extractor tests'
        // builder. Given that stream in place of this crate's, stored and then flagged compressed
        // with the map's size, the builder makes the frozen image byte for byte: with a stream of
        // the same length, it lays a ROM out as the extractor tests' builder does.
        let stream_at = base + word_at(&frozen, records[0] + 12) as usize;
        let stream = &frozen[stream_at..stream_at + word_at(&frozen, records[0] + 4) as usize];
        assert_eq!(inflate(stream), (files[0].payload.clone(), &[][..]));
        let mut alike = files.clone();
        alike[0] = RomFile::plain(&files[0].path, stream);
        let mut alike = rom(&alike);
        set_word(&mut alike.bytes, alike.records[0], 0x2000_0000);
        let size = files[0].payload.len() as u32;
        set_word(&mut alike.bytes, alike.records[0] + 8, size);
        assert_eq!(
            (alike.base, alike.table, alike.records),
            (base, table, records)
        );
        assert_eq!(hex(&alike.bytes), hex(&frozen));

        // This crate's deflater gives the map a stream of a length of its own, which moves what
        // follows it. Each word, read back, is where the builder says and holds what the files
        // give: the header's first, every other byte of it zero.
        let built = rom(&files);
        assert_eq!(
            layout(&built.bytes),
            (built.base, built.table, built.records.clone())
        );
        let bytes = &built.bytes;
        assert_eq!(bytes.len() % 4, 0);
        assert_eq!(bytes[..4], BIG_ENDIAN_FIRST_WORD);
        assert!(all_are(&bytes[4..ROM_HEADER_LEN], 0));
        // The magic, its NUL the high byte of the directory's length, then the header's words: the
        // directory's length, the table's offset, the count of files and a reserved zero.
        assert_eq!(bytes[64..97], ROMDISK_MAGIC[..]);
        assert_eq!(word_at(bytes, 96), (bytes.len() - built.table) as u32);
        assert_eq!(word_at(bytes, 100), (built.table - built.base) as u32);
        assert_eq!(word_at(bytes, 104), files.len() as u32);
        assert_eq!(word_at(bytes, 108), 0);
        assert_eq!(bytes[112], 0);
        // Each file's pointer, record, name and stored bytes, the first a byte past the header.
        let mut data_at = 113;
        for (pointer, (file, &record)) in files.iter().zip(&built.records).enumerate() {
            assert_eq!(
                word_at(bytes, built.table + 4 * pointer),
                (record - built.base) as u32
            );
            let flags = if file.compressed { 0x2000_0000 } else { 0 };
            assert_eq!(word_at(bytes, record), flags, "{}", file.path);
            let stored = word_at(bytes, record + 4) as usize;
            assert_eq!(word_at(bytes, record + 8), file.payload.len() as u32);
            assert_eq!(
                built.base + word_at(bytes, record + 12) as usize,
                data_at,
                "{}",
                file.path
            );
            assert_eq!(word_at(bytes, record + 16), BUILDER_UNKNOWN_WORD);
            let name_end = record + 20 + file.path.len();
            assert_eq!(&bytes[record + 20..name_end], file.path.as_bytes());
            // The NUL and the zeros up to a word end the record, and the next one starts there.
            let next = (name_end + 1).next_multiple_of(4);
            assert!(all_are(&bytes[name_end..next], 0));
            let following = built.records.get(pointer + 1).copied();
            assert_eq!(following.unwrap_or(bytes.len()), next);
            let data = &bytes[data_at..data_at + stored];
            if file.compressed {
                assert_eq!(data, raw_deflate(&file.payload), "{}", file.path);
                assert_eq!(
                    inflate(data),
                    (file.payload.clone(), &[][..]),
                    "{}",
                    file.path
                );
            } else {
                assert_eq!(data, file.payload, "{}", file.path);
            }
            let padded = (data_at + stored).next_multiple_of(4);
            assert!(all_are(&bytes[data_at + stored..padded], 0));
            data_at = padded;
        }
        assert_eq!(data_at, built.table);
    }

    #[test]
    fn the_default_files_are_what_the_default_tree_holds() {
        let vectors = vectors();
        let tree = vectors["trees"]["default"].as_array().unwrap();
        let files = default_rom_files();
        for file in &files {
            let path = file.path.strip_prefix('/').unwrap();
            let row = tree
                .iter()
                .find(|row| row[0] == path)
                .unwrap_or_else(|| panic!("{path} is not in the default tree"));
            assert_eq!(row[1], file.payload.len(), "{path}");
            assert_eq!(row[2], sha256_hex(&file.payload), "{path}");
        }
        // The tree holds the files and the manifest.
        assert_eq!(tree.len(), files.len() + 1);
        assert_eq!(
            files.iter().map(|file| file.compressed).collect::<Vec<_>>(),
            [true, false]
        );
    }

    #[test]
    fn wads_are_the_python_builders() {
        let vectors = vectors();
        // The shared WADs of the builder's form, each 24 bytes with one section of 4 bytes at 8,
        // made again from their tag and byte order.
        let mut made = Vec::new();
        for case in vectors["wads"].as_array().unwrap() {
            let bytes = unhex(&case["hex"]);
            let Some([section]) = case["sections"].as_array().map(Vec::as_slice) else {
                continue;
            };
            if bytes.len() != 24 || section["offset"] != 8 || section["size_or_count"] != 4 {
                continue;
            }
            let order = match section["byte_order"].as_str().unwrap() {
                "big-endian" => WadOrder::Big,
                "little-endian" => WadOrder::Little,
                other => panic!("no WAD is in the order {other}"),
            };
            let tag: [u8; 4] = section["tag"]
                .as_str()
                .unwrap()
                .as_bytes()
                .try_into()
                .unwrap();
            assert_eq!(hex(&wad(order, tag)), hex(&bytes), "{order:?}");
            made.push(order);
        }
        assert_eq!(made, [WadOrder::Big, WadOrder::Little]);
    }

    #[test]
    fn set_word_writes_one_big_endian_word() {
        let mut bytes = vec![0xEE; 12];
        set_word(&mut bytes, 4, 0x0102_0304);
        assert_eq!(
            bytes,
            [0xEE, 0xEE, 0xEE, 0xEE, 1, 2, 3, 4, 0xEE, 0xEE, 0xEE, 0xEE]
        );
        set_word(&mut bytes, 8, 0);
        assert_eq!(bytes[8..], [0, 0, 0, 0]);
    }

    #[test]
    fn an_overlay_table_rom_keeps_its_records() {
        let files = default_rom_files();
        let overlays = overlays();
        let built = rom_with_overlays(&files, overlays.clone());
        let bytes = &built.bytes;
        assert_eq!(bytes.len() % 4, 0);
        assert_eq!(bytes[..4], BIG_ENDIAN_FIRST_WORD);

        // The ROMDISK is rom's, where rom puts it; zeros follow, but for the table, up to 0x2000.
        let plain = rom(&files);
        assert_eq!(hex(&bytes[..plain.bytes.len()]), hex(&plain.bytes));
        assert_eq!(
            (built.base, built.table, &built.records),
            (plain.base, plain.table, &plain.records)
        );
        let table = 0x12E0;
        assert_eq!(OVERLAY_TABLE_OFFSET, table);
        assert!(all_are(&bytes[plain.bytes.len()..table], 0));
        assert!(all_are(&bytes[table + 5 * 48..0x2000], 0));
        // The record no overlay fills is all 0xFF.
        assert!(all_are(&bytes[table + 3 * 48..table + 4 * 48], 0xFF));

        let mut extent_at = 0x2000;
        for (index, overlay) in [0, 1, 2, 4].into_iter().zip(&overlays) {
            let record = table + index * 48;
            let name = &bytes[record..record + 16];
            assert_eq!(&name[..overlay.name.len()], overlay.name.as_bytes());
            assert!(all_are(&name[overlay.name.len()..], 0));
            let words: [u32; 8] = array::from_fn(|at| word_at(bytes, record + 16 + 4 * at));
            let [
                offset,
                stored,
                address,
                memory,
                bss,
                bss_size,
                reserved @ ..,
            ] = words;
            let len = overlay.data.len() as u32;
            assert_eq!(offset as usize, extent_at, "{}", overlay.name);
            assert_eq!(address, overlay.load_address);
            assert_eq!(memory, len + overlay.bss_size);
            assert_eq!(bss, overlay.load_address + len);
            assert_eq!(bss_size, overlay.bss_size);
            assert_eq!(reserved, [0, 0]);
            // What the stream inflates to fills the memory up to the zeroed part.
            assert_eq!(bss - address, memory - bss_size);
            let end = (offset + stored) as usize;
            let (data, filler) = inflate(&bytes[offset as usize..end]);
            assert_eq!(data, overlay.data, "{}", overlay.name);
            assert_eq!(filler.len(), overlay.padding, "{}", overlay.name);
            assert!(all_are(filler, OVERLAY_FILLER));
            assert_eq!(
                bytes[offset as usize..end - overlay.padding],
                raw_deflate(&overlay.data)[..]
            );
            extent_at = end.next_multiple_of(4);
            assert!(all_are(&bytes[end..extent_at], 0));
        }
        assert_ne!(OVERLAY_FILLER, 0);
        assert_eq!(bytes.len(), extent_at);
        let magics = bytes
            .windows(ROMDISK_MAGIC.len())
            .filter(|window| *window == ROMDISK_MAGIC)
            .count();
        assert_eq!(magics, 1);
    }

    #[test]
    #[should_panic(expected = "reaches the overlay table")]
    fn a_romdisk_that_reaches_the_overlay_table_has_no_overlays() {
        let files = [RomFile::plain("/large", &[0; OVERLAY_TABLE_OFFSET])];
        rom_with_overlays(&files, overlays());
    }
}
