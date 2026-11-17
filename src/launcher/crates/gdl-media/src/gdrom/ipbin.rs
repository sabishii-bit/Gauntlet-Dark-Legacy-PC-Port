//! IP.BIN, the boot area a GD-ROM's high-density area begins with: sixteen sectors from LBA 45000,
//! the first of which holds the Dreamcast's hardware ID, the areas of the world the disc was made
//! for, the product's number and version, the executable the boot loads and the title. The
//! Dreamcast extractor writes the boot area whole beside the disc's files and records three of its
//! fields, read as ASCII text.

use std::fmt;
use std::ops::Range;

use super::{SECTOR, ascii_text};
use crate::MediaError;

/// The LBA of the boot area's first sector: the first of the high-density area.
pub const BOOT_LBA: u32 = 45_000;

/// How many sectors the boot area takes.
pub const BOOT_SECTORS: u32 = 16;

/// How many bytes of user data the boot area's sectors hold.
pub const BOOT_LEN: usize = BOOT_SECTORS as usize * SECTOR;

/// What the boot area begins with on every Dreamcast disc: the console's hardware ID.
pub const HARDWARE_ID: &[u8; 16] = b"SEGA SEGAKATANA ";

/// What Gauntlet Legends' title begins with.
pub const TITLE_PREFIX: &[u8; 16] = b"GAUNTLET LEGENDS";

/// Where the boot area names the areas of the world the disc was made for, a letter each in its
/// own place, such as `" U      "` for North America alone.
pub const AREA: Range<usize> = 48..56;

/// Where the boot area gives the product's number, such as `T9710N`, padded with spaces.
pub const PRODUCT: Range<usize> = 64..74;

/// Where the boot area gives the product's version, such as `V1.003`.
pub const VERSION: Range<usize> = 74..80;

/// Where the boot area names the executable its boot loads, padded with spaces.
pub const EXECUTABLE: Range<usize> = 96..112;

/// Where the boot area gives the disc's title, padded.
pub const TITLE: Range<usize> = 128..256;

/// A disc's boot area as it was read, with the three text fields the Dreamcast extractor records
/// of it, decoded when it was read so that a field it could not decode refuses the disc before
/// anything is written.
#[derive(Clone, PartialEq, Eq)]
pub struct IpBin {
    /// The boot area's [`BOOT_LEN`] bytes.
    bytes: Vec<u8>,
    /// The product's number, stripped.
    product: String,
    /// The title, stripped.
    title: String,
    /// The executable's name, stripped.
    executable: String,
}

impl IpBin {
    /// The boot area `bytes`, the [`BOOT_LEN`] bytes of user data its sectors hold, with its
    /// product, title and executable read as the Dreamcast extractor reads them: ASCII, without
    /// the white space at either end, and with everything within kept, NULs included. Refused by
    /// the first of the three, in that order, that holds a byte outside ASCII. It does not judge
    /// whose boot area it is: the reader does that before decoding it.
    pub(crate) fn decode(bytes: Vec<u8>) -> Result<IpBin, MediaError> {
        debug_assert_eq!(bytes.len(), BOOT_LEN, "a boot area is read whole");
        let product = ascii_text(&bytes[PRODUCT], "IP.BIN's product")?;
        let title = ascii_text(&bytes[TITLE], "IP.BIN's title")?;
        let executable = ascii_text(&bytes[EXECUTABLE], "IP.BIN's executable")?;
        Ok(IpBin {
            bytes,
            product,
            title,
            executable,
        })
    }

    /// The boot area's bytes, as the extracted tree's IP.BIN holds them.
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// The bytes of the field `range` places, such as [`AREA`] or [`VERSION`], as the boot area
    /// keeps them. Panics when `range` reaches past the boot area, as slicing does.
    pub fn field(&self, range: Range<usize>) -> &[u8] {
        &self.bytes[range]
    }

    /// The product's number, without the white space that pads it.
    pub fn product(&self) -> &str {
        &self.product
    }

    /// The disc's title, without the white space that pads it; NULs after it are kept, as the
    /// Dreamcast extractor keeps them.
    pub fn title(&self) -> &str {
        &self.title
    }

    /// The name of the executable the boot loads, without the white space that pads it.
    pub fn executable(&self) -> &str {
        &self.executable
    }
}

impl fmt::Debug for IpBin {
    /// The fields the extraction records, without the boot area's bytes.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        to.debug_struct("IpBin")
            .field("product", &self.product)
            .field("title", &self.title)
            .field("executable", &self.executable)
            .finish_non_exhaustive()
    }
}

/// Whether `boot`, a boot area as it was read, is Gauntlet Legends' on the Dreamcast, as the
/// Dreamcast extractor knows it: the hardware ID first, and a title that begins with the game's
/// name.
pub(super) fn names_gauntlet_legends(boot: &[u8]) -> bool {
    boot.starts_with(HARDWARE_ID)
        && boot
            .get(TITLE)
            .is_some_and(|title| title.starts_with(TITLE_PREFIX))
}

#[cfg(test)]
mod tests {
    use std::ops::Range;

    use gdl_fixtures::{DiscSet, Edc};

    use super::{
        AREA, BOOT_LBA, BOOT_LEN, BOOT_SECTORS, EXECUTABLE, HARDWARE_ID, PRODUCT, TITLE,
        TITLE_PREFIX, VERSION, names_gauntlet_legends,
    };
    use crate::vectors::{dreamcast_golden, dreamcast_vectors, text};
    use crate::{IpBin, MediaError, RAW_SECTOR, SCRIPT_RULES, read_head};

    /// The LBA the format gives the boot area's first sector. The format's facts are given here
    /// apart from the reader's own constants, so that a reader that looks elsewhere is caught.
    const FORMAT_BOOT_LBA: u32 = 45_000;

    /// How many sectors the format gives the boot area.
    const FORMAT_BOOT_SECTORS: u32 = 16;

    /// How many bytes of user data a sector holds.
    const FORMAT_SECTOR: usize = 2048;

    /// Where a raw sector keeps its user data, after its sync and header.
    const FORMAT_USER_DATA_AT: usize = 16;

    /// What the boot area begins with on every Dreamcast disc.
    const FORMAT_HARDWARE_ID: &[u8; 16] = b"SEGA SEGAKATANA ";

    /// What Gauntlet Legends' title begins with.
    const FORMAT_TITLE_PREFIX: &[u8; 16] = b"GAUNTLET LEGENDS";

    /// Where the format keeps the areas of the world the disc was made for.
    const FORMAT_AREA: Range<usize> = 48..56;

    /// Where it keeps the product's number.
    const FORMAT_PRODUCT: Range<usize> = 64..74;

    /// Where it keeps the product's version.
    const FORMAT_VERSION: Range<usize> = 74..80;

    /// Where it keeps the executable's name.
    const FORMAT_EXECUTABLE: Range<usize> = 96..112;

    /// Where it keeps the title.
    const FORMAT_TITLE: Range<usize> = 128..256;

    /// The file of the cue sets' first data track, whose first sector begins the boot area.
    const FIRST_TRACK: &str = "track3.bin";

    /// The boot area of the disc `set` holds, read with its head under the extractor's rules, or
    /// the rule and the words it is refused by, a rule of the disc's format.
    fn boot_of(set: &DiscSet) -> Result<IpBin, (&'static str, String)> {
        let dir = tempfile::tempdir().unwrap();
        let sheet = set.write(dir.path()).unwrap();
        let head = read_head(&sheet, &SCRIPT_RULES).map_err(|error| {
            assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
            (error.rule(), error.to_string())
        })?;
        Ok(head.boot)
    }

    /// The refusal by `rule` with `words`.
    fn said(rule: &'static str, words: &str) -> Result<IpBin, (&'static str, String)> {
        Err((rule, words.to_owned()))
    }

    /// `set` with `bytes` written over its boot area from `at`, a place in the area's user data
    /// within one sector, as a chunk of its first data track's file written after the set's own.
    /// The sector's code is left as it was, so only the extractor's rules read it.
    fn with_boot_bytes(mut set: DiscSet, at: usize, bytes: &[u8]) -> DiscSet {
        let (sector, within) = (at / FORMAT_SECTOR, at % FORMAT_SECTOR);
        assert!(within + bytes.len() <= FORMAT_SECTOR);
        let place = sector * RAW_SECTOR + FORMAT_USER_DATA_AT + within;
        let (_, track) = set
            .files
            .iter_mut()
            .find(|(name, _)| name == FIRST_TRACK)
            .unwrap();
        track.chunks.push((place as u64, bytes.to_vec()));
        set
    }

    /// The three text fields the extraction records, as `boot` gives them.
    fn fields(boot: &IpBin) -> (&str, &str, &str) {
        (boot.product(), boot.title(), boot.executable())
    }

    #[test]
    fn fields_strip_as_the_script_strips() {
        let vectors = dreamcast_vectors();
        let golden =
            |set: &str| dreamcast_golden(&vectors["sets"][set]["goldens"]["disc-manifest.json"]);
        // The small set's title is followed by NULs, which are no white space: all 112 of them
        // are kept, as the extractor's manifest records them.
        let small = boot_of(&DiscSet::small(Edc::Zero)).unwrap();
        let title = format!("GAUNTLET LEGENDS{}", "\0".repeat(112));
        assert_eq!(fields(&small), ("T9710N", title.as_str(), "1ST_READ.BIN"));
        assert_eq!(small.title(), text(&golden("small")["title"]));
        // The full set's title is padded with spaces, which are stripped.
        let full = boot_of(&DiscSet::full(Edc::Zero)).unwrap();
        assert_eq!(
            fields(&full),
            ("T9710N", "GAUNTLET LEGENDS", "1ST_READ.BIN")
        );
        assert_eq!(full.title(), text(&golden("full")["title"]));

        // The information separators the extractor strips are stripped from either end too, and
        // what lies within a field is kept, white space included.
        let padded = with_boot_bytes(
            DiscSet::full(Edc::Zero),
            FORMAT_PRODUCT.start,
            b"\x1cT9710N\x1c\x1d\x1e",
        );
        let padded = with_boot_bytes(padded, FORMAT_EXECUTABLE.start, b"\x1f 1ST READ.BIN\t\x1c");
        let padded = with_boot_bytes(padded, FORMAT_TITLE.start + 16, &[0x1c; 112]);
        let boot = boot_of(&padded).unwrap();
        assert_eq!(
            fields(&boot),
            ("T9710N", "GAUNTLET LEGENDS", "1ST READ.BIN")
        );

        // The fields judged as bytes are the boot area's own, as it keeps them: the full set's area
        // and version, and the small set's, which are NULs.
        assert_eq!(full.field(AREA), b" U      ");
        assert_eq!(full.field(VERSION), b"V1.003");
        assert_eq!(small.field(AREA), [0; 8]);
        assert_eq!(small.field(VERSION), [0; 6]);
        assert_eq!(full.field(0..16), FORMAT_HARDWARE_ID);
        assert_eq!(
            full.bytes().len(),
            FORMAT_BOOT_SECTORS as usize * FORMAT_SECTOR
        );
        assert_eq!(full.bytes()[FORMAT_PRODUCT], *b"T9710N    ");
        // Its debug form names the fields and leaves the bytes out.
        let shown = format!("{full:?}");
        assert!(shown.contains("T9710N") && shown.len() < 200, "{shown}");
    }

    #[test]
    fn non_ascii_fields_are_refused() {
        // A byte outside ASCII in any of the three fields the extraction records refuses the disc
        // before anything is written, the field named; the extractor would refuse it only after
        // writing the tree.
        let cases = [
            (
                FORMAT_PRODUCT.start + 3,
                "IP.BIN's product holds a byte outside ASCII",
            ),
            (
                FORMAT_TITLE.start + 20,
                "IP.BIN's title holds a byte outside ASCII",
            ),
            (
                FORMAT_TITLE.end - 1,
                "IP.BIN's title holds a byte outside ASCII",
            ),
            (
                FORMAT_EXECUTABLE.start,
                "IP.BIN's executable holds a byte outside ASCII",
            ),
        ];
        for (at, words) in cases {
            for byte in [0x80, 0xE9, 0xFF] {
                let set = with_boot_bytes(DiscSet::full(Edc::Zero), at, &[byte]);
                assert_eq!(boot_of(&set), said("textField", words), "{at} {byte:#04x}");
            }
        }
        // So is a character outside ASCII written in UTF-8, within a field, past the title's
        // game's name, or in the padding at a field's end, where a no-break space would otherwise
        // be stripped as white space.
        let places = [
            (
                FORMAT_PRODUCT.start + 2,
                FORMAT_PRODUCT.end,
                "IP.BIN's product holds a byte outside ASCII",
            ),
            (
                FORMAT_TITLE.start + 20,
                FORMAT_TITLE.end,
                "IP.BIN's title holds a byte outside ASCII",
            ),
            (
                FORMAT_EXECUTABLE.start + 2,
                FORMAT_EXECUTABLE.end,
                "IP.BIN's executable holds a byte outside ASCII",
            ),
        ];
        for (within, end, words) in places {
            for character in ["\u{e9}", "\u{a0}"] {
                let bytes = character.as_bytes();
                for at in [within, end - bytes.len()] {
                    let set = with_boot_bytes(DiscSet::full(Edc::Zero), at, bytes);
                    let given = boot_of(&set);
                    assert_eq!(given, said("textField", words), "{at} {character:?}");
                }
            }
        }
        // They are judged in the order the extractor records them: the product, the title, then
        // the executable.
        let late = with_boot_bytes(DiscSet::full(Edc::Zero), FORMAT_EXECUTABLE.start, &[0xE9]);
        let both = with_boot_bytes(late.clone(), FORMAT_TITLE.start + 20, &[0xE9]);
        assert_eq!(
            boot_of(&both),
            said("textField", "IP.BIN's title holds a byte outside ASCII")
        );
        let all = with_boot_bytes(both, FORMAT_PRODUCT.start, &[0xE9]);
        assert_eq!(
            boot_of(&all),
            said("textField", "IP.BIN's product holds a byte outside ASCII")
        );

        // A byte outside ASCII anywhere else in the boot area is kept as it is: in the area, the
        // version, between the fields and in the area's later sectors.
        let places = [
            FORMAT_AREA.start + 1,
            FORMAT_VERSION.start,
            60,
            300,
            FORMAT_SECTOR + 5,
            FORMAT_BOOT_SECTORS as usize * FORMAT_SECTOR - 1,
        ];
        for at in places {
            let set = with_boot_bytes(DiscSet::full(Edc::Zero), at, &[0xE9]);
            let boot = boot_of(&set).unwrap();
            assert_eq!(boot.bytes()[at], 0xE9, "{at}");
            assert_eq!(
                fields(&boot),
                ("T9710N", "GAUNTLET LEGENDS", "1ST_READ.BIN"),
                "{at}"
            );
        }
    }

    #[test]
    fn only_gauntlet_legends_boot_area_is_read() {
        let other = said(
            "notGauntletLegends",
            "Expected a Dreamcast Gauntlet Legends GD-ROM",
        );
        // Another console's hardware ID, a byte of it changed, another game's title, and a byte
        // outside ASCII in either, which the boot area is judged by before its fields are decoded.
        let cases: [(usize, &[u8]); 6] = [
            (0, b"SEGA SEGASATURN "),
            (5, b"X"),
            (15, b"\0"),
            (FORMAT_TITLE.start, b"GAUNTLET LEGEND\0"),
            (FORMAT_TITLE.start + 9, b"\xe9"),
            (3, b"\xff"),
        ];
        for (at, bytes) in cases {
            let set = with_boot_bytes(DiscSet::full(Edc::Zero), at, bytes);
            assert_eq!(boot_of(&set), other, "{at} {bytes:?}");
        }
        // A title that goes on past the game's name is still its.
        let longer = with_boot_bytes(
            DiscSet::full(Edc::Zero),
            FORMAT_TITLE.start + 16,
            b": THE DEMO",
        );
        assert_eq!(
            boot_of(&longer).unwrap().title(),
            "GAUNTLET LEGENDS: THE DEMO"
        );
        // Bytes too few to hold the title are no boot area of the game.
        let full = boot_of(&DiscSet::full(Edc::Zero)).unwrap();
        let start = &full.bytes()[..FORMAT_TITLE.end];
        assert!(names_gauntlet_legends(start));
        assert!(!names_gauntlet_legends(&start[..FORMAT_TITLE.end - 1]));
        assert!(!names_gauntlet_legends(FORMAT_HARDWARE_ID));
        assert!(!names_gauntlet_legends(&[]));
    }

    #[test]
    fn the_boot_areas_places_are_the_formats() {
        assert_eq!(
            (BOOT_LBA, BOOT_SECTORS),
            (FORMAT_BOOT_LBA, FORMAT_BOOT_SECTORS)
        );
        assert_eq!(BOOT_LEN, FORMAT_BOOT_SECTORS as usize * FORMAT_SECTOR);
        assert_eq!(
            (HARDWARE_ID, TITLE_PREFIX),
            (FORMAT_HARDWARE_ID, FORMAT_TITLE_PREFIX)
        );
        assert_eq!(
            (AREA, PRODUCT, VERSION),
            (FORMAT_AREA, FORMAT_PRODUCT, FORMAT_VERSION)
        );
        assert_eq!((EXECUTABLE, TITLE), (FORMAT_EXECUTABLE, FORMAT_TITLE));
    }
}
