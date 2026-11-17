//! The N64 ROM as the N64 extractor reads it: a dump in any of the three byte orders cartridges
//! are copied in, put back into the big-endian order the cartridge holds, with the digests of the
//! dump as it was given and of the ROM in that order, what the ROM's header says of the
//! cartridge, the ROMDISK that holds the game's files, and the executable overlays the US ROM's
//! boot table describes.

use std::fs::File;
use std::io::Read;
use std::path::Path;
use std::{array, fmt};

use gdl_files::{BLOCK_LEN, Cancel, Job};

use crate::{MediaError, Region, RomRule, Sha1Digest, Sha256Digest, SourceName};

mod inflate;
mod overlays;
mod romdisk;
mod wad;

pub use inflate::{INFLATE_STEP, MAX_OVERLAY_PADDING, Trailer, inflate};
pub use overlays::{
    OVERLAY_NAME_LEN, Overlay, OverlayTable, PROGRAM_FOLDER, PROGRAM_SUFFIX, US_OVERLAYS,
};
pub use romdisk::{
    COMPRESSED, HEADER_WORDS_AT, RECORD_WORDS_LEN, ROMDISK_HEADER_LEN, ROMDISK_LIMITS,
    ROMDISK_MAGIC, Romdisk, RomdiskEntry, RomdiskLimits,
};
pub use wad::{
    MAX_WAD_SECTIONS, MIN_WAD_DIRECTORY, WAD_ROW_LEN, WadByteOrder, WadSection, wad_sections,
};

/// The length of an N64 ROM's header, the least a ROM may hold.
pub const ROM_HEADER_LEN: usize = 64;

/// The most bytes a ROM may hold: 512 Mbit, the largest cartridge made.
pub const MAX_ROM_LEN: u64 = 64 << 20;

/// The SHA-1 digest of the US ROM in the big-endian byte order, the one the N64 extractor knows
/// the US cartridge by.
pub const US_SHA1: Sha1Digest =
    Sha1Digest::from_hex_const("0489dcce749c6a5102681d288ed0616a4b94e99d");

/// How many bytes a word of the ROM holds, and of the native files it holds. A ROM is a whole
/// number of words, and the ROMDISK's pointers and records, the overlays' records and a WAD's
/// header and directory are made of them.
const WORD_LEN: usize = 4;

/// [`WORD_LEN`] as a length in the ROM, whose places are reckoned in 64 bits, so that no place a
/// word gives, with a length added, can overflow.
const WORD_PLACE_LEN: u64 = WORD_LEN as u64;

/// How many bytes a byte-swapped dump swaps at a time.
const PAIR_LEN: usize = 2;

/// The first word of a ROM kept in the big-endian byte order.
const BIG_ENDIAN_FIRST_WORD: [u8; WORD_LEN] = [0x80, 0x37, 0x12, 0x40];

/// That word as a dump keeps it whose byte pairs are swapped.
const BYTE_SWAPPED_FIRST_WORD: [u8; WORD_LEN] = [0x37, 0x80, 0x40, 0x12];

/// That word as a dump keeps it whose every word has its bytes reversed.
const LITTLE_ENDIAN_FIRST_WORD: [u8; WORD_LEN] = [0x40, 0x12, 0x37, 0x80];

/// Where the header keeps the cartridge's title.
const TITLE_AT: usize = 0x20;

/// How many bytes the header gives the title.
const TITLE_LEN: usize = 20;

/// The bytes that pad the title.
const TITLE_PADDING: [u8; 2] = [b' ', 0];

/// Where the header keeps the game code.
const GAME_CODE_AT: usize = 0x3B;

/// How many bytes the game code holds.
const GAME_CODE_LEN: usize = 4;

/// Where the game code keeps the letter of the region the cartridge was made for.
const REGION_LETTER_AT: usize = 3;

/// The region letters of the cartridges made for North America: `E` for the continent, and `N`
/// for Canada.
const NORTH_AMERICA_LETTERS: [u8; 2] = *b"EN";

/// The region letter of a cartridge made for Japan.
const JAPAN_LETTER: u8 = b'J';

/// The region letters of the cartridges made for Europe: `P`, `X`, `Y` and `Z` for the continent,
/// and `D`, `F`, `H`, `I`, `S` and `W` for Germany, France, the Netherlands, Italy, Spain and
/// Scandinavia.
const EUROPE_LETTERS: [u8; 10] = *b"PXYZDFHISW";

/// Where the header keeps the ROM's version.
const VERSION_AT: usize = 0x3F;

/// The first of the bytes the header reads as the printable ASCII characters they are.
const PRINTABLE_FIRST: u8 = b' ';

/// The last of those bytes.
const PRINTABLE_LAST: u8 = b'~';

/// The first of the bytes JIS X 0201 gives to the half-width katakana.
const KATAKANA_FIRST: u8 = 0xA1;

/// The last of those bytes.
const KATAKANA_LAST: u8 = 0xDF;

/// The character Unicode gives the first half-width katakana, U+FF61; the rest follow it in the
/// order of their bytes.
const KATAKANA_FIRST_CHAR: u32 = 0xFF61;

/// What the N64 extractor answers for a ROM too short to hold its header, or that does not end on
/// a whole word.
const INCOMPLETE_HEADER: &str = "ROM must contain a complete, word-aligned N64 header";

/// What the N64 extractor answers for a ROM whose first word is no byte order's.
const UNKNOWN_BYTE_ORDER: &str = "unrecognized Nintendo 64 ROM byte order";

/// The byte order a dump keeps the cartridge's words in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ByteOrder {
    /// The cartridge's own order, as `.z64` files usually keep it.
    BigEndian,
    /// Every pair of bytes swapped, as `.v64` files usually keep it.
    ByteSwapped,
    /// Every word's four bytes reversed, as `.n64` files usually keep it.
    LittleEndian,
}

impl ByteOrder {
    /// The order's name, as the N64 extractor's manifest writes it.
    pub fn as_str(self) -> &'static str {
        match self {
            ByteOrder::BigEndian => "big-endian",
            ByteOrder::ByteSwapped => "byte-swapped",
            ByteOrder::LittleEndian => "little-endian",
        }
    }

    /// The order of a dump whose first word is `first`, or `None` when it is no order's.
    fn of_first_word(first: [u8; WORD_LEN]) -> Option<ByteOrder> {
        match first {
            BIG_ENDIAN_FIRST_WORD => Some(ByteOrder::BigEndian),
            BYTE_SWAPPED_FIRST_WORD => Some(ByteOrder::ByteSwapped),
            LITTLE_ENDIAN_FIRST_WORD => Some(ByteOrder::LittleEndian),
            _ => None,
        }
    }

    /// Puts `bytes`, a whole number of words kept in this order, into the big-endian order, in
    /// place.
    fn normalise(self, bytes: &mut [u8]) {
        match self {
            ByteOrder::BigEndian => {}
            ByteOrder::ByteSwapped => {
                let (pairs, _) = bytes.as_chunks_mut::<PAIR_LEN>();
                for pair in pairs {
                    pair.swap(0, 1);
                }
            }
            ByteOrder::LittleEndian => {
                let (words, _) = bytes.as_chunks_mut::<WORD_LEN>();
                for word in words {
                    word.reverse();
                }
            }
        }
    }
}

/// What an N64 ROM's header says of the cartridge.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RomHeader {
    /// The title, without the spaces and NULs that pad it at either end, each byte read as
    /// JIS X 0201 gives it: printable ASCII as itself, a half-width katakana as its Unicode
    /// character, and any other byte as U+FFFD.
    pub title: String,
    /// The game code, such as `NGXE`: the kind of media, two letters for the game, then the
    /// region's letter.
    pub game_code: [u8; 4],
    /// The ROM's version, counted from 0.
    pub version: u8,
}

impl RomHeader {
    /// The game code as text, each byte read as the title's are.
    pub fn game_code_text(&self) -> String {
        self.game_code.iter().copied().map(header_char).collect()
    }

    /// The region the game code's last letter, the cartridge's destination, lies in: North
    /// America for `E`, or `N` for Canada; Japan for `J`; and Europe for `P`, `X`, `Y` or `Z`, or
    /// the letter of one of its countries, `D`, `F`, `H`, `I`, `S` or `W`. Any other letter, such
    /// as `U` for Australia, names a place outside the three, or none.
    pub fn region(&self) -> Option<Region> {
        match self.game_code[REGION_LETTER_AT] {
            letter if NORTH_AMERICA_LETTERS.contains(&letter) => Some(Region::NorthAmerica),
            JAPAN_LETTER => Some(Region::Japan),
            letter if EUROPE_LETTERS.contains(&letter) => Some(Region::Europe),
            _ => None,
        }
    }
}

/// An N64 ROM read whole and put into the big-endian byte order, with what the N64 extractor
/// records of the dump it was read from.
pub struct N64Rom {
    /// The ROM in the big-endian order, at least a header long and a whole number of words.
    canonical: Vec<u8>,
    /// The order the dump kept the ROM in.
    byte_order: ByteOrder,
    /// The dump's file name as the system gave it.
    source_name: SourceName,
    /// The digest of the dump as it was given.
    source_sha256: Sha256Digest,
}

impl N64Rom {
    /// Reads the ROM at `path`, as [`from_bytes`](Self::from_bytes) reads the bytes, under the
    /// name `path` gives its file. A path that names no file is refused before anything is read,
    /// and a file longer than [`MAX_ROM_LEN`] by its length, before a byte of it is read. The file
    /// is read in blocks of at most [`BLOCK_LEN`] bytes, with the job's cancel looked at before
    /// each, and is only ever read.
    pub fn open(path: &Path, job: &Job<'_>) -> Result<N64Rom, MediaError> {
        let name = SourceName::of(path).ok_or_else(|| MediaError::names_no_file(path))?;
        let file = File::open(path).map_err(|error| MediaError::io(path, error))?;
        let len = file
            .metadata()
            .map_err(|error| MediaError::io(path, error))?
            .len();
        if len > MAX_ROM_LEN {
            return Err(too_large());
        }
        let bytes = read_in_blocks(file, path, len, job.cancel)?;
        N64Rom::from_bytes(name, bytes)
    }

    /// The ROM `bytes` hold, kept in whichever of the three byte orders their first word shows,
    /// put into the big-endian order. `name` is the dump's file name, which the manifest records.
    /// Refused unless the bytes hold a whole header, end on a whole word and begin with the first
    /// word of one of the orders.
    pub fn from_bytes(name: SourceName, mut bytes: Vec<u8>) -> Result<N64Rom, MediaError> {
        if bytes.len() < ROM_HEADER_LEN || !bytes.len().is_multiple_of(WORD_LEN) {
            return Err(MediaError::rom(RomRule::Header, INCOMPLETE_HEADER));
        }
        let byte_order = bytes
            .first_chunk::<WORD_LEN>()
            .and_then(|first| ByteOrder::of_first_word(*first))
            .ok_or_else(|| MediaError::rom(RomRule::ByteOrder, UNKNOWN_BYTE_ORDER))?;
        let source_sha256 = Sha256Digest::of(&bytes);
        byte_order.normalise(&mut bytes);
        Ok(N64Rom {
            canonical: bytes,
            byte_order,
            source_name: name,
            source_sha256,
        })
    }

    /// The ROM in the big-endian byte order, the cartridge's own.
    pub fn canonical(&self) -> &[u8] {
        &self.canonical
    }

    /// The byte order the dump kept the ROM in.
    pub fn byte_order(&self) -> ByteOrder {
        self.byte_order
    }

    /// The dump's file name as the system gave it.
    pub fn source_name(&self) -> &SourceName {
        &self.source_name
    }

    /// How many bytes the dump holds, which putting them in order leaves as it is.
    pub fn source_len(&self) -> u64 {
        self.canonical.len() as u64
    }

    /// The SHA-256 digest of the dump as it was given, in its own byte order.
    pub fn source_sha256(&self) -> Sha256Digest {
        self.source_sha256
    }

    /// The SHA-1 digest of the ROM in the big-endian order, which knows a cartridge in whichever
    /// order it was dumped.
    pub fn canonical_sha1(&self) -> Sha1Digest {
        Sha1Digest::of(&self.canonical)
    }

    /// The SHA-256 digest of the ROM in the big-endian order.
    pub fn canonical_sha256(&self) -> Sha256Digest {
        Sha256Digest::of(&self.canonical)
    }

    /// What the ROM's header says of the cartridge, read in the big-endian order whatever order
    /// the dump kept.
    pub fn header(&self) -> RomHeader {
        // Every field lies within the header, which every ROM holds whole.
        let field = |at: usize, len: usize| &self.canonical[at..at + len];
        let mut game_code = [0; GAME_CODE_LEN];
        game_code.copy_from_slice(field(GAME_CODE_AT, GAME_CODE_LEN));
        let title = without_padding(field(TITLE_AT, TITLE_LEN));
        RomHeader {
            title: title.iter().copied().map(header_char).collect(),
            game_code,
            version: self.canonical[VERSION_AT],
        }
    }

    /// The ROMDISK that holds the game's files, read as the N64 extractor reads it and held to
    /// `limits`, or the first rule of its format it breaks.
    pub fn romdisk(&self, limits: &RomdiskLimits) -> Result<Romdisk<'_>, MediaError> {
        romdisk::read(&self.canonical, limits)
    }

    /// The executable overlays `table` describes, read as the N64 extractor reads the US ROM's:
    /// none unless the digest of the ROM in the big-endian order is the table's, and otherwise
    /// every overlay the table lists, in its order, or the first rule of the format one breaks.
    pub fn overlays(&self, table: &OverlayTable) -> Result<Vec<Overlay>, MediaError> {
        overlays::read(&self.canonical, table)
    }
}

impl fmt::Debug for N64Rom {
    /// What is known of the dump, without the ROM's bytes.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        to.debug_struct("N64Rom")
            .field("byte_order", &self.byte_order)
            .field("source_name", &self.source_name)
            .field("source_len", &self.source_len())
            .field("source_sha256", &self.source_sha256)
            .finish_non_exhaustive()
    }
}

/// What `dump`, the file at `path`, holds: read in blocks of at most [`BLOCK_LEN`] bytes, with
/// `cancel` looked at before each, into room for the `len` bytes its metadata gives, and refused
/// once it runs past [`MAX_ROM_LEN`], as a file that has grown since its length was read does.
fn read_in_blocks(
    dump: impl Read,
    path: &Path,
    len: u64,
    cancel: &Cancel,
) -> Result<Vec<u8>, MediaError> {
    cancel.check()?;
    let mut bytes = Vec::with_capacity(usize::try_from(len).unwrap_or_default());
    // One byte past the bound, so that a file that has grown is refused with no more than that
    // held.
    let mut dump = dump.take(MAX_ROM_LEN + 1);
    loop {
        let read = dump
            .by_ref()
            .take(BLOCK_LEN as u64)
            .read_to_end(&mut bytes)
            .map_err(|error| MediaError::io(path, error))?;
        if read < BLOCK_LEN {
            break;
        }
        cancel.check()?;
    }
    if bytes.len() as u64 > MAX_ROM_LEN {
        return Err(too_large());
    }
    Ok(bytes)
}

/// The refusal of a ROM longer than the largest cartridge.
fn too_large() -> MediaError {
    let detail = format!("ROM is longer than {MAX_ROM_LEN} bytes, the most an N64 cartridge holds");
    MediaError::rom(RomRule::RomTooLarge, detail)
}

/// Where the `size` bytes at `offset` end, refused unless they end by `limit`, the end of the data
/// that holds the part `what` names: the ROM for the ROMDISK's header and directory and for an
/// executable overlay's record and extent, the directory for its pointer table and records, and
/// the pointer table for a file's bytes.
fn checked_end(
    offset: u64,
    size: u64,
    limit: u64,
    what: impl fmt::Display,
) -> Result<u64, MediaError> {
    if offset > limit || size > limit - offset {
        let detail = format!("{what} extends outside its containing data");
        return Err(MediaError::rom(RomRule::OutsideData, detail));
    }
    Ok(offset + size)
}

/// The big-endian word at `at` in `rom`, which the checks before each read keep within the ROM.
fn word_at(rom: &[u8], at: u64) -> u32 {
    let mut word = [0; WORD_LEN];
    word.copy_from_slice(&rom[at as usize..][..WORD_LEN]);
    u32::from_be_bytes(word)
}

/// The `N` big-endian words that start at `at` in `rom`, which the checks before each read keep
/// within the ROM.
fn words<const N: usize>(rom: &[u8], at: u64) -> [u32; N] {
    array::from_fn(|index| word_at(rom, at + WORD_PLACE_LEN * index as u64))
}

/// `bytes` without the title's padding at either end.
fn without_padding(bytes: &[u8]) -> &[u8] {
    let kept = |byte: &u8| !TITLE_PADDING.contains(byte);
    let start = bytes.iter().position(kept).unwrap_or(bytes.len());
    let end = bytes.iter().rposition(kept).map_or(start, |last| last + 1);
    &bytes[start..end]
}

/// The character a byte of the header is in JIS X 0201: printable ASCII as itself, a half-width
/// katakana as its Unicode character, and any other byte as U+FFFD.
fn header_char(byte: u8) -> char {
    match byte {
        PRINTABLE_FIRST..=PRINTABLE_LAST => char::from(byte),
        KATAKANA_FIRST..=KATAKANA_LAST => {
            char::from_u32(KATAKANA_FIRST_CHAR + u32::from(byte - KATAKANA_FIRST))
                .unwrap_or(char::REPLACEMENT_CHARACTER)
        }
        _ => char::REPLACEMENT_CHARACTER,
    }
}

#[cfg(test)]
mod tests {
    use std::fs::{self, File};
    use std::io::{self, Read};
    use std::ops::Range;
    use std::path::Path;

    use gdl_files::{BLOCK_LEN, Cancel, Job, NoProgress};
    use gdl_fixtures::{reorder, unhex};
    use serde_json::Value;

    use super::{
        BIG_ENDIAN_FIRST_WORD, BYTE_SWAPPED_FIRST_WORD, ByteOrder, LITTLE_ENDIAN_FIRST_WORD,
        MAX_ROM_LEN, N64Rom, ROM_HEADER_LEN, RomHeader, US_SHA1, WORD_LEN, read_in_blocks,
    };
    use crate::vectors::{frozen, n64_golden, n64_vectors, named, number, players_rom, text};
    use crate::{
        COMPRESSED, MediaError, ROMDISK_LIMITS, Region, RomRule, Sha1Digest, Sha256Digest,
        US_OVERLAYS, wad_sections,
    };

    /// How many bytes the US cartridge holds: 128 Mbit.
    const US_ROM_LEN: u64 = 16 << 20;

    /// How many bytes the format gives a header's title. The places of the header's fields are
    /// given here apart from the reader's own, so that a reader that looks elsewhere is caught.
    const FORMAT_TITLE_LEN: usize = 20;

    /// Where the format places the title.
    const FORMAT_TITLE: Range<usize> = 0x20..0x20 + FORMAT_TITLE_LEN;

    /// Where the format places the game code.
    const FORMAT_GAME_CODE: Range<usize> = 0x3B..0x3F;

    /// Where the format places the version.
    const FORMAT_VERSION: usize = 0x3F;

    /// A dump the N64 extractor wrote a golden manifest for.
    struct Dump {
        /// The dump's bytes, in the order it keeps them.
        source: Vec<u8>,
        /// The ROM they hold, in the big-endian order.
        canonical: Vec<u8>,
        /// The dump's file name.
        name: String,
        /// The name of the order it keeps, as the vectors give it.
        order: String,
        /// The manifest the extractor wrote for it.
        golden: Value,
    }

    /// A dump of zeros that asks its job to stop once it has given `after` bytes, and counts what
    /// it gives.
    struct StoppingDump<'a> {
        /// How many bytes it has yet to give.
        left: usize,
        /// How many it has given.
        given: usize,
        /// How many it gives before it asks the job to stop.
        after: usize,
        /// The job's request to stop.
        cancel: &'a Cancel,
    }

    impl Read for StoppingDump<'_> {
        fn read(&mut self, into: &mut [u8]) -> io::Result<usize> {
            let len = into.len().min(self.left);
            into[..len].fill(0);
            self.left -= len;
            self.given += len;
            if self.given >= self.after {
                self.cancel.cancel();
            }
            Ok(len)
        }
    }

    /// A job nobody follows, asked to stop only through `cancel`.
    fn quiet(cancel: &Cancel) -> Job<'_> {
        Job {
            cancel,
            progress: &NoProgress,
        }
    }

    /// The default image with `title`, `game_code` and `version` written where the format places
    /// them.
    fn with_header(title: &[u8; FORMAT_TITLE_LEN], game_code: &[u8; 4], version: u8) -> Vec<u8> {
        let mut image = frozen(&n64_vectors(), "default");
        image[FORMAT_TITLE].copy_from_slice(title);
        image[FORMAT_GAME_CODE].copy_from_slice(game_code);
        image[FORMAT_VERSION] = version;
        image
    }

    /// The header of the ROM `bytes` hold.
    fn header_of(bytes: Vec<u8>) -> RomHeader {
        N64Rom::from_bytes(named("dump.z64"), bytes)
            .unwrap()
            .header()
    }

    #[test]
    fn byte_orders_normalise_to_the_same_rom() {
        let vectors = n64_vectors();
        // The frozen images as the extractor read them, then the default image in the two other
        // orders.
        let mut dumps = Vec::new();
        for extraction in vectors["extractions"].as_array().unwrap() {
            let image = frozen(&vectors, text(&extraction["image"]));
            dumps.push(Dump {
                source: image.clone(),
                canonical: image,
                name: text(&extraction["name"]).to_owned(),
                order: "big-endian".to_owned(),
                golden: n64_golden(&extraction["golden"]),
            });
        }
        let default = frozen(&vectors, "default");
        for case in vectors["byteOrders"].as_array().unwrap() {
            dumps.push(Dump {
                source: reorder(&default, text(&case["transform"])),
                canonical: default.clone(),
                name: text(&case["name"]).to_owned(),
                order: text(&case["byteOrder"]).to_owned(),
                golden: n64_golden(&case["golden"]),
            });
        }

        let mut orders = Vec::new();
        for dump in dumps {
            let name = &dump.name;
            let rom = N64Rom::from_bytes(named(name), dump.source.clone()).unwrap();
            assert!(rom.canonical() == dump.canonical, "{name}");
            assert_eq!(rom.byte_order().as_str(), dump.order, "{name}");
            assert_eq!(rom.source_name(), &named(name), "{name}");
            assert_eq!(
                rom.source_sha256(),
                Sha256Digest::of(&dump.source),
                "{name}"
            );
            assert_eq!(rom.canonical_sha1(), Sha1Digest::of(&dump.canonical));
            assert_eq!(rom.canonical_sha256(), Sha256Digest::of(&dump.canonical));
            // What the extractor's manifest records of the dump is what the reader gives.
            let recorded = &dump.golden["source"];
            assert_eq!(recorded["name"], name.as_str());
            assert_eq!(recorded["size"], rom.source_len(), "{name}");
            assert_eq!(recorded["byte_order"], rom.byte_order().as_str(), "{name}");
            assert_eq!(recorded["sha256"], rom.source_sha256().hex(), "{name}");
            let canonical = rom.canonical_sha256().hex();
            assert_eq!(recorded["canonical_sha256"], canonical, "{name}");
            orders.push(rom.byte_order());
        }
        for order in [
            ByteOrder::BigEndian,
            ByteOrder::ByteSwapped,
            ByteOrder::LittleEndian,
        ] {
            assert!(orders.contains(&order), "{order:?} among {orders:?}");
        }
    }

    #[test]
    fn short_misaligned_and_unknown_headers_are_refused() {
        let vectors = n64_vectors();
        let cases = vectors["headers"].as_array().unwrap();
        assert!(!cases.is_empty());
        for case in cases {
            // Bytes of the case's own, or a frozen image cut short.
            let bytes = match case.get("hex") {
                Some(hex) => unhex(hex),
                None => {
                    let image = frozen(&vectors, text(&case["image"]));
                    let drop = number(&case["drop"]);
                    image[..image.len() - drop].to_vec()
                }
            };
            let error = N64Rom::from_bytes(named("dump.z64"), bytes).unwrap_err();
            assert!(matches!(error, MediaError::Rom { .. }), "{error:?}");
            assert_eq!(case["rule"], error.rule(), "{case}");
            // The extractor's own words, which are fixed.
            assert_eq!(case["python"], error.to_string(), "{case}");
        }

        // Every length short of a header, or past a whole word, is refused whatever its first
        // word, and a whole header in each order is taken.
        let firsts = [
            (BIG_ENDIAN_FIRST_WORD, ByteOrder::BigEndian),
            (BYTE_SWAPPED_FIRST_WORD, ByteOrder::ByteSwapped),
            (LITTLE_ENDIAN_FIRST_WORD, ByteOrder::LittleEndian),
        ];
        for (first, order) in firsts {
            let lens = (0..ROM_HEADER_LEN).chain((1..WORD_LEN).map(|part| ROM_HEADER_LEN + part));
            for len in lens {
                let mut bytes = vec![0; len];
                let start = len.min(WORD_LEN);
                bytes[..start].copy_from_slice(&first[..start]);
                let error = N64Rom::from_bytes(named("dump.z64"), bytes).unwrap_err();
                assert_eq!(error.rule(), "header", "{len}");
            }
            for len in [ROM_HEADER_LEN, ROM_HEADER_LEN + WORD_LEN] {
                let mut bytes = vec![0; len];
                bytes[..WORD_LEN].copy_from_slice(&first);
                let rom = N64Rom::from_bytes(named("dump.z64"), bytes).unwrap();
                assert_eq!(rom.byte_order(), order, "{len}");
                assert_eq!(rom.source_len(), len as u64);
            }
        }

        // A first word that is no order's is refused, though it holds the right bytes.
        let unknown = [
            [0x12, 0x40, 0x80, 0x37],
            [0x80, 0x37, 0x40, 0x12],
            [0x37, 0x80, 0x12, 0x40],
            [0x40, 0x12, 0x80, 0x37],
            [0x80, 0x37, 0x12, 0x41],
            [0; WORD_LEN],
        ];
        for first in unknown {
            let mut bytes = vec![0; ROM_HEADER_LEN];
            bytes[..WORD_LEN].copy_from_slice(&first);
            let error = N64Rom::from_bytes(named("dump.z64"), bytes).unwrap_err();
            assert!(
                matches!(
                    &error,
                    MediaError::Rom {
                        rule: RomRule::ByteOrder,
                        ..
                    }
                ),
                "{first:x?}: {error:?}"
            );
        }
    }

    #[test]
    fn a_rom_over_64_mib_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("large.z64");
        let file = File::create(&path).unwrap();
        // A job already asked to stop, so that a ROM the bound takes stops before its first block.
        let cancel = Cancel::default();
        cancel.cancel();
        let job = quiet(&cancel);

        // The bound itself is taken: only the cancel stops the read.
        file.set_len(MAX_ROM_LEN).unwrap();
        let error = N64Rom::open(&path, &job).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");

        // A word more is refused by the file's length, before the cancel is looked at and before
        // a byte is read.
        file.set_len(MAX_ROM_LEN + WORD_LEN as u64).unwrap();
        let error = N64Rom::open(&path, &job).unwrap_err();
        assert!(
            matches!(
                &error,
                MediaError::Rom {
                    rule: RomRule::RomTooLarge,
                    ..
                }
            ),
            "{error:?}"
        );
        assert_eq!(error.rule(), "romTooLarge");
        assert_eq!(
            error.to_string(),
            "ROM is longer than 67108864 bytes, the most an N64 cartridge holds"
        );
        assert_eq!(MAX_ROM_LEN, 64 * 1024 * 1024);
        drop(file);
        assert_eq!(fs::metadata(&path).unwrap().len(), MAX_ROM_LEN + 4);
    }

    #[test]
    fn the_header_names_a_rom() {
        // The default image's header is zeros but for its first word: it has no title, and its
        // game code is four NULs, which name no region.
        let plain = header_of(frozen(&n64_vectors(), "default"));
        let nothing = RomHeader {
            title: String::new(),
            game_code: [0; 4],
            version: 0,
        };
        assert_eq!(plain, nothing);
        assert_eq!(plain.game_code_text(), "\u{fffd}".repeat(4));
        assert_eq!(plain.region(), None);

        let image = with_header(b"GAUNTLET LEGENDS    ", b"NGXE", 2);
        let header = header_of(image.clone());
        assert_eq!(header.title, "GAUNTLET LEGENDS");
        assert_eq!(header.game_code, *b"NGXE");
        assert_eq!(header.game_code_text(), "NGXE");
        assert_eq!(header.version, 2);
        assert_eq!(header.region(), Some(Region::NorthAmerica));
        // The header is read in the cartridge's own order, whatever order the dump keeps.
        for transform in ["pairs", "words"] {
            assert_eq!(header_of(reorder(&image, transform)), header, "{transform}");
        }

        // Each field is read where the format places it, and no further: a title that fills its
        // field, and bytes just outside the fields that are not padding, which are no part of them.
        let mut marked = with_header(b"ABCDEFGHIJKLMNOPQRST", b"NGXE", 2);
        for outside in [
            FORMAT_TITLE.start - 1,
            FORMAT_TITLE.end,
            FORMAT_GAME_CODE.start - 1,
        ] {
            marked[outside] = b'x';
        }
        let header = header_of(marked);
        assert_eq!(header.title, "ABCDEFGHIJKLMNOPQRST");
        assert_eq!(header.game_code, *b"NGXE");
        assert_eq!(header.version, 2);

        // The game code's last letter is the cartridge's destination, and names the region it
        // lies in: North America, Japan or Europe, or one of their countries.
        let regions = [
            (b'E', Some(Region::NorthAmerica)),
            (b'N', Some(Region::NorthAmerica)),
            (b'J', Some(Region::Japan)),
            (b'P', Some(Region::Europe)),
            (b'X', Some(Region::Europe)),
            (b'Y', Some(Region::Europe)),
            (b'Z', Some(Region::Europe)),
            (b'D', Some(Region::Europe)),
            (b'F', Some(Region::Europe)),
            (b'H', Some(Region::Europe)),
            (b'I', Some(Region::Europe)),
            (b'S', Some(Region::Europe)),
            (b'W', Some(Region::Europe)),
            // Australia, Brazil, China and Korea lie outside the three, `A` is for every region,
            // and `G` and `L` name a console's model, not a region.
            (b'U', None),
            (b'B', None),
            (b'C', None),
            (b'K', None),
            (b'A', None),
            (b'G', None),
            (b'L', None),
            // Neither a letter in lower case nor a NUL is a destination.
            (b'e', None),
            (0, None),
        ];
        for (letter, region) in regions {
            let header = header_of(with_header(
                b"GAUNTLET LEGENDS    ",
                &[b'N', b'G', b'X', letter],
                0,
            ));
            assert_eq!(header.region(), region, "{letter:?}");
        }

        // A title is JIS X 0201: the half-width katakana from 0xA1 to 0xDF, 0xB1 among them as
        // U+FF71, printable ASCII as itself, and every other byte U+FFFD. Spaces and NULs pad it
        // at either end, but stay where they stand within it.
        let title = b"\0 \xb1\xa1\xdf \xa0\xe0\x7f\x1fA\0~ \0 \0\0  ";
        let header = header_of(with_header(title, b"NGDJ", 0));
        assert_eq!(
            header.title,
            "\u{ff71}\u{ff61}\u{ff9f} \u{fffd}\u{fffd}\u{fffd}\u{fffd}A\u{fffd}~"
        );
        assert_eq!(header.region(), Some(Region::Japan));
        let katakana = header_of(with_header(b"\xb1\xb2\xb3                 ", b"\xb1Z~ ", 1));
        assert_eq!(katakana.title, "\u{ff71}\u{ff72}\u{ff73}");
        assert_eq!(katakana.game_code_text(), "\u{ff71}Z~ ");
        assert_eq!(katakana.region(), None);
        let padding = header_of(with_header(&[b' '; FORMAT_TITLE_LEN], b"NGXP", 0));
        assert_eq!(padding.title, "");
        assert_eq!(padding.region(), Some(Region::Europe));
    }

    #[test]
    fn a_dump_is_read_whole_and_left_as_it_was() {
        let default = frozen(&n64_vectors(), "default");
        let dir = tempfile::tempdir().unwrap();
        let cancel = Cancel::default();

        // A dump of more than two blocks: the default image, then zeros.
        let path = dir.path().join("source.z64");
        fs::write(&path, &default).unwrap();
        let len = 2 * BLOCK_LEN + default.len();
        let file = File::options().write(true).open(&path).unwrap();
        file.set_len(len as u64).unwrap();
        drop(file);
        let mut held = default.clone();
        held.resize(len, 0);
        let rom = N64Rom::open(&path, &quiet(&cancel)).unwrap();
        assert!(rom.canonical() == held, "the dump read back");
        assert_eq!(rom.byte_order(), ByteOrder::BigEndian);
        assert_eq!(rom.source_len(), len as u64);
        assert_eq!(rom.source_sha256(), Sha256Digest::of(&held));
        assert_eq!(rom.source_name(), &named("source.z64"));
        // Its debugging form tells of the dump, not of every byte.
        let shown = format!("{rom:?}");
        assert!(shown.contains("BigEndian") && shown.len() < 512, "{shown}");
        assert!(fs::read(&path).unwrap() == held, "the dump is as it was");

        // A dump in another order is read as its bytes are, under the name its path gives,
        // letter case included.
        let swapped = dir.path().join("Source (Swapped).V64");
        let bytes = reorder(&default, "pairs");
        fs::write(&swapped, &bytes).unwrap();
        let rom = N64Rom::open(&swapped, &quiet(&cancel)).unwrap();
        let read = N64Rom::from_bytes(named("Source (Swapped).V64"), bytes.clone()).unwrap();
        assert_eq!(rom.canonical(), read.canonical());
        assert_eq!(rom.canonical(), default);
        assert_eq!(rom.byte_order(), ByteOrder::ByteSwapped);
        assert_eq!(rom.source_name(), read.source_name());
        assert_eq!(rom.source_sha256(), read.source_sha256());
        assert_eq!(fs::read(&swapped).unwrap(), bytes);

        // Asked to stop, the read stops before its first block, and the dump is as it was.
        cancel.cancel();
        let error = N64Rom::open(&swapped, &quiet(&cancel)).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        assert_eq!(fs::read(&swapped).unwrap(), bytes);

        // A dump that is not there is a failure to read it, which names its path.
        let missing = dir.path().join("missing.z64");
        let error = N64Rom::open(&missing, &quiet(&Cancel::default())).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path, source }
                if *path == missing && source.kind() == io::ErrorKind::NotFound),
            "{error:?}"
        );
        assert_eq!(error.rule(), "io");
    }

    #[test]
    fn a_read_stops_between_blocks_once_its_job_is_asked_to() {
        // A dump a byte longer than a block, whose job is asked to stop as its first block is
        // read: the read stops before the second block, and takes no byte of it.
        let cancel = Cancel::default();
        let mut dump = StoppingDump {
            left: BLOCK_LEN + 1,
            given: 0,
            after: BLOCK_LEN,
            cancel: &cancel,
        };
        let error = read_in_blocks(&mut dump, Path::new("dump.z64"), 0, &cancel).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        assert_eq!(dump.given, BLOCK_LEN);
    }

    #[test]
    fn a_path_that_names_no_file_is_refused_before_anything_is_read() {
        let dir = tempfile::tempdir().unwrap();
        let cancel = Cancel::default();
        // Each is a folder that is there, which reading would fail on in a way of its own.
        let root = dir.path().ancestors().last().unwrap().to_path_buf();
        let unnamed = [dir.path().join(".."), root, Path::new("..").to_path_buf()];
        for path in unnamed {
            let error = N64Rom::open(&path, &quiet(&cancel)).unwrap_err();
            let MediaError::Io { path: at, source } = &error else {
                panic!("{}: {error:?}", path.display());
            };
            assert_eq!(*at, path);
            assert_eq!(source.kind(), io::ErrorKind::InvalidInput, "{error}");
            assert_eq!(source.to_string(), "the path names no file");
            assert_eq!(error.rule(), "io");
        }
    }

    #[test]
    #[ignore = "needs the player's dump"]
    fn the_players_rom_holds_the_whole_native_corpus() {
        let rom = N64Rom::open(&players_rom(), &quiet(&Cancel::default())).unwrap();
        // The US cartridge the extractor knows, dumped in its own order.
        assert_eq!(rom.canonical_sha1(), US_SHA1);
        assert_eq!(rom.byte_order(), ByteOrder::BigEndian);
        assert_eq!(rom.source_len(), US_ROM_LEN);
        // Its header names the game, the US cartridge's game code and the region it was made for,
        // read where the format places them. A failure says which, never what the header holds.
        let header = rom.header();
        assert!(header.title == "GAUNTLET LEGENDS", "another title");
        assert!(header.game_code == *b"NGXE", "another game code");
        assert!(
            header.region() == Some(Region::NorthAmerica),
            "another region"
        );

        // Every file of its ROMDISK decodes to the size its record gives, and every WAD among them
        // describes its sections. A failure names a file by its place in the directory and the
        // rule it breaks, never by what it holds.
        let romdisk = rom
            .romdisk(&ROMDISK_LIMITS)
            .unwrap_or_else(|error| panic!("the ROMDISK is refused: {}", error.rule()));
        let entries = &romdisk.entries;
        assert_eq!(entries.len(), 618);
        let compressed = entries
            .iter()
            .filter(|entry| entry.flags == COMPRESSED)
            .count();
        assert_eq!(compressed, 586);
        let (mut wads, mut maps) = (0, 0);
        for (index, entry) in entries.iter().enumerate() {
            let data = romdisk
                .decode(entry)
                .unwrap_or_else(|error| panic!("file {index}: {}", error.rule()));
            assert_eq!(data.len(), entry.size as usize, "file {index}");
            let path = entry.path.as_str();
            if path.to_ascii_lowercase().ends_with(".wad") {
                if let Err(error) = wad_sections(&entry.path, &data) {
                    panic!("file {index}: {}", error.rule());
                }
                wads += 1;
            }
            if path.starts_with("levels/") && path.ends_with("/worlds.wad") {
                maps += 1;
            }
        }
        assert_eq!((wads, maps), (445, 34));

        // Its boot table describes four overlays, the last of them loaded high in memory.
        let overlays = rom
            .overlays(&US_OVERLAYS)
            .unwrap_or_else(|error| panic!("the overlays are refused: {}", error.rule()));
        assert_eq!(overlays.len(), 4);
        let last = &overlays[3];
        assert_eq!(last.load_address, 0xE000_0400);
        assert_eq!(last.data.len(), 0xACAF0);
    }
}
