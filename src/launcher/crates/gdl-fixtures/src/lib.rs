//! Builders of synthetic discs, ROMs, GD-ROM sets, payloads, release feeds and installations, so the
//! launcher's plain tests run without the player's own dumps.
#![forbid(unsafe_code)]

mod folder;
mod program;
mod runtime;
mod zip;

#[cfg(unix)]
pub use folder::DeniedAccess;
#[cfg(windows)]
pub use folder::make_junction;
pub use folder::{
    EntryState, HeldClosed, InstallFixture, backdate, folder_names, hold_closed, link_folder,
    long_root, make_alpha_markers, snapshot, symlink_file, symlink_file_where_allowed,
    symlink_folder_where_allowed,
};
pub use program::{
    FAKE_CERTIFICATE_LEN, SparseFile, TRAILER_MAGIC, append_sections, append_sections_at,
    fake_sign, table_and_trailer, tiny_elf, tiny_pe,
};
pub use runtime::{RuntimeFiles, pretty_json, sha256_hex};
pub use zip::{RawEntry, RawZip, crc32};
