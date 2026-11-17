//! The Dreamcast GD-ROM as the Dreamcast extractor reads it, from the raw sectors its track files
//! keep: each sector placed by the address its own header gives, and guarded by the error
//! detection code it keeps after its user data.

mod edc;
mod sector;

pub use edc::{EDC_FIELD, EDC_POLYNOMIAL, EDC_SPAN, edc};
pub use sector::{
    FRAMES_PER_SECOND, HEADER_LEN, LBA_BIAS, MAX_CHUNK_SECTORS, RAW_SECTOR, SECONDS_PER_MINUTE,
    SECTOR, SYNC, USER_DATA, sector_lba,
};
