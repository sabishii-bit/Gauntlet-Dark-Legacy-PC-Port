//! The executable overlays of the US ROM, as the N64 extractor writes them into the tree's
//! `_program` folder: code the game copies into memory as it runs, which the ROM keeps
//! raw-DEFLATEd outside the ROMDISK. A table of the boot code describes them, a record each: the
//! overlay's name, padded with NULs, then eight big-endian words, which give where its extent lies
//! in the ROM and how long it is, the address it is loaded at, the memory it takes, where the
//! zeroed memory after its bytes starts and how large that is, and two reserved zeros. Where the
//! table lies is known for the US ROM alone, by its digest, so a ROM of any other digest has no
//! overlays; the table is handed in, so that tests can give a synthetic ROM one of its own.

use std::fmt;

use gdl_files::{DISC_NAMES, NameError, RelPath, check_component};

use super::inflate::{Trailer, inflate};
use super::romdisk::{checked_end, words};
use super::{US_SHA1, WORD_LEN};
use crate::{MediaError, RomRule, Sha1Digest};

/// How many bytes an overlay's record gives its name, which NULs pad.
pub const OVERLAY_NAME_LEN: usize = 16;

/// The folder of the extracted tree that holds the overlays, which no ROMDISK path may begin with.
pub const PROGRAM_FOLDER: &str = "_program";

/// What an overlay's file name ends with, after the name its record gives.
pub const PROGRAM_SUFFIX: &str = ".bin";

/// How many words follow an overlay's name in its record.
const RECORD_WORDS: usize = 8;

/// How many bytes a record holds: the name, then its words.
const RECORD_LEN: usize = OVERLAY_NAME_LEN + RECORD_WORDS * WORD_LEN;

/// The byte that ends a name shorter than its field.
const NAME_END: u8 = 0;

/// What a record of the table is called where it reaches past the ROM's end.
const TABLE_PART: &str = "executable overlay table";

/// What an overlay's extent is called where it reaches past the ROM's end.
const EXTENT_PART: &str = "executable overlay";

/// What the N64 extractor answers for a record whose reserved words are not zero, or whose zeroed
/// memory disagrees with the overlay's length.
const INCONSISTENT_METADATA: &str = "inconsistent executable overlay metadata";

/// How a name holding a byte outside ASCII is refused.
const NON_ASCII_NAME: &str = "non-ASCII executable overlay name";

/// Where a ROM keeps its table of executable overlays, and which of its records describe one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OverlayTable {
    /// The digest, in the big-endian byte order, of the ROM that keeps its table here. A ROM of
    /// any other digest has no overlays.
    pub canonical_sha1: Sha1Digest,
    /// Where the table starts in the ROM.
    pub offset: usize,
    /// How far apart its records start.
    pub record_len: usize,
    /// The records that describe an overlay, counted from the table's first, in the order the
    /// tree lists their overlays.
    pub indices: &'static [usize],
}

/// The US ROM's table: records of 48 bytes from 0x12E0, of which the first three and the fifth
/// describe its four overlays.
pub const US_OVERLAYS: OverlayTable = OverlayTable {
    canonical_sha1: US_SHA1,
    offset: 0x12E0,
    record_len: RECORD_LEN,
    indices: &[0, 1, 2, 4],
};

/// An executable overlay, as its record describes it and its stream inflates.
#[derive(Clone, PartialEq, Eq)]
pub struct Overlay {
    /// Its place in the tree: [`PROGRAM_FOLDER`], then the name its record gives followed by
    /// [`PROGRAM_SUFFIX`].
    pub path: RelPath,
    /// Where its extent starts in the ROM: its raw DEFLATE stream, then the bytes that pad it.
    pub rom_offset: u32,
    /// How many bytes its extent holds.
    pub stored_size: u32,
    /// The address it is loaded at.
    pub load_address: u32,
    /// How much memory it takes once loaded: its bytes, then the zeroed memory after them.
    pub memory_size: u32,
    /// Where that zeroed memory starts, right past its bytes.
    pub bss_address: u32,
    /// How large that zeroed memory is.
    pub bss_size: u32,
    /// Its bytes, inflated: as many as lie from the load address to the zeroed memory.
    pub data: Vec<u8>,
}

impl fmt::Debug for Overlay {
    /// What its record says of it, and how many bytes it holds, without the bytes.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        to.debug_struct("Overlay")
            .field("path", &self.path)
            .field("rom_offset", &self.rom_offset)
            .field("stored_size", &self.stored_size)
            .field("load_address", &self.load_address)
            .field("memory_size", &self.memory_size)
            .field("bss_address", &self.bss_address)
            .field("bss_size", &self.bss_size)
            .field("size", &self.data.len())
            .finish_non_exhaustive()
    }
}

/// The overlays `table` describes in `rom`, a ROM in the big-endian byte order, read as the N64
/// extractor reads the US ROM's, in the order of the table's indices: none when the ROM's digest
/// is not the table's, and otherwise every one, or the first rule of the format one breaks. A
/// record must lie within the ROM, name its overlay in ASCII with a name whose file the tree may
/// hold, keep its reserved words zero and agree with itself on how long the overlay is: from its
/// load address to its zeroed memory, as the memory it takes less that zeroed memory. Its extent
/// must lie within the ROM, and its stream inflate to exactly that length, with no more after it
/// than [`Trailer::OverlayPadding`] allows.
pub(super) fn read(rom: &[u8], table: &OverlayTable) -> Result<Vec<Overlay>, MediaError> {
    if Sha1Digest::of(rom) != table.canonical_sha1 {
        return Ok(Vec::new());
    }
    let len = rom.len() as u64;
    table
        .indices
        .iter()
        .map(|&index| {
            // A place past what a word of 64 bits holds lies past every ROM's end.
            let record = (table.offset as u64)
                .saturating_add((index as u64).saturating_mul(table.record_len as u64));
            checked_end(record, RECORD_LEN as u64, len, TABLE_PART)?;
            let name_at = record as usize;
            let path = program_path(&rom[name_at..name_at + OVERLAY_NAME_LEN])?;
            let [
                offset,
                stored,
                address,
                memory,
                bss,
                bss_size,
                reserved0,
                reserved1,
            ]: [u32; RECORD_WORDS] = words(rom, record + OVERLAY_NAME_LEN as u64);
            // The overlay's bytes run from its load address to its zeroed memory, and fill the
            // memory it takes but for that zeroed memory.
            let length = bss
                .checked_sub(address)
                .filter(|&length| memory.checked_sub(bss_size) == Some(length));
            let (0, 0, Some(length)) = (reserved0, reserved1, length) else {
                return Err(MediaError::rom(
                    RomRule::OverlayMetadata,
                    INCONSISTENT_METADATA,
                ));
            };
            let start = u64::from(offset);
            let end = checked_end(start, u64::from(stored), len, EXTENT_PART)?;
            let extent = &rom[start as usize..end as usize];
            let data = inflate(extent, length as usize, Trailer::OverlayPadding)?;
            Ok(Overlay {
                path,
                rom_offset: offset,
                stored_size: stored,
                load_address: address,
                memory_size: memory,
                bss_address: bss,
                bss_size,
                data,
            })
        })
        .collect()
}

/// The place in the tree of the overlay whose record's name field is `field`: [`PROGRAM_FOLDER`],
/// then the field up to its first NUL followed by [`PROGRAM_SUFFIX`]. The name must be ASCII, and
/// its file a name the tree may hold, judged as one component, so that a name holding a separator
/// is refused rather than read as folders.
fn program_path(field: &[u8]) -> Result<RelPath, MediaError> {
    let end = field
        .iter()
        .position(|&byte| byte == NAME_END)
        .unwrap_or(field.len());
    let name = str::from_utf8(&field[..end])
        .ok()
        .filter(|name| name.is_ascii())
        .ok_or_else(|| MediaError::rom(RomRule::OverlayName, NON_ASCII_NAME))?;
    let file = format!("{name}{PROGRAM_SUFFIX}");
    let refuse_name = |refused: NameError| {
        let detail = format!(
            "unsafe executable overlay name: {:?} ({})",
            refused.name, refused.rule
        );
        MediaError::rom(RomRule::OverlayName, detail)
    };
    check_component(&file, &DISC_NAMES).map_err(refuse_name)?;
    RelPath::parse(&format!("{PROGRAM_FOLDER}/{file}"), &DISC_NAMES).map_err(refuse_name)
}

#[cfg(test)]
mod tests {
    use gdl_files::{NameRule, ROMDISK_NAMES, RelPath};
    use gdl_fixtures::{
        OVERLAY_TABLE_OFFSET, OverlaySpec, default_rom_files, reorder, rom, rom_with_overlays,
        set_word,
    };

    use super::{
        OVERLAY_NAME_LEN, Overlay, OverlayTable, PROGRAM_FOLDER, PROGRAM_SUFFIX, RECORD_LEN,
        US_OVERLAYS, read,
    };
    use crate::vectors::named;
    use crate::{MAX_OVERLAY_PADDING, MediaError, N64Rom, Sha1Digest, US_SHA1};

    /// The word of a record, counted from the first after its name, that says where an
    /// overlay's extent starts.
    const OFFSET: usize = 0;

    /// The word that says how many bytes the extent holds.
    const STORED: usize = 1;

    /// The word that gives the load address.
    const ADDRESS: usize = 2;

    /// The word that gives the memory the overlay takes.
    const MEMORY: usize = 3;

    /// The word that says where the zeroed memory starts.
    const BSS: usize = 4;

    /// The word that says how large the zeroed memory is.
    const BSS_SIZE: usize = 5;

    /// The two reserved words.
    const RESERVED: [usize; 2] = [6, 7];

    /// How many bytes a word of a record holds.
    const WORD_BYTES: usize = 4;

    /// What the N64 extractor answers for a record that disagrees with itself.
    const INCONSISTENT: &str = "inconsistent executable overlay metadata";

    /// What it answers for a stream that does not inflate to the length its record gives.
    const WRONG_END: &str = "DEFLATE output size or end marker does not match its directory";

    /// What the launcher answers for a record that reaches past the ROM's end.
    const TABLE_OUTSIDE: &str = "executable overlay table extends outside its containing data";

    /// What it answers for an extent that reaches past the ROM's end.
    const EXTENT_OUTSIDE: &str = "executable overlay extends outside its containing data";

    /// What it answers for a name holding a byte outside ASCII.
    const NON_ASCII: &str = "non-ASCII executable overlay name";

    /// Words of a record to change, each given as its word and its new value.
    type Changes<'a> = &'a [(usize, u32)];

    /// What a case is expected to give: the rule and the words it is refused with, or none when
    /// it is taken.
    type Expected<'a> = Option<(&'a str, &'a str)>;

    /// Bytes that are not all alike, so that their stream takes more than one code.
    fn noise(len: usize) -> Vec<u8> {
        (0..len).map(|index| (index * 151 % 251) as u8).collect()
    }

    /// Four overlays unlike one another: of noise, empty, of zeros and of text, one of them named
    /// with all 16 bytes, followed by none, one, the most and four bytes of padding.
    fn specs() -> [OverlaySpec; 4] {
        [
            OverlaySpec {
                name: "game",
                data: noise(3000),
                load_address: 0x8000_0400,
                bss_size: 0x40,
                padding: 0,
            },
            OverlaySpec {
                name: "empty",
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
                padding: MAX_OVERLAY_PADDING,
            },
            OverlaySpec {
                name: "last",
                data: b"overlay text ".repeat(40),
                load_address: 0xE000_0400,
                bss_size: 7,
                padding: 4,
            },
        ]
    }

    /// A ROM of the default files with the four overlays in a table where the US ROM keeps its
    /// own.
    fn built() -> Vec<u8> {
        rom_with_overlays(&default_rom_files(), specs()).bytes
    }

    /// The US table's layout, made for the ROM `bytes` hold, as a test injects a table.
    fn table_for(bytes: &[u8]) -> OverlayTable {
        OverlayTable {
            canonical_sha1: Sha1Digest::of(bytes),
            ..US_OVERLAYS
        }
    }

    /// The overlays of the ROM `bytes` hold, read through the table made for it.
    fn injected(bytes: &[u8]) -> Result<Vec<Overlay>, MediaError> {
        read(bytes, &table_for(bytes))
    }

    /// The paths of the overlays the records `indices` of the table made for `bytes` describe.
    fn listed(bytes: &[u8], indices: &'static [usize]) -> Vec<String> {
        let table = OverlayTable {
            indices,
            ..table_for(bytes)
        };
        let overlays = read(bytes, &table).unwrap();
        overlays
            .iter()
            .map(|overlay| overlay.path.as_str().to_owned())
            .collect()
    }

    /// Where the word `word` of the record `index` of the US table's layout lies.
    fn word_place(index: usize, word: usize) -> usize {
        OVERLAY_TABLE_OFFSET + index * RECORD_LEN + OVERLAY_NAME_LEN + WORD_BYTES * word
    }

    /// The big-endian word `word` of the record `index`.
    fn word_of(bytes: &[u8], index: usize, word: usize) -> u32 {
        let at = word_place(index, word);
        u32::from_be_bytes(bytes[at..at + WORD_BYTES].try_into().unwrap())
    }

    /// `bytes` with words of the record `index` changed.
    fn with_words(bytes: &[u8], index: usize, changes: Changes<'_>) -> Vec<u8> {
        let mut changed = bytes.to_vec();
        for &(word, value) in changes {
            set_word(&mut changed, word_place(index, word), value);
        }
        changed
    }

    /// `bytes` with the record `index` naming its overlay `name`, NULs after it.
    fn with_name(bytes: &[u8], index: usize, name: &[u8]) -> Vec<u8> {
        let mut changed = bytes.to_vec();
        let at = OVERLAY_TABLE_OFFSET + index * RECORD_LEN;
        let field = &mut changed[at..at + OVERLAY_NAME_LEN];
        field.fill(0);
        field[..name.len()].copy_from_slice(name);
        changed
    }

    /// Checks that `outcome` refuses the ROM by `rule`, in the words `detail`.
    fn assert_refused(outcome: Result<Vec<Overlay>, MediaError>, rule: &str, detail: &str) {
        let error = outcome.unwrap_err();
        assert!(matches!(error, MediaError::Rom { .. }), "{error:?}");
        assert_eq!(error.rule(), rule, "{error}");
        assert_eq!(error.to_string(), detail);
    }

    #[test]
    fn the_romdisk_may_not_hold_the_program_folder() {
        // The ROMDISK's name rules keep the folder the overlays are written to, so that no file of
        // the ROMDISK can take an overlay's place in the tree.
        assert!(ROMDISK_NAMES.reserved_first.contains(&PROGRAM_FOLDER));
        let path = format!("{PROGRAM_FOLDER}/game{PROGRAM_SUFFIX}");
        let refused = RelPath::parse(&path, &ROMDISK_NAMES).unwrap_err();
        assert_eq!(refused.rule, NameRule::Reserved);
    }

    #[test]
    fn a_rom_of_another_digest_has_no_overlays() {
        let bytes = built();
        let plain = rom(&default_rom_files()).bytes;
        // The US table is the US ROM's alone, as a table made for another ROM is that ROM's.
        for table in [US_OVERLAYS, table_for(&plain)] {
            assert!(read(&bytes, &table).unwrap().is_empty(), "{table:?}");
        }

        // Nothing of the table is read then: not a ROM too short to hold one, nor records that
        // would be refused. A table made for either ROM reads its records, and refuses them.
        let mut unreadable = bytes.clone();
        unreadable[OVERLAY_TABLE_OFFSET..OVERLAY_TABLE_OFFSET + 5 * RECORD_LEN].fill(0xFF);
        for refused in [&plain, &unreadable] {
            assert!(read(refused, &US_OVERLAYS).unwrap().is_empty());
            assert!(read(refused, &table_for(&bytes)).unwrap().is_empty());
        }
        assert_refused(injected(&plain), "outsideData", TABLE_OUTSIDE);
        assert_refused(injected(&unreadable), "overlayName", NON_ASCII);

        // The digest is the ROM's in the cartridge's own order: a dump that keeps its byte pairs
        // swapped holds the overlays of the ROM it is, and a table made for the dump's own bytes
        // finds none.
        let swapped = reorder(&bytes, "pairs");
        let dump = N64Rom::from_bytes(named("dump.v64"), swapped.clone()).unwrap();
        let overlays = dump.overlays(&table_for(&bytes)).unwrap();
        assert_eq!(overlays, injected(&bytes).unwrap());
        assert_eq!(overlays.len(), 4);
        assert!(dump.overlays(&table_for(&swapped)).unwrap().is_empty());
    }

    #[test]
    fn an_injected_table_reads_four_overlays() {
        // The fixture's table lies where the US ROM's does, in records of the same length.
        assert_eq!(US_OVERLAYS.canonical_sha1, US_SHA1);
        assert_eq!(US_OVERLAYS.offset, OVERLAY_TABLE_OFFSET);
        assert_eq!(US_OVERLAYS.record_len, 48);
        assert_eq!(US_OVERLAYS.indices, [0, 1, 2, 4]);

        let specs = specs();
        let bytes = built();
        let cartridge = N64Rom::from_bytes(named("dump.z64"), bytes.clone()).unwrap();
        let overlays = cartridge.overlays(&table_for(&bytes)).unwrap();
        assert_eq!(overlays.len(), specs.len());
        let rows = US_OVERLAYS.indices.iter().zip(&specs).zip(&overlays);
        for ((&index, spec), overlay) in rows {
            let name = spec.name;
            assert_eq!(overlay.path.as_str(), format!("_program/{name}.bin"));
            // Where its extent lies and how long it is, as the builder wrote them into its record.
            assert_eq!(overlay.rom_offset, word_of(&bytes, index, OFFSET), "{name}");
            assert_eq!(
                overlay.stored_size,
                word_of(&bytes, index, STORED),
                "{name}"
            );
            let len = u32::try_from(spec.data.len()).unwrap();
            assert_eq!(overlay.load_address, spec.load_address, "{name}");
            assert_eq!(overlay.memory_size, len + spec.bss_size, "{name}");
            assert_eq!(overlay.bss_address, spec.load_address + len, "{name}");
            assert_eq!(overlay.bss_size, spec.bss_size, "{name}");
            assert!(overlay.data == spec.data, "{name}");
            // No file of the ROMDISK may take its place in the tree.
            let refused = RelPath::parse(overlay.path.as_str(), &ROMDISK_NAMES).unwrap_err();
            assert_eq!(refused.rule, NameRule::Reserved, "{name}");
        }
        // Among them an empty overlay, and one whose extent holds the most padding after its
        // stream.
        assert!(overlays[1].data.is_empty());
        assert_eq!(specs[2].padding, MAX_OVERLAY_PADDING);
        // Its debugging form tells of the record, not of the bytes.
        let shown = format!("{:?}", overlays[0]);
        assert!(
            shown.contains("_program/game.bin") && shown.len() < 512,
            "{shown}"
        );

        // A table lists the records it names, in its order, and none when it names none.
        assert_eq!(
            listed(&bytes, &[4, 0]),
            ["_program/last.bin", "_program/game.bin"]
        );
        assert_eq!(
            listed(&bytes, &[1, 1]),
            ["_program/empty.bin", "_program/empty.bin"]
        );
        assert!(listed(&bytes, &[]).is_empty());

        // A byte of padding more than the most is refused.
        let mut padded = specs.clone();
        padded[2].padding = MAX_OVERLAY_PADDING + 1;
        let bytes = rom_with_overlays(&default_rom_files(), padded).bytes;
        let refused = "unsupported executable overlay padding";
        assert_refused(injected(&bytes), "overlayPadding", refused);
    }

    #[test]
    fn reserved_words_and_bss_must_agree() {
        let bytes = built();
        let first = injected(&bytes).unwrap().remove(0);
        let (address, memory) = (first.load_address, first.memory_size);
        let (bss, bss_size) = (first.bss_address, first.bss_size);
        let inconsistent = Some(("overlayMetadata", INCONSISTENT));
        let wrong_end = Some(("deflateEnd", WRONG_END));
        let cases: [(Changes<'_>, Expected<'_>); 13] = [
            // A reserved word that is not zero.
            (&[(RESERVED[0], 1)], inconsistent),
            (&[(RESERVED[1], 0x8000_0000)], inconsistent),
            // Zeroed memory that starts before the load address, or is larger than the memory the
            // overlay takes, even where both differences would wrap around to the same length.
            (&[(BSS, address - 1)], inconsistent),
            (&[(MEMORY, bss_size - 1)], inconsistent),
            (&[(BSS, address - 1), (MEMORY, bss_size - 1)], inconsistent),
            // Lengths that disagree.
            (&[(BSS, bss + 1)], inconsistent),
            (&[(MEMORY, memory - 1)], inconsistent),
            // A length both agree on that the stream does not inflate to: shorter, longer, or as
            // long as a word allows, which is never made room for at once.
            (&[(BSS, bss - 1), (MEMORY, memory - 1)], wrong_end),
            (&[(BSS, bss + 1), (MEMORY, memory + 1)], wrong_end),
            (
                &[
                    (ADDRESS, 0),
                    (BSS, u32::MAX),
                    (MEMORY, u32::MAX),
                    (BSS_SIZE, 0),
                ],
                wrong_end,
            ),
            // Zeroed memory of any size, and an overlay loaded anywhere, where the record agrees
            // with itself.
            (&[(BSS_SIZE, 0), (MEMORY, memory - bss_size)], None),
            (
                &[(BSS_SIZE, bss_size + 0x1000), (MEMORY, memory + 0x1000)],
                None,
            ),
            (&[(ADDRESS, address + 0x10), (BSS, bss + 0x10)], None),
        ];
        for (changes, expected) in cases {
            let changed = with_words(&bytes, 0, changes);
            match expected {
                Some((rule, detail)) => assert_refused(injected(&changed), rule, detail),
                None => {
                    let overlay = injected(&changed).unwrap().remove(0);
                    assert_eq!(overlay.load_address, word_of(&changed, 0, ADDRESS));
                    assert_eq!(overlay.memory_size, word_of(&changed, 0, MEMORY));
                    assert_eq!(overlay.bss_address, word_of(&changed, 0, BSS));
                    assert_eq!(overlay.bss_size, word_of(&changed, 0, BSS_SIZE));
                    assert!(overlay.data == first.data, "{changes:?}");
                }
            }
        }
    }

    #[test]
    fn an_overlay_outside_the_rom_is_refused() {
        let bytes = built();
        let len = bytes.len();
        let rom_len = u32::try_from(len).unwrap();

        // A record may end where the ROM does: here a copy of the first, after all the rest.
        let mut copied = bytes.clone();
        copied.extend_from_within(OVERLAY_TABLE_OFFSET..OVERLAY_TABLE_OFFSET + RECORD_LEN);
        let at_end = |rom: &[u8]| OverlayTable {
            offset: len,
            indices: &[0],
            ..table_for(rom)
        };
        let first = injected(&bytes).unwrap().remove(0);
        assert_eq!(read(&copied, &at_end(&copied)).unwrap(), [first]);
        // A word shorter, the record reaches past the ROM's end.
        let cut = &copied[..copied.len() - WORD_BYTES];
        assert_refused(read(cut, &at_end(cut)), "outsideData", TABLE_OUTSIDE);
        // So does a record anywhere further, however far, with no place overflowing.
        let further: [(usize, usize, &'static [usize]); 3] = [
            (OVERLAY_TABLE_OFFSET, RECORD_LEN, &[0, 1, 2, 4, 1_000_000]),
            (usize::MAX, RECORD_LEN, &[0]),
            (OVERLAY_TABLE_OFFSET, usize::MAX, &[1]),
        ];
        for (offset, record_len, indices) in further {
            let table = OverlayTable {
                offset,
                record_len,
                indices,
                ..table_for(&bytes)
            };
            assert_refused(read(&bytes, &table), "outsideData", TABLE_OUTSIDE);
        }

        // An extent may end where the ROM does: the last overlay's, taking the zeros that end the
        // ROM on a word as padding too.
        let last = injected(&bytes).unwrap().remove(3);
        let to_end = rom_len - last.rom_offset;
        let longer = with_words(&bytes, 4, &[(STORED, to_end)]);
        let overlay = injected(&longer).unwrap().remove(3);
        assert_eq!(overlay.stored_size, to_end);
        assert!(overlay.data == last.data);
        // A byte further, or anywhere past the ROM's end, it reaches outside the ROM, with no
        // place overflowing.
        let outside: [Changes<'_>; 4] = [
            &[(STORED, to_end + 1)],
            &[(OFFSET, 0), (STORED, rom_len + 1)],
            &[(OFFSET, rom_len + 4), (STORED, 0)],
            &[(OFFSET, u32::MAX), (STORED, u32::MAX)],
        ];
        for changes in outside {
            let changed = with_words(&bytes, 4, changes);
            assert_refused(injected(&changed), "outsideData", EXTENT_OUTSIDE);
        }
        // At the ROM's very end, an extent of nothing lies within it, and holds no stream.
        let nothing = with_words(&bytes, 4, &[(OFFSET, rom_len), (STORED, 0)]);
        assert_refused(injected(&nothing), "deflateEnd", WRONG_END);
    }

    #[test]
    fn an_unsafe_overlay_name_is_refused() {
        let bytes = built();
        // Names the extractor writes as they are, which the launcher refuses by the rule of names
        // their file breaks: a device's name, a separator or another character Windows keeps, or
        // a control character.
        let unsafe_names: [(&[u8], &str, NameRule); 9] = [
            (b"con", "con.bin", NameRule::DeviceName),
            (b"Lpt9", "Lpt9.bin", NameRule::DeviceName),
            (b"CON  ", "CON  .bin", NameRule::DeviceName),
            (b"a/b", "a/b.bin", NameRule::Forbidden),
            (b"..\\x", "..\\x.bin", NameRule::Forbidden),
            (b"c:", "c:.bin", NameRule::Forbidden),
            (b"a*", "a*.bin", NameRule::Forbidden),
            (b"x\ty", "x\ty.bin", NameRule::Control),
            (b"\x7f", "\x7f.bin", NameRule::Control),
        ];
        for (name, file, rule) in unsafe_names {
            let detail = format!("unsafe executable overlay name: {file:?} ({rule})");
            let changed = with_name(&bytes, 0, name);
            assert_refused(injected(&changed), "overlayName", &detail);
        }

        // A byte outside ASCII anywhere in a name, as every byte of the record the US table leaves
        // out is.
        for at in 0..OVERLAY_NAME_LEN {
            for byte in [0x80, 0xFF] {
                let mut name = *b"sixteen-letters!";
                name[at] = byte;
                let changed = with_name(&bytes, 4, &name);
                assert_refused(injected(&changed), "overlayName", NON_ASCII);
            }
        }
        let unused = OverlayTable {
            indices: &[3],
            ..table_for(&bytes)
        };
        assert_refused(read(&bytes, &unused), "overlayName", NON_ASCII);

        // Names the tree may hold: none at all, all sixteen bytes, dots and spaces within. The
        // bytes past a name's NUL are not its own.
        let taken: [(&[u8], &str); 6] = [
            (b"", "_program/.bin"),
            (b"sixteen-letters!", "_program/sixteen-letters!.bin"),
            (b"..", "_program/...bin"),
            (b"a.", "_program/a..bin"),
            (b" a b", "_program/ a b.bin"),
            (b"x\0\xff\x01", "_program/x.bin"),
        ];
        for (name, path) in taken {
            let overlays = injected(&with_name(&bytes, 0, name)).unwrap();
            assert_eq!(overlays[0].path.as_str(), path);
        }
    }
}
