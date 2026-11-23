//! Read-only readers of the player's dumps: the GameCube ISO and CISO with its file table, TPL
//! icons, the N64 ROM with its ROMDISK and overlays, and the Dreamcast GD-ROM (CUE and GDI) with
//! ISO 9660 and its packed file table. A dump is only ever opened for reading.
#![forbid(unsafe_code)]

mod digest;
mod dump_file;
mod error;
mod gamecube;
mod gdrom;
mod identify;
mod n64;
mod source_name;
mod tpl;
#[cfg(test)]
mod vectors;

pub use digest::{Sha1Digest, Sha256Digest};
pub use error::{DiscRule, GameCubeRule, MediaError, RomRule, TplRule};
pub use gamecube::{
    DISC_SIZE, DiscEntry, DiscEntryKind, DiscHeader, GameCubeImage, MAX_DISC_ENTRIES, MAX_FST_LEN,
    MAX_REVISION, US_GAME_ID, gamecube_verdict,
};
pub use gdrom::{
    ALLOWED_FLAGS, AREA, BLOCK_SIZE_AT, BOOT_LBA, BOOT_LEN, BOOT_SECTORS, DataTrack, DiscFile,
    DiscHead, DiscRules, EDC_FIELD, EDC_POLYNOMIAL, EDC_SPAN, EXECUTABLE, FLAG_DIRECTORY,
    FRAMES_PER_SECOND, GdRom, HARDWARE_ID, HEADER_LEN, INSTALL_RULES, IpBin, LBA_BIAS,
    MAX_CHUNK_SECTORS, MAX_DIRECTORY_DEPTH, MAX_DIRECTORY_LEN, MAX_ENTRIES, MAX_SHEET_LEN,
    MAX_SIZES_ROM_LEN, MIN_RECORD_LEN, PRODUCT, PVD_LBA, PackedFile, PackedPlace, RAW_SECTOR,
    ROOT_RECORD_AT, SCRIPT_RULES, SECONDS_PER_MINUTE, SECTOR, SIZES_COMPONENTS, SIZES_ROW_LEN,
    STANDARD_ID, SYNC, SectorCheck, Sheet, SheetKind, SheetTrack, TITLE, TITLE_PREFIX, TrackMode,
    USER_DATA, VERSION, VOLUME_ID, ascii_field, edc, find_sheet, lines, packed_files, parse_sheet,
    read_head, read_sheet, sector_lba, strip,
};
pub use identify::{
    Console, Container, DiscIdentity, DumpRefusal, Identity, Region, Source, TrackFile, US_DISC,
    Verdict, disc_verdict, identify, n64_verdict,
};
pub use n64::{
    ByteOrder, COMPRESSED, HEADER_WORDS_AT, INFLATE_STEP, MAX_OVERLAY_PADDING, MAX_ROM_LEN,
    MAX_WAD_SECTIONS, MIN_WAD_DIRECTORY, N64Rom, OVERLAY_NAME_LEN, Overlay, OverlayTable,
    PROGRAM_FOLDER, PROGRAM_SUFFIX, RECORD_WORDS_LEN, ROM_HEADER_LEN, ROMDISK_HEADER_LEN,
    ROMDISK_LIMITS, ROMDISK_MAGIC, RomHeader, Romdisk, RomdiskEntry, RomdiskLimits, Trailer,
    US_OVERLAYS, US_SHA1, WAD_ROW_LEN, WadByteOrder, WadSection, inflate, wad_sections,
};
pub use source_name::SourceName;
pub use tpl::{Frame, MAX_TPL_LEN, first_frame};
