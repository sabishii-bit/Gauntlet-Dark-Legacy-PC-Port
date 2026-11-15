//! Builders of synthetic discs, ROMs, GD-ROM sets, payloads, release feeds and installations, so the
//! launcher's plain tests run without the player's own dumps.
#![forbid(unsafe_code)]

mod folder;

#[cfg(unix)]
pub use folder::DeniedAccess;
#[cfg(windows)]
pub use folder::make_junction;
pub use folder::{folder_names, link_folder, long_root, make_alpha_markers, symlink_file};
