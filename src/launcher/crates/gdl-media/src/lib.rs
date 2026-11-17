//! Read-only readers of the player's dumps: the GameCube ISO and CISO with its file table, TPL
//! icons, the N64 ROM with its ROMDISK and overlays, and the Dreamcast GD-ROM (CUE and GDI) with
//! ISO 9660. A dump is only ever opened for reading.
#![forbid(unsafe_code)]

mod digest;
mod error;
mod gdrom;
mod identify;
mod n64;
mod source_name;
#[cfg(test)]
mod vectors;

pub use digest::{Sha1Digest, Sha256Digest};
pub use error::{DiscRule, MediaError, RomRule};
pub use gdrom::{
    AREA, BOOT_LBA, BOOT_LEN, BOOT_SECTORS, DataTrack, DiscFile, DiscHead, DiscRules, EDC_FIELD,
    EDC_POLYNOMIAL, EDC_SPAN, EXECUTABLE, FRAMES_PER_SECOND, GdRom, HARDWARE_ID, HEADER_LEN,
    INSTALL_RULES, IpBin, LBA_BIAS, MAX_CHUNK_SECTORS, MAX_SHEET_LEN, PRODUCT, RAW_SECTOR,
    SCRIPT_RULES, SECONDS_PER_MINUTE, SECTOR, SYNC, SectorCheck, Sheet, SheetKind, SheetTrack,
    TITLE, TITLE_PREFIX, TrackMode, USER_DATA, VERSION, ascii_field, edc, lines, parse_sheet,
    read_head, read_sheet, sector_lba, strip,
};
pub use identify::Region;
pub use n64::{
    ByteOrder, COMPRESSED, HEADER_WORDS_AT, INFLATE_STEP, MAX_OVERLAY_PADDING, MAX_ROM_LEN,
    MAX_WAD_SECTIONS, MIN_WAD_DIRECTORY, N64Rom, OVERLAY_NAME_LEN, Overlay, OverlayTable,
    PROGRAM_FOLDER, PROGRAM_SUFFIX, RECORD_WORDS_LEN, ROM_HEADER_LEN, ROMDISK_HEADER_LEN,
    ROMDISK_LIMITS, ROMDISK_MAGIC, RomHeader, Romdisk, RomdiskEntry, RomdiskLimits, Trailer,
    US_OVERLAYS, US_SHA1, WAD_ROW_LEN, WadByteOrder, WadSection, inflate, wad_sections,
};
pub use source_name::SourceName;
