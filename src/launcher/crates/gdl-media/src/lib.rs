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
    EDC_FIELD, EDC_POLYNOMIAL, EDC_SPAN, FRAMES_PER_SECOND, HEADER_LEN, LBA_BIAS,
    MAX_CHUNK_SECTORS, RAW_SECTOR, SECONDS_PER_MINUTE, SECTOR, SYNC, USER_DATA, edc, sector_lba,
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
