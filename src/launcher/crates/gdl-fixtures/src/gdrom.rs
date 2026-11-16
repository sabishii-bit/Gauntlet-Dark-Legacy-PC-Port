//! GD-ROM sets for the tests of the launcher's Dreamcast reader and extraction, laid out as
//! tests/scripts/legends_fixtures.py lays out the Dreamcast extractor's: track files of raw
//! MODE1/2352 sectors whose headers give their own addresses, a boot area, an ISO 9660 volume and
//! its directories, the packed file table SIZES.ROM beside its DISK.ROM, and the cue sheet or GDI
//! that lists the tracks. A set built with [`Edc::Zero`] has the bytes that builder makes, which
//! the shared vectors pin; one built with [`Edc::Computed`] carries each sector's error detection
//! code as a real disc does, for the checks only an installation's extraction makes.

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::ops::{Range, RangeInclusive};
use std::path::{Path, PathBuf};

use crate::program::SparseFile;

/// The length of a raw sector as a track file keeps it: its sync, header, user data and codes.
pub const RAW_SECTOR: usize = 2352;

/// The length of a sector's user data, the blocks a disc's file system is made of.
pub const SECTOR: usize = 2048;

/// The twelve bytes a raw sector begins with.
const SYNC: [u8; 12] = [
    0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
];

/// Where a sector's header gives its address, after the sync: the minute, second and frame.
const ADDRESS_AT: usize = SYNC.len();

/// Where the header gives the sector's mode.
const MODE_AT: usize = ADDRESS_AT + 3;

/// The mode of a data sector: 2,048 bytes of user data, then their codes.
const MODE1: u8 = 1;

/// Where a raw sector keeps its user data, after its sync and header.
const USER_DATA: Range<usize> = 16..16 + SECTOR;

/// The bytes a raw sector's error detection code covers: the sync, the header and the user data.
const EDC_SPAN: Range<usize> = 0..USER_DATA.end;

/// Where a raw sector keeps that code, little-endian.
const EDC_FIELD: Range<usize> = USER_DATA.end..USER_DATA.end + 4;

/// The reflected polynomial of the error detection code of ECMA-130.
const EDC_POLYNOMIAL: u32 = 0xD801_8001;

/// The code's value for each byte, from which [`edc`] runs a byte at a time.
const EDC_TABLE: [u32; 256] = edc_table();

/// The frames an address counts before LBA 0: the two seconds of the lead-in.
const LBA_BIAS: u32 = 150;

/// The seconds of an address's minute.
const SECONDS_PER_MINUTE: u32 = 60;

/// The frames of an address's second, a sector each.
const FRAMES_PER_SECOND: u32 = 75;

/// Where the high-density area begins: its first data track and its boot area.
const HIGH_DENSITY_LBA: u32 = 45_000;

/// Where the primary volume descriptor lies, in sectors from the boot area's start.
const PVD_SECTOR: u32 = 16;

/// Where every set's root directory lies.
const ROOT_LBA: u32 = HIGH_DENSITY_LBA + 20;

/// Where a set's second data track starts, past the audio track between the two.
const SECOND_LBA: u32 = 45_100;

/// The sectors `track5.bin` keeps before that track's data: zeros without headers, as the
/// extractor tests' builder writes them. No reader reads them, since the sheet's INDEX 01 points
/// past them.
const PREGAP_SECTORS: u32 = 225;

/// The hardware ID a Dreamcast boot area begins with.
const HARDWARE_ID: &[u8; 16] = b"SEGA SEGAKATANA ";

/// Where the boot area's first sector keeps the field [`BootFields::area`] gives.
const AREA: Range<usize> = 48..56;

/// Where it keeps [`BootFields::product`].
const PRODUCT: Range<usize> = 64..74;

/// Where it keeps [`BootFields::version`].
const VERSION: Range<usize> = 74..80;

/// Where it keeps [`BootFields::executable`].
const EXECUTABLE: Range<usize> = 96..112;

/// Where it keeps [`BootFields::title`].
const TITLE: Range<usize> = 128..256;

/// The product every set's boot area names, padded with spaces.
const PRODUCT_ID: [u8; 10] = *b"T9710N    ";

/// The executable every set's boot area names, padded with spaces.
const EXECUTABLE_NAME: [u8; 16] = *b"1ST_READ.BIN    ";

/// What every set's boot area's title begins with.
const TITLE_TEXT: &[u8] = b"GAUNTLET LEGENDS";

/// The small set's boot area, and that of a disc of one file: no area, no version, and a title
/// followed by NULs.
const SMALL_BOOT: BootFields = BootFields {
    area: [0; 8],
    product: PRODUCT_ID,
    version: [0; 6],
    executable: EXECUTABLE_NAME,
    title: field(TITLE_TEXT, 0),
};

/// The full set's boot area: the US area, version 1.003, and a title padded with spaces.
const FULL_BOOT: BootFields = BootFields {
    area: *b" U      ",
    version: *b"V1.003",
    title: field(TITLE_TEXT, b' '),
    ..SMALL_BOOT
};

/// What a primary volume descriptor begins with: its type, the standard's identifier and version.
const STANDARD_ID: &[u8; 7] = b"\x01CD001\x01";

/// Where the descriptor names its volume, padded with spaces.
const VOLUME_ID: Range<usize> = 40..72;

/// Where it gives its logical block's size, both-endian.
const BLOCK_SIZE_AT: usize = 128;

/// The size of a logical block, and of every directory here: one sector.
const BLOCK_SIZE: u16 = SECTOR as u16;

/// Where the descriptor keeps the root directory's record.
const ROOT_RECORD_AT: usize = 156;

/// The flag of a directory's record.
const FLAG_DIRECTORY: u8 = 2;

/// The identifier a directory lists itself by.
const SELF_ID: &[u8] = b"\0";

/// The identifier a directory lists its parent by.
const PARENT_ID: &[u8] = b"\x01";

/// The length of a directory record before its identifier.
const RECORD_FIXED_LEN: usize = 33;

/// Where a record gives its extent's first LBA, both-endian.
const RECORD_LBA_AT: usize = 2;

/// Where a record gives its extent's size, both-endian.
const RECORD_SIZE_AT: usize = 10;

/// Where a record keeps its flags.
const RECORD_FLAGS_AT: usize = 25;

/// Where a record gives its volume's sequence number, both-endian.
const RECORD_SEQUENCE_AT: usize = 28;

/// Where a record gives its identifier's length.
const RECORD_NAME_LEN_AT: usize = 32;

/// The sequence number of a set's one volume.
const VOLUME_SEQUENCE: u16 = 1;

/// The first data track's file in the cue sets.
const TRACK3: &str = "track3.bin";

/// The second data track's file in the cue sets.
const TRACK5: &str = "track5.bin";

/// The sectors of the small set's first data track, and of a disc of one file without folders.
const SMALL_FIRST_SECTORS: u32 = 21;

/// The volume the small set and a disc of one file name.
const SMALL_VOLUME: &[u8] = b"TEST";

/// The identifier the small set lists its one file by.
const SMALL_FILE_NAME: &[u8] = b"TEST.WAD;1";

/// What the cue sets keep at 45100, the small set's file and the full set's DISK.ROM, and what a
/// disc of one file's file holds.
const HELLO: &[u8] = b"hello";

/// The small set's cue sheet.
const SMALL_CUE_NAME: &str = "test.cue";

/// What the small set's cue sheet says, with LF line ends on every system.
const SMALL_CUE: &str = concat!(
    "REM HIGH-DENSITY AREA\n",
    "FILE \"track3.bin\" BINARY\n",
    "TRACK 03 MODE1/2352\n",
    "INDEX 01 00:00:00\n",
    "FILE \"track5.bin\" BINARY\n",
    "TRACK 05 MODE1/2352\n",
    "INDEX 00 00:00:00\n",
    "INDEX 01 00:03:00\n",
);

/// The sectors of the full set's first data track: the boot area, the volume descriptor, the root
/// directory and `AUDIO/`.
const FULL_FIRST_SECTORS: u32 = 22;

/// The sectors of the full set's second data track: DISK.ROM, SIZES.ROM and the two of
/// `AUDIO/X.STR`.
const FULL_SECOND_SECTORS: u32 = 4;

/// The volume the full set names.
const FULL_VOLUME: &[u8] = b"GAUNT1";

/// Where the full set keeps its `AUDIO` folder, after the root directory.
const AUDIO_LBA: u32 = ROOT_LBA + 1;

/// Where the full set keeps DISK.ROM, which holds [`HELLO`].
const DISK_ROM_LBA: u32 = SECOND_LBA;

/// Where the full set keeps SIZES.ROM.
const SIZES_ROM_LBA: u32 = SECOND_LBA + 1;

/// Where the full set keeps `AUDIO/X.STR`, over two sectors.
const STREAM_LBA: u32 = SECOND_LBA + 2;

/// Where the full set's root lists EMPTY.BIN, a file of no bytes: an LBA no track holds.
const EMPTY_LBA: u32 = 999_999;

/// The rows of the full set's SIZES.ROM: `levels/a.wad`, DISK.ROM's five bytes; `levels/b.rom`,
/// empty, after them; and `ext.str`, seven bytes outside DISK.ROM.
const SIZES_ROWS: [(u32, i32, [i16; 4]); 3] = [
    (5, 0, [0, 7, -1, 0]),
    (0, 5, [0, 13, -1, 0]),
    (7, -1, [19, -1, 0, 0]),
];

/// The pool of names those rows' components point into.
const SIZES_POOL: &[u8] = b"levels\0a.wad\0b.rom\0ext.str\0";

/// The length of the full set's `AUDIO/X.STR`, the bytes 0 to 255 over and over.
const STREAM_LEN: usize = 3000;

/// The full set's cue sheet.
const FULL_CUE_NAME: &str = "full.cue";

/// What the full set's cue sheet says: a byte order mark, CRLF line ends, indents of two and four
/// spaces, and an audio track whose file the set leaves out.
const FULL_CUE: &str = concat!(
    "\u{feff}REM HIGH-DENSITY AREA\r\n",
    "FILE \"track3.bin\" BINARY\r\n",
    "  TRACK 03 MODE1/2352\r\n",
    "    INDEX 01 00:00:00\r\n",
    "FILE \"track4.bin\" BINARY\r\n",
    "  TRACK 04 AUDIO\r\n",
    "    INDEX 00 00:00:00\r\n",
    "    INDEX 01 00:02:00\r\n",
    "FILE \"track5.bin\" BINARY\r\n",
    "  TRACK 05 MODE1/2352\r\n",
    "    INDEX 00 00:00:00\r\n",
    "    INDEX 01 00:03:00\r\n",
);

/// The GDI set's sheet.
const GDI_NAME: &str = "full.gdi";

/// A GDI track's type for raw data.
const GDI_DATA: u32 = 4;

/// A GDI track's type for audio.
const GDI_AUDIO: u32 = 0;

/// The GDI set's tracks: number, first LBA, type and file, each data track's file holding its
/// sectors from its first LBA. The audio track's file is left out, as the full set leaves out its
/// own.
const GDI_TRACKS: [(u32, u32, u32, &str); 3] = [
    (1, HIGH_DENSITY_LBA, GDI_DATA, "track03.bin"),
    (2, 45_050, GDI_AUDIO, "track04.raw"),
    (3, SECOND_LBA, GDI_DATA, "track05.bin"),
];

/// Where a disc of one file keeps it, between the volume descriptor and the root directory.
const ONE_FILE_LBA: u32 = HIGH_DENSITY_LBA + 17;

/// A disc of one file's cue sheet.
const ONE_FILE_CUE_NAME: &str = "names.cue";

/// What a disc of one file's cue sheet says: its one data track, from its file's start.
const ONE_FILE_CUE: &str = "FILE \"track3.bin\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n";

/// The characters the Dreamcast extractor ends a sheet's lines at. A CR followed by an LF ends
/// one line: the empty line left between the two is passed over, as every empty line is.
const LINE_BREAKS: [char; 10] = [
    '\n', '\r', '\u{b}', '\u{c}', '\u{1c}', '\u{1d}', '\u{1e}', '\u{85}', '\u{2028}', '\u{2029}',
];

/// The ASCII separators the Dreamcast extractor strips from the ends of a sheet's line, besides
/// Unicode's white space.
const STRIPPED_SEPARATORS: RangeInclusive<char> = '\u{1c}'..='\u{1f}';

/// What a built sector keeps in its EDC field.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Edc {
    /// Zeros, as the extractor tests' builder leaves the field: the sectors the shared vectors pin,
    /// which only a reader that skips the code takes.
    Zero,
    /// The code of the sector's first 2,064 bytes, as a real disc keeps it.
    Computed,
}

impl Edc {
    /// Writes the EDC field of `sector`, a raw sector whose other bytes are written.
    fn fill(self, sector: &mut [u8]) {
        if self == Edc::Computed {
            let code = edc(&sector[EDC_SPAN]);
            sector[EDC_FIELD].copy_from_slice(&code.to_le_bytes());
        }
    }
}

/// The error detection code of ECMA-130 over `bytes`: the CRC of the reflected polynomial
/// 0xD8018001 from zero, with no final inversion, that a raw sector keeps after its user data.
pub fn edc(bytes: &[u8]) -> u32 {
    bytes.iter().fold(0, |code, &byte| {
        EDC_TABLE[usize::from(code.to_le_bytes()[0] ^ byte)] ^ (code >> 8)
    })
}

/// The code's value for each byte, worked out a bit at a time.
const fn edc_table() -> [u32; 256] {
    let mut table = [0; 256];
    let mut byte = 0;
    while byte < table.len() {
        let mut value = byte as u32;
        let mut bit = 0;
        while bit < u8::BITS {
            value = if value & 1 == 1 {
                (value >> 1) ^ EDC_POLYNOMIAL
            } else {
                value >> 1
            };
            bit += 1;
        }
        table[byte] = value;
        byte += 1;
    }
    table
}

/// The raw MODE1/2352 sector at `lba`: the sync, the header that gives `lba` as the BCD minute,
/// second and frame of `lba + 150` and mode 1, then `payload` from byte 16 and zeros after it, with
/// the EDC field as `edc` says. Panics when `payload` is longer than a sector's user data, or when
/// `lba` lies past 719,849, the last address a header's minute byte holds.
pub fn raw_sector(lba: u32, payload: &[u8], edc: Edc) -> Vec<u8> {
    assert!(
        payload.len() <= SECTOR,
        "a sector's user data holds at most {SECTOR} bytes, not {}",
        payload.len()
    );
    let mut sector = vec![0; RAW_SECTOR];
    sector[..SYNC.len()].copy_from_slice(&SYNC);
    sector[ADDRESS_AT..MODE_AT].copy_from_slice(&sector_address(lba));
    sector[MODE_AT] = MODE1;
    sector[USER_DATA.start..USER_DATA.start + payload.len()].copy_from_slice(payload);
    edc.fill(&mut sector);
    sector
}

/// The minute, second and frame a sector's header gives `lba` by, each in BCD. The minute's tens
/// may pass nine, up to 15, as the high-density area's addresses need.
fn sector_address(lba: u32) -> [u8; 3] {
    let address = lba
        .checked_add(LBA_BIAS)
        .unwrap_or_else(|| panic!("LBA {lba} has no address"));
    let frames_per_minute = SECONDS_PER_MINUTE * FRAMES_PER_SECOND;
    let bcd = |value: u32| {
        u8::try_from(((value / 10) << 4) | (value % 10)).unwrap_or_else(|_| {
            panic!("LBA {lba} lies past the last address a sector's header holds")
        })
    };
    [
        bcd(address / frames_per_minute),
        bcd(address % frames_per_minute / FRAMES_PER_SECOND),
        bcd(address % FRAMES_PER_SECOND),
    ]
}

/// `value` both-endian, as ISO 9660 keeps a 16-bit number: little-endian, then big-endian.
pub fn both16(value: u16) -> [u8; 4] {
    let [low, high] = value.to_le_bytes();
    [low, high, high, low]
}

/// `value` both-endian, as ISO 9660 keeps a 32-bit number: little-endian, then big-endian.
pub fn both32(value: u32) -> [u8; 8] {
    let mut both = [0; 8];
    both[..4].copy_from_slice(&value.to_le_bytes());
    both[4..].copy_from_slice(&value.to_be_bytes());
    both
}

/// The ISO 9660 directory record of `name` as the extractor tests' builder writes one: its length
/// (33 bytes, the identifier, and a pad byte when the identifier's length is even), the extent's
/// LBA and size both-endian, `flags`, volume sequence 1, the identifier's length and the
/// identifier; every other byte zero. Panics when the record is longer than its length byte holds.
pub fn record(name: &[u8], lba: u32, size: u32, flags: u8) -> Vec<u8> {
    let len = RECORD_FIXED_LEN + name.len() + usize::from(name.len().is_multiple_of(2));
    let len_byte = u8::try_from(len).unwrap_or_else(|_| {
        panic!(
            "the record of an identifier of {} bytes is {len} bytes long, more than its length \
             byte holds",
            name.len()
        )
    });
    let mut record = vec![0; len];
    record[0] = len_byte;
    record[RECORD_LBA_AT..RECORD_LBA_AT + 8].copy_from_slice(&both32(lba));
    record[RECORD_SIZE_AT..RECORD_SIZE_AT + 8].copy_from_slice(&both32(size));
    record[RECORD_FLAGS_AT] = flags;
    record[RECORD_SEQUENCE_AT..RECORD_SEQUENCE_AT + 4].copy_from_slice(&both16(VOLUME_SEQUENCE));
    record[RECORD_NAME_LEN_AT] = u8::try_from(name.len()).expect("shorter than its record");
    record[RECORD_FIXED_LEN..RECORD_FIXED_LEN + name.len()].copy_from_slice(name);
    record
}

/// The fields of a boot area's first sector a test may choose, each exactly the bytes the sector
/// keeps.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BootFields {
    /// The areas the disc is for, at 48, such as `" U      "`.
    pub area: [u8; 8],
    /// The product number, at 64, such as `"T9710N    "`.
    pub product: [u8; 10],
    /// The product's version, at 74, such as `"V1.003"`.
    pub version: [u8; 6],
    /// The executable the boot loads, at 96.
    pub executable: [u8; 16],
    /// The title, at 128.
    pub title: [u8; 128],
}

/// The first sector of a boot area, 2,048 bytes: the hardware ID `SEGA SEGAKATANA ` at 0, then
/// `fields` where a Dreamcast boot area keeps them, every other byte zero.
pub fn boot_sector(fields: &BootFields) -> Vec<u8> {
    let mut sector = vec![0; SECTOR];
    sector[..HARDWARE_ID.len()].copy_from_slice(HARDWARE_ID);
    sector[AREA].copy_from_slice(&fields.area);
    sector[PRODUCT].copy_from_slice(&fields.product);
    sector[VERSION].copy_from_slice(&fields.version);
    sector[EXECUTABLE].copy_from_slice(&fields.executable);
    sector[TITLE].copy_from_slice(&fields.title);
    sector
}

/// A primary volume descriptor of 2,048-byte blocks, 2,048 bytes: the standard's identifier,
/// `volume` padded with spaces to 32 bytes at 40, the block size both-endian at 128, and at 156
/// the record of the root directory, `root_size` bytes at `root_lba`; every other byte zero. Panics
/// when `volume` is longer than 32 bytes.
pub fn pvd_sector(volume: &[u8], root_lba: u32, root_size: u32) -> Vec<u8> {
    assert!(
        volume.len() <= VOLUME_ID.len(),
        "a volume's identifier holds at most {} bytes, not {}",
        VOLUME_ID.len(),
        volume.len()
    );
    let mut sector = vec![0; SECTOR];
    sector[..STANDARD_ID.len()].copy_from_slice(STANDARD_ID);
    let name = &mut sector[VOLUME_ID];
    name.fill(b' ');
    name[..volume.len()].copy_from_slice(volume);
    sector[BLOCK_SIZE_AT..BLOCK_SIZE_AT + 4].copy_from_slice(&both16(BLOCK_SIZE));
    let root = record(SELF_ID, root_lba, root_size, FLAG_DIRECTORY);
    sector[ROOT_RECORD_AT..ROOT_RECORD_AT + root.len()].copy_from_slice(&root);
    sector
}

/// A SIZES.ROM table: its count of rows, then each row's size, DISK.ROM offset (-1 for a file
/// outside it) and the pool offsets of its four components, the first negative one ending them,
/// all little-endian; then `pool`, the NUL-ended names. Panics when the count does not fit 32 bits.
pub fn sizes_rom(rows: &[(u32, i32, [i16; 4])], pool: &[u8]) -> Vec<u8> {
    let count = u32::try_from(rows.len()).expect("a table's count of rows fits 32 bits");
    let mut table = count.to_le_bytes().to_vec();
    for (size, offset, components) in rows {
        table.extend_from_slice(&size.to_le_bytes());
        table.extend_from_slice(&offset.to_le_bytes());
        for component in components {
            table.extend_from_slice(&component.to_le_bytes());
        }
    }
    table.extend_from_slice(pool);
    table
}

/// The files of a GD-ROM set and the one of them that lists the tracks. Every field may be changed
/// to make the set a test needs, a damaged one included.
#[derive(Clone, Debug)]
pub struct DiscSet {
    /// Each file by its name in the set's folder, the track files first, in the order the
    /// extractor tests' builder writes them, and the sheet last.
    pub files: Vec<(String, SparseFile)>,
    /// The name of the file of [`files`](Self::files) that lists the tracks: a cue sheet or a GDI.
    pub sheet: String,
}

impl DiscSet {
    /// The extractor tests' small set: `track3.bin`, 21 sectors from LBA 45000 holding the boot
    /// area, the volume descriptor of `TEST` and the root directory, which lists `TEST.WAD;1`,
    /// five bytes at 45100; `track5.bin`, 225 sectors of zeros, then the sector at 45100, which
    /// holds `hello`; and `test.cue`, which lists the two tracks with LF line ends.
    pub fn small(edc: Edc) -> DiscSet {
        DiscSet::small_with(SMALL_FILE_NAME, SECOND_LBA, 0, edc)
    }

    /// The small set with its root directory's third record changed: the file is listed by the
    /// identifier `name`, five bytes at `file_lba`, under `flags`. Its sectors stay where they
    /// are, so `hello` still lies at 45100.
    pub fn small_with(name: &[u8], file_lba: u32, flags: u8, edc: Edc) -> DiscSet {
        let len = u32::try_from(HELLO.len()).expect("five bytes");
        let root = directory(ROOT_LBA, ROOT_LBA, &record(name, file_lba, len, flags));
        let boot = boot_sector(&SMALL_BOOT);
        let pvd = pvd_sector(SMALL_VOLUME, ROOT_LBA, u32::from(BLOCK_SIZE));
        let first = raw_track(
            HIGH_DENSITY_LBA,
            SMALL_FIRST_SECTORS,
            &[
                (0, &boot),
                (PVD_SECTOR, &pvd),
                (ROOT_LBA - HIGH_DENSITY_LBA, &root),
            ],
            edc,
        );
        DiscSet {
            files: vec![
                (TRACK3.to_owned(), whole(first)),
                (
                    TRACK5.to_owned(),
                    after_pregap(raw_sector(SECOND_LBA, HELLO, edc), 1),
                ),
                (SMALL_CUE_NAME.to_owned(), whole(SMALL_CUE.into())),
            ],
            sheet: SMALL_CUE_NAME.to_owned(),
        }
    }

    /// The end-to-end set, behind a cue sheet: `track3.bin`, 22 sectors from LBA 45000 holding the
    /// boot area of product T9710N, version V1.003 and area U, the volume descriptor of `GAUNT1`,
    /// the root directory (`AUDIO/`, `DISK.ROM;1` at 45100, `EMPTY.BIN;1`, no bytes at 999999,
    /// and `SIZES.ROM;1` at 45101) and `AUDIO/`, which lists `X.STR;1`, 3,000 bytes at 45102;
    /// `track5.bin`, 225 sectors of zeros, then the four sectors from 45100 holding DISK.ROM
    /// (`hello`), SIZES.ROM (a table of `levels/a.wad`, `levels/b.rom` and the external
    /// `ext.str`) and `X.STR`; and `full.cue`, which lists the two and an audio track in
    /// `track4.bin`, which the set leaves out, after a byte order mark, with CRLF line ends and
    /// indents.
    pub fn full(edc: Edc) -> DiscSet {
        DiscSet::full_with_sizes_rom_len(edc, sizes_rom_len())
    }

    /// The full set's sectors behind a GDI, `full.gdi`, with CRLF line ends: `track03.bin` holds
    /// the 22 sectors from 45000 and `track05.bin` the four from 45100, without the pregap. The
    /// audio track it lists at 45050, `track04.raw`, is left out.
    pub fn full_gdi(edc: Edc) -> DiscSet {
        let mut data = full_tracks(sizes_rom_len(), edc).into_iter();
        let mut files = Vec::new();
        let mut lines = vec![GDI_TRACKS.len().to_string()];
        for (number, lba, kind, name) in GDI_TRACKS {
            if kind == GDI_DATA {
                let track = data.next().expect("a data track for each data line");
                files.push((name.to_owned(), whole(track)));
            }
            lines.push(format!("{number} {lba} {kind} {RAW_SECTOR} {name} 0"));
        }
        let text: String = lines.iter().map(|line| format!("{line}\r\n")).collect();
        files.push((GDI_NAME.to_owned(), whole(text.into_bytes())));
        DiscSet {
            files,
            sheet: GDI_NAME.to_owned(),
        }
    }

    /// The full set with SIZES.ROM's record in the root declaring `declared` bytes, and
    /// `track5.bin` lengthened with zeros, when the extent needs it, to end where the extent does,
    /// so that the extent lies on the disc and the table's size alone tells it apart. The file
    /// holds only the full set's sectors as chunks, so that its length takes no memory, and
    /// [`write`](Self::write) gives the rest by its length alone.
    pub fn full_with_sizes_rom_len(edc: Edc, declared: u32) -> DiscSet {
        let [first, second] = full_tracks(declared, edc);
        let extent = u64::from(declared).div_ceil(SECTOR as u64);
        let sectors =
            u64::from(FULL_SECOND_SECTORS).max(u64::from(SIZES_ROM_LBA - SECOND_LBA) + extent);
        DiscSet {
            files: vec![
                (TRACK3.to_owned(), whole(first)),
                (TRACK5.to_owned(), after_pregap(second, sectors)),
                (FULL_CUE_NAME.to_owned(), whole(FULL_CUE.into())),
            ],
            sheet: FULL_CUE_NAME.to_owned(),
        }
    }

    /// A disc of one file, as the shared name and path vectors were made on: `track3.bin`, the
    /// small set's boot area and volume descriptor, `hello` at 45017, and at 45020 the root
    /// directory, which lists the first of `folders` and each folder the next, a sector each, the
    /// last folder, or the root when there are none, listing the file as `identifier`; and
    /// `names.cue`, which lists the one track with LF line ends. The track is 21 sectors and a
    /// sector more for each folder.
    pub fn one_file(identifier: &[u8], folders: &[&[u8]], edc: Edc) -> DiscSet {
        let len = u32::try_from(HELLO.len()).expect("five bytes");
        let depth = u32::try_from(folders.len()).expect("a few folders");
        let directories: Vec<(u32, Vec<u8>)> = (0..=depth)
            .map(|level| {
                let lba = ROOT_LBA + level;
                let entry = match folders.get(level as usize) {
                    Some(folder) => record(folder, lba + 1, u32::from(BLOCK_SIZE), FLAG_DIRECTORY),
                    None => record(identifier, ONE_FILE_LBA, len, 0),
                };
                let parent = ROOT_LBA + level.saturating_sub(1);
                (lba - HIGH_DENSITY_LBA, directory(lba, parent, &entry))
            })
            .collect();
        let boot = boot_sector(&SMALL_BOOT);
        let pvd = pvd_sector(SMALL_VOLUME, ROOT_LBA, u32::from(BLOCK_SIZE));
        let mut payloads = vec![
            (0, boot.as_slice()),
            (PVD_SECTOR, pvd.as_slice()),
            (ONE_FILE_LBA - HIGH_DENSITY_LBA, HELLO),
        ];
        payloads.extend(
            directories
                .iter()
                .map(|(at, sector)| (*at, sector.as_slice())),
        );
        let track = raw_track(
            HIGH_DENSITY_LBA,
            SMALL_FIRST_SECTORS + depth,
            &payloads,
            edc,
        );
        DiscSet {
            files: vec![
                (TRACK3.to_owned(), whole(track)),
                (ONE_FILE_CUE_NAME.to_owned(), whole(ONE_FILE_CUE.into())),
            ],
            sheet: ONE_FILE_CUE_NAME.to_owned(),
        }
    }

    /// The set with a file for each audio track its sheet lists that the set does not hold yet,
    /// one sector of zeros each, put before the sheet in the order the sheet lists them: the files
    /// a cue sheet names before each `TRACK nn AUDIO`, and those a GDI lists with type 0, on the
    /// lines the Dreamcast extractor reads the sheet as.
    pub fn with_audio_files(mut self) -> DiscSet {
        let at = self
            .files
            .iter()
            .position(|(name, _)| *name == self.sheet)
            .unwrap_or_else(|| panic!("the set holds no sheet {:?}", self.sheet));
        let mut place = at;
        for name in audio_files(&contents(&self.files[at].1)) {
            if self.files.iter().all(|(held, _)| *held != name) {
                let silence = SparseFile {
                    len: RAW_SECTOR as u64,
                    chunks: Vec::new(),
                    at: 0,
                };
                self.files.insert(place, (name, silence));
                place += 1;
            }
        }
        self
    }

    /// Writes every file of the set into `folder`, each made new, given its length and then its
    /// chunks in their order, and gives the sheet's path. A chunk's bytes past its file's length
    /// are not written. It stops at the first file that cannot be made, one that exists already
    /// included, keeping the files written before it.
    pub fn write(&self, folder: &Path) -> io::Result<PathBuf> {
        for (name, file) in &self.files {
            let mut written = File::create_new(folder.join(name))?;
            written.set_len(file.len)?;
            for (offset, bytes) in &file.chunks {
                let Some(room) = file.len.checked_sub(*offset).filter(|&room| room > 0) else {
                    continue;
                };
                let kept = bytes.len().min(usize::try_from(room).unwrap_or(usize::MAX));
                written.seek(SeekFrom::Start(*offset))?;
                written.write_all(&bytes[..kept])?;
            }
        }
        Ok(folder.join(&self.sheet))
    }
}

/// `text` at the start of a field of `N` bytes, the rest `fill`.
const fn field<const N: usize>(text: &[u8], fill: u8) -> [u8; N] {
    let mut bytes = [fill; N];
    let mut at = 0;
    while at < text.len() {
        bytes[at] = text[at];
        at += 1;
    }
    bytes
}

/// The length of the full set's SIZES.ROM, which its root's record gives.
fn sizes_rom_len() -> u32 {
    u32::try_from(sizes_rom(&SIZES_ROWS, SIZES_POOL).len()).expect("a table of three rows")
}

/// The full set's two data tracks, the second without its pregap, with SIZES.ROM's record in the
/// root declaring `sizes_rom_len` bytes.
fn full_tracks(sizes_rom_len: u32, edc: Edc) -> [Vec<u8>; 2] {
    let len = |bytes: &[u8]| u32::try_from(bytes.len()).expect("a file of a few sectors");
    let stream: Vec<u8> = (0..STREAM_LEN).map(|at| at.to_le_bytes()[0]).collect();
    let entries = [
        record(b"AUDIO", AUDIO_LBA, u32::from(BLOCK_SIZE), FLAG_DIRECTORY),
        record(b"DISK.ROM;1", DISK_ROM_LBA, len(HELLO), 0),
        record(b"EMPTY.BIN;1", EMPTY_LBA, 0, 0),
        record(b"SIZES.ROM;1", SIZES_ROM_LBA, sizes_rom_len, 0),
    ]
    .concat();
    let root = directory(ROOT_LBA, ROOT_LBA, &entries);
    let audio = directory(
        AUDIO_LBA,
        ROOT_LBA,
        &record(b"X.STR;1", STREAM_LBA, len(&stream), 0),
    );
    let boot = boot_sector(&FULL_BOOT);
    let pvd = pvd_sector(FULL_VOLUME, ROOT_LBA, u32::from(BLOCK_SIZE));
    let first = raw_track(
        HIGH_DENSITY_LBA,
        FULL_FIRST_SECTORS,
        &[
            (0, &boot),
            (PVD_SECTOR, &pvd),
            (ROOT_LBA - HIGH_DENSITY_LBA, &root),
            (AUDIO_LBA - HIGH_DENSITY_LBA, &audio),
        ],
        edc,
    );
    let sizes = sizes_rom(&SIZES_ROWS, SIZES_POOL);
    let (stream_first, stream_second) = stream.split_at(SECTOR);
    let second = raw_track(
        SECOND_LBA,
        FULL_SECOND_SECTORS,
        &[
            (DISK_ROM_LBA - SECOND_LBA, HELLO),
            (SIZES_ROM_LBA - SECOND_LBA, &sizes),
            (STREAM_LBA - SECOND_LBA, stream_first),
            (STREAM_LBA + 1 - SECOND_LBA, stream_second),
        ],
        edc,
    );
    [first, second]
}

/// A one-sector directory at `lba`: its own record, its parent's at `parent`, then `entries`.
fn directory(lba: u32, parent: u32, entries: &[u8]) -> Vec<u8> {
    let size = u32::from(BLOCK_SIZE);
    [
        record(SELF_ID, lba, size, FLAG_DIRECTORY),
        record(PARENT_ID, parent, size, FLAG_DIRECTORY),
        entries.to_vec(),
    ]
    .concat()
}

/// `count` raw sectors from `first_lba`, each holding what `payloads` gives for its place in the
/// track, counted from nought, or zeros. Panics when a payload's place lies past the track.
fn raw_track(first_lba: u32, count: u32, payloads: &[(u32, &[u8])], edc: Edc) -> Vec<u8> {
    for (place, _) in payloads {
        assert!(
            *place < count,
            "sector {place} lies past a track of {count}"
        );
    }
    (0..count)
        .flat_map(|place| {
            let payload = payloads
                .iter()
                .find(|(held, _)| *held == place)
                .map_or(&[][..], |(_, payload)| *payload);
            raw_sector(first_lba + place, payload, edc)
        })
        .collect()
}

/// A file that holds `bytes` and nothing else.
fn whole(bytes: Vec<u8>) -> SparseFile {
    SparseFile {
        len: bytes.len() as u64,
        chunks: vec![(0, bytes)],
        at: 0,
    }
}

/// A second data track's file: the pregap's zeros, then `sectors` sectors that begin with the
/// raw sectors `data`, zeros after them.
fn after_pregap(data: Vec<u8>, sectors: u64) -> SparseFile {
    let raw = RAW_SECTOR as u64;
    let pregap = u64::from(PREGAP_SECTORS) * raw;
    SparseFile {
        len: pregap + sectors * raw,
        chunks: vec![(pregap, data)],
        at: 0,
    }
}

/// What `file` holds, read whole.
fn contents(file: &SparseFile) -> Vec<u8> {
    let mut file = SparseFile {
        at: 0,
        ..file.clone()
    };
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)
        .expect("a file held in memory reads whole");
    bytes
}

/// The files `sheet` lists for its audio tracks, in its order: a GDI's of type 0, or a cue sheet's
/// named before each `TRACK nn AUDIO`. Its lines end and are stripped where the Dreamcast
/// extractor ends and strips them, and a sheet whose first line holds only digits is a GDI, as
/// the launcher's reader tells them apart.
fn audio_files(sheet: &[u8]) -> Vec<String> {
    let text = String::from_utf8_lossy(sheet);
    let text = text.strip_prefix('\u{feff}').unwrap_or(&text);
    let mut lines = text
        .split(LINE_BREAKS)
        .map(|line| line.trim_matches(stripped))
        .filter(|line| !line.is_empty());
    let Some(first) = lines.next() else {
        return Vec::new();
    };
    let mut files: Vec<String> = Vec::new();
    if first.bytes().all(|byte| byte.is_ascii_digit()) {
        // After the count, each track's number, LBA, type, sector size, file and offset.
        for line in lines {
            let fields = gdi_fields(line);
            if fields.get(2).and_then(|kind| kind.parse().ok()) == Some(GDI_AUDIO) {
                files.extend(fields.get(4).map(|name| (*name).to_owned()));
            }
        }
    } else {
        let mut file = None;
        for line in [first].into_iter().chain(lines) {
            if let Some(rest) = line.strip_prefix("FILE \"") {
                file = rest.rsplit_once('"').map(|(name, _)| name);
            } else if line.starts_with("TRACK ") && line.ends_with(" AUDIO") {
                files.extend(file.map(str::to_owned));
            }
        }
    }
    files
}

/// Whether the Dreamcast extractor strips `character` from the ends of a sheet's line.
fn stripped(character: char) -> bool {
    character.is_whitespace() || STRIPPED_SEPARATORS.contains(&character)
}

/// The fields of a GDI's line: runs of characters between spaces or tabs, a field that begins
/// with a quote running to the next quote, without them.
fn gdi_fields(line: &str) -> Vec<&str> {
    let mut fields = Vec::new();
    let mut rest = line.trim_start_matches([' ', '\t']);
    while !rest.is_empty() {
        let (field, after) = match rest.strip_prefix('"') {
            Some(quoted) => quoted.split_once('"').unwrap_or((quoted, "")),
            None => rest.split_once([' ', '\t']).unwrap_or((rest, "")),
        };
        fields.push(field);
        rest = after.trim_start_matches([' ', '\t']);
    }
    fields
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs::{self, File};
    use std::io::{ErrorKind, Read};
    use std::panic::{self, UnwindSafe};

    use serde_json::Value;

    use super::{
        BootFields, DiscSet, EDC_FIELD, EDC_SPAN, Edc, FULL_BOOT, RAW_SECTOR, RECORD_FIXED_LEN,
        RECORD_SIZE_AT, SECTOR, SIZES_POOL, SIZES_ROWS, SMALL_BOOT, SYNC, USER_DATA, audio_files,
        boot_sector, both16, both32, contents, edc, pvd_sector, raw_sector, raw_track, record,
        sizes_rom, whole,
    };
    use crate::program::SparseFile;
    use crate::runtime::{hex, sha256_hex};

    /// The Dreamcast cases the extractor's tests and the launcher's read alike.
    const VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/legends/dreamcast-vectors.json"
    ));

    /// The sectors of pregap the cue sets' second track keeps before its data.
    const PREGAP: usize = 225;

    /// The length of a directory record of a one-byte identifier: a directory's own and its
    /// parent's.
    const DOT_RECORD_LEN: usize = 34;

    /// A builder of a set, given what its sectors keep in their EDC fields.
    type Build = fn(Edc) -> DiscSet;

    fn vectors() -> Value {
        serde_json::from_str(VECTORS).expect("the Dreamcast vectors are JSON")
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

    /// A vector's number that a builder takes as an LBA or a size.
    fn number(value: &Value) -> u32 {
        u32::try_from(value.as_u64().expect("a number")).expect("a number of 32 bits")
    }

    /// The names of the files of `set`, in its order.
    fn names(set: &DiscSet) -> Vec<String> {
        set.files.iter().map(|(name, _)| name.clone()).collect()
    }

    /// The file of `set` named `name`.
    fn file<'a>(set: &'a DiscSet, name: &str) -> &'a SparseFile {
        set.files
            .iter()
            .find(|(held, _)| held == name)
            .map(|(_, file)| file)
            .unwrap_or_else(|| panic!("the set holds no {name}"))
    }

    /// What the file of `set` named `name` holds.
    fn bytes_of(set: &DiscSet, name: &str) -> Vec<u8> {
        contents(file(set, name))
    }

    /// Every file of `set` by its name, with what it holds.
    fn files_of(set: &DiscSet) -> BTreeMap<String, Vec<u8>> {
        set.files
            .iter()
            .map(|(name, file)| (name.clone(), contents(file)))
            .collect()
    }

    /// The user data of `count` sectors of `track`, from its sector `first` on.
    fn user_data(track: &[u8], first: usize, count: usize) -> Vec<u8> {
        track
            .as_chunks::<RAW_SECTOR>()
            .0
            .iter()
            .skip(first)
            .take(count)
            .flat_map(|sector| &sector[USER_DATA])
            .copied()
            .collect()
    }

    /// Where `needle` first occurs in `bytes`.
    fn find(bytes: &[u8], needle: &[u8]) -> usize {
        bytes
            .windows(needle.len())
            .position(|window| window == needle)
            .unwrap_or_else(|| panic!("no {needle:?}"))
    }

    /// Whether every one of `bytes` is zero.
    fn zeros(bytes: &[u8]) -> bool {
        bytes.iter().all(|&byte| byte == 0)
    }

    /// Whether `build` panics.
    fn panics<T>(build: impl FnOnce() -> T + UnwindSafe) -> bool {
        panic::catch_unwind(build).is_err()
    }

    #[test]
    fn the_edc_check_value_is_ecma_130s() {
        let vectors = vectors();
        let cases = &vectors["edc"];
        let check = &cases["check"];
        assert_eq!(edc(b"123456789"), 0x6EC2_EDC4);
        let text = check["ascii"].as_str().unwrap();
        assert_eq!(
            format!("{:08x}", edc(text.as_bytes())),
            check["edc"].as_str().unwrap()
        );
        // Zeros keep the code at nought, so a run of them carries a valid code of zeros.
        assert_eq!(edc(&[]), 0);
        assert_eq!(edc(&[0; RAW_SECTOR]), 0);

        let sectors = cases["sectors"].as_array().unwrap();
        for case in sectors {
            let lba = number(&case["lba"]);
            let name = case["payload"].as_str().unwrap();
            let payload = unhex(&cases["payloads"][name]);
            let code = u32::from_str_radix(case["edc"].as_str().unwrap(), 16).unwrap();
            let zero = raw_sector(lba, &payload, Edc::Zero);
            assert_eq!(edc(&zero[EDC_SPAN]), code, "{lba} {name}");
            assert_eq!(zero[EDC_FIELD], [0; 4], "{lba} {name}");
            let computed = raw_sector(lba, &payload, Edc::Computed);
            assert_eq!(computed[EDC_FIELD], code.to_le_bytes(), "{lba} {name}");
        }
        assert_eq!(sectors.len(), 8);
    }

    #[test]
    fn the_sets_are_the_python_builders_bytes() {
        let vectors = vectors();
        let sets = &vectors["sets"];
        let built = [
            ("small", DiscSet::small(Edc::Zero)),
            ("full", DiscSet::full(Edc::Zero)),
            ("fullGdi", DiscSet::full_gdi(Edc::Zero)),
        ];
        for (name, set) in &built {
            let expected = &sets[name];
            assert_eq!(set.sheet, expected["sheet"].as_str().unwrap(), "{name}");
            let listed: BTreeMap<String, (usize, String)> = expected["files"]
                .as_object()
                .unwrap()
                .iter()
                .map(|(file, digest)| {
                    let size = digest["size"].as_u64().unwrap() as usize;
                    let sha256 = digest["sha256"].as_str().unwrap().to_owned();
                    (file.clone(), (size, sha256))
                })
                .collect();
            let made: BTreeMap<String, (usize, String)> = files_of(set)
                .into_iter()
                .map(|(file, bytes)| (file, (bytes.len(), sha256_hex(&bytes))))
                .collect();
            assert_eq!(made, listed, "{name}");
            // Each file once, the sheet written last.
            assert_eq!(set.files.len(), listed.len(), "{name}");
            assert_eq!(set.files.last().unwrap().0, set.sheet, "{name}");
        }
        assert_eq!(sets.as_object().unwrap().len(), built.len());

        // Each of the GDI's data tracks holds its sectors from the LBA the GDI gives it.
        let gdi = &built[2].1;
        let tracks = sets["fullGdi"]["dataTracks"].as_array().unwrap();
        for track in tracks {
            let name = track[0].as_str().unwrap();
            let bytes = bytes_of(gdi, name);
            assert_eq!(track[1], 0, "{name}");
            assert_eq!(
                bytes.len(),
                track[3].as_u64().unwrap() as usize * RAW_SECTOR
            );
            let header = raw_sector(number(&track[2]), &[], Edc::Zero);
            assert_eq!(
                bytes[..USER_DATA.start],
                header[..USER_DATA.start],
                "{name}"
            );
        }
        assert_eq!(tracks.len(), 2);
        // They hold the full set's sectors, the second without its pregap.
        let full = &built[1].1;
        assert_eq!(bytes_of(gdi, "track03.bin"), bytes_of(full, "track3.bin"));
        assert_eq!(
            bytes_of(gdi, "track05.bin"),
            bytes_of(full, "track5.bin")[PREGAP * RAW_SECTOR..]
        );

        let records = vectors["records"].as_array().unwrap();
        for case in records {
            let name = case["name"].as_str().unwrap();
            let flags = u8::try_from(case["flags"].as_u64().unwrap()).unwrap();
            let made = record(
                name.as_bytes(),
                number(&case["lba"]),
                number(&case["size"]),
                flags,
            );
            assert_eq!(hex(&made), case["hex"].as_str().unwrap(), "{name:?}");
        }
        assert_eq!(records.len(), 3);
    }

    #[test]
    fn the_full_set_holds_the_files_of_its_tree() {
        let vectors = vectors();
        let tree = vectors["sets"]["full"]["tree"].as_array().unwrap();
        let set = DiscSet::full(Edc::Zero);
        let first = bytes_of(&set, "track3.bin");
        let second = bytes_of(&set, "track5.bin");
        // The boot area's 16 sectors from 45000, and each disc file's sectors from 45100, past the
        // second track's pregap.
        let places: [(&str, &[u8], usize, usize); 5] = [
            ("IP.BIN", &first, 0, 16),
            ("disc/DISK.ROM", &second, PREGAP, 1),
            ("disc/SIZES.ROM", &second, PREGAP + 1, 1),
            ("disc/AUDIO/X.STR", &second, PREGAP + 2, 2),
            ("disc/EMPTY.BIN", &second, 0, 0),
        ];
        for (path, track, at, count) in places {
            let row = tree
                .iter()
                .find(|row| row[0] == path)
                .unwrap_or_else(|| panic!("{path} is not in the full set's tree"));
            let size = row[1].as_u64().unwrap() as usize;
            let data = user_data(track, at, count);
            assert!(size <= data.len(), "{path}");
            assert_eq!(
                sha256_hex(&data[..size]),
                row[2].as_str().unwrap(),
                "{path}"
            );
            assert!(zeros(&data[size..]), "{path}");
        }
        let sizes = tree.iter().find(|row| row[0] == "disc/SIZES.ROM").unwrap();
        let table = sizes_rom(&SIZES_ROWS, SIZES_POOL);
        assert_eq!(table.len() as u64, sizes[1].as_u64().unwrap());
        assert_eq!(sha256_hex(&table), sizes[2].as_str().unwrap());
    }

    #[test]
    fn computed_edcs_hold_and_payloads_are_unchanged() {
        let builds: [(&str, Build); 6] = [
            ("the small set", DiscSet::small),
            ("another record", |edc| {
                DiscSet::small_with(b"OTHER.BIN;1", 45_050, 0x80, edc)
            }),
            ("the full set", DiscSet::full),
            ("the full set's audio", |edc| {
                DiscSet::full(edc).with_audio_files()
            }),
            ("the GDI set", DiscSet::full_gdi),
            ("one file in two folders", |edc| {
                DiscSet::one_file(b"X;1", &[b"A", b"B"], edc)
            }),
        ];
        for (case, build) in builds {
            let (zero, computed) = (build(Edc::Zero), build(Edc::Computed));
            assert_eq!(zero.sheet, computed.sheet, "{case}");
            assert_eq!(names(&zero), names(&computed), "{case}");
            let mut coded = 0;
            for ((name, zero_file), (_, computed_file)) in zero.files.iter().zip(&computed.files) {
                let (zero_bytes, computed_bytes) = (contents(zero_file), contents(computed_file));
                if *name == zero.sheet {
                    assert_eq!(zero_bytes, computed_bytes, "{case}");
                    continue;
                }
                assert_eq!(zero_bytes.len(), computed_bytes.len(), "{case} {name}");
                let (zero_sectors, rest) = zero_bytes.as_chunks::<RAW_SECTOR>();
                assert!(rest.is_empty(), "{case} {name}");
                let (computed_sectors, _) = computed_bytes.as_chunks::<RAW_SECTOR>();
                let sectors = zero_sectors.iter().zip(computed_sectors);
                for (at, (zero_sector, computed_sector)) in sectors.enumerate() {
                    let place = format!("{case}: sector {at} of {name}");
                    if zero_sector[..SYNC.len()] == SYNC {
                        // The same sector but for its code, which holds over the rest of it.
                        assert_eq!(zero_sector[EDC_SPAN], computed_sector[EDC_SPAN], "{place}");
                        assert_eq!(zero_sector[EDC_FIELD], [0; 4], "{place}");
                        let code = edc(&computed_sector[EDC_SPAN]);
                        assert_ne!(code, 0, "{place}");
                        assert_eq!(computed_sector[EDC_FIELD], code.to_le_bytes(), "{place}");
                        let after = EDC_FIELD.end;
                        assert!(zeros(&zero_sector[after..]), "{place}");
                        assert!(zeros(&computed_sector[after..]), "{place}");
                        coded += 1;
                    } else {
                        // The pregap and the audio files: zeros, with no header to code.
                        assert!(zeros(zero_sector), "{place}");
                        assert!(zeros(computed_sector), "{place}");
                    }
                }
            }
            assert!(coded > 0, "{case}");
        }
    }

    #[test]
    fn a_lengthened_track_holds_only_its_sectors() {
        let full = DiscSet::full(Edc::Zero);
        let table_len = u32::try_from(sizes_rom(&SIZES_ROWS, SIZES_POOL).len()).unwrap();
        // At the table's own size, the record lengthens nothing.
        assert_eq!(
            files_of(&DiscSet::full_with_sizes_rom_len(Edc::Zero, table_len)),
            files_of(&full)
        );

        let declared = (16 << 20) + 1;
        let set = DiscSet::full_with_sizes_rom_len(Edc::Zero, declared);
        assert_eq!(set.sheet, full.sheet);
        assert_eq!(names(&set), names(&full));
        assert_eq!(bytes_of(&set, "full.cue"), bytes_of(&full, "full.cue"));
        // The root's record of SIZES.ROM declares the size, and nothing else of the first track
        // changes.
        let (first, full_first) = (bytes_of(&set, "track3.bin"), bytes_of(&full, "track3.bin"));
        let root = 20 * RAW_SECTOR + USER_DATA.start;
        let name_at = root + find(&full_first[root..root + SECTOR], b"SIZES.ROM;1");
        let size_at = name_at - RECORD_FIXED_LEN + RECORD_SIZE_AT;
        assert_eq!(full_first[size_at..size_at + 8], both32(table_len));
        assert_eq!(first[size_at..size_at + 8], both32(declared));
        let mut restored = first.clone();
        restored[size_at..size_at + 8].copy_from_slice(&both32(table_len));
        assert_eq!(restored, full_first);

        // The second track keeps the full set's sectors and ends where SIZES.ROM's extent does:
        // past the pregap, DISK.ROM's sector at 45100, then the extent's from 45101.
        let (track, full_track) = (file(&set, "track5.bin"), file(&full, "track5.bin"));
        assert_eq!(track.chunks, full_track.chunks);
        let extent = u64::from(declared).div_ceil(SECTOR as u64);
        assert_eq!(extent, 8193);
        assert_eq!(track.len, (PREGAP as u64 + 1 + extent) * RAW_SECTOR as u64);

        let folder = tempfile::tempdir().unwrap();
        let sheet = set.write(folder.path()).unwrap();
        assert_eq!(sheet, folder.path().join("full.cue"));
        for name in ["track3.bin", "full.cue"] {
            let written = fs::read(folder.path().join(name)).unwrap();
            assert_eq!(written, bytes_of(&set, name), "{name}");
        }
        let mut written = File::open(folder.path().join("track5.bin")).unwrap();
        assert_eq!(written.metadata().unwrap().len(), track.len);
        let held = contents(full_track);
        let mut head = vec![0; held.len()];
        written.read_exact(&mut head).unwrap();
        assert_eq!(head, held);
        let mut block = vec![0; 1 << 16];
        let mut rest = 0;
        loop {
            let read = written.read(&mut block).unwrap();
            if read == 0 {
                break;
            }
            assert!(zeros(&block[..read]), "a byte past the written sectors");
            rest += read as u64;
        }
        assert_eq!(rest, track.len - held.len() as u64);
    }

    #[test]
    fn sector_headers_give_the_addresses_the_vectors_read() {
        let vectors = vectors();
        let mut built = 0;
        for case in vectors["sectors"].as_array().unwrap() {
            // A header the readers take, at an LBA a builder can give: none before LBA 0.
            let Some(lba) = case["lba"].as_u64() else {
                continue;
            };
            let header = unhex(&case["header"]);
            let sector = raw_sector(u32::try_from(lba).unwrap(), &[], Edc::Zero);
            assert_eq!(sector[..USER_DATA.start], header[..], "{lba}");
            built += 1;
        }
        // 45000, 547163, 0, and the minutes with tens past nine, A0 and F9.
        assert_eq!(built, 5);

        // The payload from byte 16, and zeros after it.
        let payload: Vec<u8> = (0..SECTOR).map(|at| at.to_le_bytes()[0] | 1).collect();
        let sector = raw_sector(45_000, &payload, Edc::Zero);
        assert_eq!(sector.len(), RAW_SECTOR);
        assert_eq!(
            sector[..16],
            *b"\0\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\0\x10\x02\x00\x01"
        );
        assert_eq!(sector[USER_DATA], payload[..]);
        assert!(zeros(&sector[USER_DATA.end..]));
        let short = raw_sector(45_000, b"hi", Edc::Zero);
        assert_eq!(short[16..18], *b"hi");
        assert!(zeros(&short[18..]));

        assert_eq!(both16(0x0102), [2, 1, 1, 2]);
        assert_eq!(both32(0x0102_0304), [4, 3, 2, 1, 1, 2, 3, 4]);
    }

    #[test]
    fn boot_and_volume_sectors_put_their_fields_in_place() {
        let fields = BootFields {
            area: *b"JUE     ",
            product: *b"0123456789",
            version: *b"V9.876",
            executable: *b"0123456789abcdef",
            title: [b'T'; 128],
        };
        let mut expected = vec![0; SECTOR];
        expected[..16].copy_from_slice(b"SEGA SEGAKATANA ");
        expected[48..56].copy_from_slice(&fields.area);
        expected[64..74].copy_from_slice(&fields.product);
        expected[74..80].copy_from_slice(&fields.version);
        expected[96..112].copy_from_slice(&fields.executable);
        expected[128..256].copy_from_slice(&fields.title);
        assert_eq!(boot_sector(&fields), expected);
        // The sets' own: the small set's title followed by NULs, the full set's by spaces.
        assert_eq!(SMALL_BOOT.area, [0; 8]);
        assert_eq!(SMALL_BOOT.version, [0; 6]);
        assert_eq!(SMALL_BOOT.title[..16], *b"GAUNTLET LEGENDS");
        assert!(zeros(&SMALL_BOOT.title[16..]));
        assert_eq!(
            (FULL_BOOT.area, FULL_BOOT.version),
            (*b" U      ", *b"V1.003")
        );
        assert_eq!(FULL_BOOT.title[..16], *b"GAUNTLET LEGENDS");
        assert!(FULL_BOOT.title[16..].iter().all(|&byte| byte == b' '));
        for boot in [SMALL_BOOT, FULL_BOOT] {
            assert_eq!(
                (boot.product, boot.executable),
                (*b"T9710N    ", *b"1ST_READ.BIN    ")
            );
        }

        let mut expected = vec![0; SECTOR];
        expected[..7].copy_from_slice(b"\x01CD001\x01");
        expected[40..72].copy_from_slice(b"VOL                             ");
        expected[128..132].copy_from_slice(&[0, 8, 8, 0]);
        let root = record(b"\0", 45_020, 4096, 2);
        expected[156..156 + root.len()].copy_from_slice(&root);
        assert_eq!(root.len(), 34);
        assert_eq!(pvd_sector(b"VOL", 45_020, 4096), expected);
        let filled = pvd_sector(&[b'V'; 32], 45_020, 2048);
        assert!(filled[40..72].iter().all(|&byte| byte == b'V'));
    }

    #[test]
    fn small_with_changes_only_its_files_record() {
        let small = DiscSet::small(Edc::Zero);
        assert_eq!(
            files_of(&DiscSet::small_with(b"TEST.WAD;1", 45_100, 0, Edc::Zero)),
            files_of(&small)
        );
        let other = DiscSet::small_with(b"../escape;1", 45_050, 0x80, Edc::Zero);
        assert_eq!(other.sheet, small.sheet);
        assert_eq!(names(&other), names(&small));
        for name in ["track5.bin", "test.cue"] {
            assert_eq!(bytes_of(&other, name), bytes_of(&small, name), "{name}");
        }
        // Only the root's sector differs, from its third record on: the root's own record and its
        // parent's come first.
        let (first, small_first) = (
            bytes_of(&other, "track3.bin"),
            bytes_of(&small, "track3.bin"),
        );
        assert_eq!(first.len(), small_first.len());
        let root = 20 * RAW_SECTOR;
        let third = root + USER_DATA.start + 2 * DOT_RECORD_LEN;
        assert_eq!(first[..third], small_first[..third]);
        assert_eq!(first[root + RAW_SECTOR..], small_first[root + RAW_SECTOR..]);
        let listed = record(b"../escape;1", 45_050, 5, 0x80);
        assert_eq!(first[third..third + listed.len()], listed[..]);
        assert!(zeros(&first[third + listed.len()..root + RAW_SECTOR]));
        // The file's sector still holds what the small set's does.
        let second = bytes_of(&other, "track5.bin");
        assert_eq!(user_data(&second, PREGAP, 1)[..5], *b"hello");
    }

    #[test]
    fn a_one_file_disc_lists_its_file_where_the_name_vectors_put_it() {
        let small_first = bytes_of(&DiscSet::small(Edc::Zero), "track3.bin");
        // The longest path the vectors walk: three folders, then the file.
        let vectors = vectors();
        let path = vectors["isoPaths"][0]["path"].as_str().unwrap();
        assert_eq!(path.len(), 220);
        let components: Vec<&str> = path.split('/').collect();
        let (file_name, folders) = components.split_last().unwrap();
        assert_eq!(folders.len(), 3);
        let identifier = format!("{file_name};1");
        let folders: Vec<&[u8]> = folders.iter().map(|folder| folder.as_bytes()).collect();
        for folders in [&folders[..0], &folders[..]] {
            let set = DiscSet::one_file(identifier.as_bytes(), folders, Edc::Zero);
            assert_eq!(set.sheet, "names.cue");
            assert_eq!(names(&set), ["track3.bin", "names.cue"]);
            assert_eq!(
                bytes_of(&set, "names.cue"),
                b"FILE \"track3.bin\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n"
            );
            let track = bytes_of(&set, "track3.bin");
            let depth = folders.len();
            assert_eq!(track.len(), (21 + depth) * RAW_SECTOR, "{depth}");
            // The small set's boot area and volume descriptor, and the file at 45017.
            let at = |sector: usize| sector * RAW_SECTOR;
            assert_eq!(track[..at(17)], small_first[..at(17)], "{depth}");
            let hello = raw_sector(45_017, b"hello", Edc::Zero);
            assert_eq!(track[at(17)..at(18)], hello[..], "{depth}");
            assert_eq!(
                track[at(18)..at(20)],
                small_first[at(18)..at(20)],
                "{depth}"
            );
            // From the root at 45020, each directory lists the next folder, and the last the file.
            let mut entries: Vec<Vec<u8>> = (0..depth)
                .map(|level| record(folders[level], 45_021 + level as u32, 2048, 2))
                .collect();
            entries.push(record(identifier.as_bytes(), 45_017, 5, 0));
            for (level, entry) in entries.iter().enumerate() {
                let lba = 45_020 + level as u32;
                let parent = lba - u32::from(level > 0);
                let listing = [
                    record(b"\0", lba, 2048, 2),
                    record(b"\x01", parent, 2048, 2),
                    entry.clone(),
                ]
                .concat();
                let sector = raw_sector(lba, &listing, Edc::Zero);
                assert_eq!(track[at(20 + level)..at(21 + level)], sector[..], "{level}");
            }
        }
    }

    #[test]
    fn audio_files_join_a_set_before_its_sheet_once() {
        for (set, audio) in [
            (DiscSet::full(Edc::Zero), "track4.bin"),
            (DiscSet::full_gdi(Edc::Zero), "track04.raw"),
        ] {
            let with = set.clone().with_audio_files();
            let mut expected = names(&set);
            expected.insert(expected.len() - 1, audio.to_owned());
            assert_eq!(names(&with), expected);
            assert_eq!(bytes_of(&with, audio), vec![0; RAW_SECTOR]);
            for name in names(&set) {
                assert_eq!(bytes_of(&with, &name), bytes_of(&set, &name), "{name}");
            }
            let again = with.clone().with_audio_files();
            assert_eq!(names(&again), expected);
            assert_eq!(files_of(&again), files_of(&with));
        }
        // A sheet without audio tracks adds nothing.
        for set in [
            DiscSet::small(Edc::Zero),
            DiscSet::one_file(b"A;1", &[], Edc::Zero),
        ] {
            let with = set.clone().with_audio_files();
            assert_eq!(names(&with), names(&set));
        }
        // Every audio track a sheet lists, in its order, a quoted GDI name with its spaces.
        let cue = "FILE \"a.bin\" BINARY\r\nTRACK 01 AUDIO\r\nINDEX 01 00:00:00\r\n\
                   FILE \"b.bin\" BINARY\r\n  TRACK 02 MODE1/2352\r\n  INDEX 01 00:00:00\r\n\
                   FILE \"c d.bin\" BINARY\r\n  TRACK 03 AUDIO\r\n  INDEX 01 00:00:00\r\n";
        assert_eq!(audio_files(cue.as_bytes()), ["a.bin", "c d.bin"]);
        let gdi = "3\r\n1 0 0 2352 \"one two.raw\" 0\r\n2 600 4 2352 data.bin 0\r\n\
                   3  900\t0 2352 three.raw 0\r\n";
        assert_eq!(audio_files(gdi.as_bytes()), ["one two.raw", "three.raw"]);
        assert!(audio_files(b"").is_empty());

        let mut set = DiscSet::full_gdi(Edc::Zero);
        set.files.last_mut().unwrap().1 = whole(gdi.into());
        let names_after = names(&set.with_audio_files());
        assert_eq!(
            names_after,
            [
                "track03.bin",
                "track05.bin",
                "one two.raw",
                "three.raw",
                "full.gdi"
            ]
        );
    }

    #[test]
    fn audio_files_are_found_on_the_lines_the_extractor_reads() {
        let sheet = |set: DiscSet| String::from_utf8(bytes_of(&set, &set.sheet)).unwrap();
        let cue = sheet(DiscSet::full(Edc::Zero));
        let gdi = sheet(DiscSet::full_gdi(Edc::Zero));
        // In place of the sets' CRLF: each character a line of a sheet ends at, as the extractor
        // splits its lines, and line ends that follow a separator it strips from the line.
        let line_ends = [
            "\n",
            "\r",
            "\u{b}",
            "\u{c}",
            "\u{1c}",
            "\u{1d}",
            "\u{1e}",
            "\u{85}",
            "\u{2028}",
            "\u{2029}",
            "\u{1f}\r\n",
            "\u{1d}\u{1f}\n",
        ];
        for line_end in line_ends {
            assert_eq!(
                audio_files(cue.replace("\r\n", line_end).as_bytes()),
                ["track4.bin"],
                "{line_end:?}"
            );
            assert_eq!(
                audio_files(gdi.replace("\r\n", line_end).as_bytes()),
                ["track04.raw"],
                "{line_end:?}"
            );
        }
        // A separator stripped from a line's ends but never ending one leaves a single line,
        // which lists no track.
        assert!(audio_files(cue.replace("\r\n", "\u{1f}").as_bytes()).is_empty());
        assert!(audio_files(gdi.replace("\r\n", "\u{1f}").as_bytes()).is_empty());

        // A set whose sheet ends its lines at a CR alone still gains its audio track.
        let mut set = DiscSet::full(Edc::Zero);
        set.files.last_mut().unwrap().1 = whole(cue.replace("\r\n", "\r").into_bytes());
        let names_after = names(&set.with_audio_files());
        assert_eq!(
            names_after,
            ["track3.bin", "track5.bin", "track4.bin", "full.cue"]
        );
    }

    #[test]
    fn a_set_is_written_new_and_names_its_sheet() {
        let folder = tempfile::tempdir().unwrap();
        let set = DiscSet::small(Edc::Zero);
        let sheet = set.write(folder.path()).unwrap();
        assert_eq!(sheet, folder.path().join("test.cue"));
        let mut found: Vec<String> = fs::read_dir(folder.path())
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
        found.sort();
        let mut expected = names(&set);
        expected.sort();
        assert_eq!(found, expected);
        let files = files_of(&set);
        for (name, bytes) in &files {
            assert_eq!(
                fs::read(folder.path().join(name)).unwrap(),
                *bytes,
                "{name}"
            );
        }
        // Written again, the set finds its first file there and changes nothing.
        let error = DiscSet::small(Edc::Computed)
            .write(folder.path())
            .unwrap_err();
        assert_eq!(error.kind(), ErrorKind::AlreadyExists);
        for (name, bytes) in &files {
            assert_eq!(
                fs::read(folder.path().join(name)).unwrap(),
                *bytes,
                "{name}"
            );
        }

        // Chunks are written in their order, a later one over an earlier, never past the length.
        let odd = DiscSet {
            files: vec![(
                "odd.bin".to_owned(),
                SparseFile {
                    len: 6,
                    chunks: vec![
                        (1, b"abc".to_vec()),
                        (2, b"XY".to_vec()),
                        (4, b"1234".to_vec()),
                        (6, b"end".to_vec()),
                        (9, b"past".to_vec()),
                    ],
                    at: 0,
                },
            )],
            sheet: "odd.bin".to_owned(),
        };
        let folder = tempfile::tempdir().unwrap();
        let path = odd.write(folder.path()).unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"\0aXY12");
        assert_eq!(contents(&odd.files[0].1), b"\0aXY12");
    }

    #[test]
    fn what_a_builder_cannot_write_panics() {
        // The last address a header holds, a whole sector of user data and the longest records.
        let last = raw_sector(719_849, &[], Edc::Zero);
        assert_eq!(last[12..16], [0xF9, 0x59, 0x74, 1]);
        assert_eq!(
            raw_sector(0, &[1; SECTOR], Edc::Zero)[USER_DATA],
            [1; SECTOR]
        );
        assert_eq!(record(&[b'a'; 221], 0, 0, 0).len(), 254);
        assert_eq!(record(&[b'a'; 220], 0, 0, 0).len(), 254);
        assert_eq!(
            raw_track(0, 2, &[(1, b"x")], Edc::Zero).len(),
            2 * RAW_SECTOR
        );

        assert!(panics(|| raw_sector(719_850, &[], Edc::Zero)));
        assert!(panics(|| raw_sector(u32::MAX, &[], Edc::Zero)));
        assert!(panics(|| raw_sector(0, &[0; SECTOR + 1], Edc::Zero)));
        assert!(panics(|| record(&[b'a'; 222], 0, 0, 0)));
        assert!(panics(|| pvd_sector(&[b'v'; 33], 45_020, 2048)));
        assert!(panics(|| raw_track(0, 2, &[(2, b"x")], Edc::Zero)));
    }
}
