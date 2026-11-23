//! Builders of synthetic discs, ROMs, GD-ROM sets, payloads, release feeds and installations, so the
//! launcher's plain tests run without the player's own dumps.
#![forbid(unsafe_code)]

mod folder;
mod gdrom;
mod n64;
mod program;
mod runtime;
mod zip;

#[cfg(unix)]
pub use folder::DeniedAccess;
#[cfg(windows)]
pub use folder::make_junction;
pub use folder::{
    EntryState, HeldClosed, InstallFixture, Listed, WritesDenied, backdate, deny_writes,
    folder_names, hold_closed, holds_bind, link_folder, listed, long_root, make_alpha_markers,
    snapshot, snapshot_outside, symlink_file, symlink_file_where_allowed,
    symlink_folder_where_allowed, tree_listing,
};
pub use gdrom::{
    BootFields, DiscSet, Edc, RAW_SECTOR, SECTOR, boot_sector, both16, both32, edc, pvd_sector,
    raw_sector, record, sizes_rom,
};
pub use n64::{
    BIG_ENDIAN_FIRST_WORD, BUILDER_UNKNOWN_WORD, BuiltRom, OVERLAY_TABLE_OFFSET, OverlaySpec,
    ROM_HEADER_LEN, RomFile, WadOrder, default_rom_files, raw_deflate, reorder, rom,
    rom_with_overlays, set_word, wad,
};
pub use program::{
    FAKE_CERTIFICATE_LEN, SparseFile, TRAILER_MAGIC, append_sections, append_sections_at,
    fake_sign, table_and_trailer, tiny_elf, tiny_pe,
};
pub use runtime::{RuntimeFiles, pretty_json, sha256_hex, unhex};
pub use zip::{RawEntry, RawZip, crc32};
