//! How the launcher writes files: names, staging, publishing by rename, atomic JSON, link checks
//! and retries, and what a folder and its volume allow before anything is written there. Only its
//! `platform` module may call the OS directly.
#![deny(unsafe_code)]

mod error;
mod job;
mod json;
mod links;
mod long_path;
mod names;
#[allow(unsafe_code)]
mod platform;
mod rename;
mod retry;
mod staging;
mod volume;

pub use error::FilesError;
pub use job::{
    BLOCK_LEN, Cancel, Cancelled, Job, NoProgress, PhaseId, Progress, RunningTotal, fill_from,
};
pub use json::{
    MAX_JSON_LEN, PENDING_SUFFIX, pending_path, read_bounded, read_json_bounded, write_json_atomic,
};
pub use links::{absolute_lexical, is_absent, no_links};
#[cfg(windows)]
pub use long_path::VerbatimPath;
pub use long_path::{MAX_INSTALL_ROOT_UTF16, root_fits, root_utf16_len, strip_verbatim};
pub use names::{
    ALPHA_NAMES, DEVICE_NAMES, DISC_NAMES, FORBIDDEN_CHARS, MAX_COMPONENT_CHARS,
    MAX_DISC_PATH_CHARS, MAX_PAYLOAD_PATH_CHARS, NameError, NameRule, NameRules, NameSet,
    PAYLOAD_NAMES, ROMDISK_NAMES, RelPath, TRACK_NAMES, check_component,
};
pub use rename::{rename_noreplace, replace};
pub use retry::{RETRIED_WINDOWS_ERRORS, RETRY_INTERVAL, RETRY_WINDOW, retry_locked};
pub use staging::{Journal, STAGING_RANDOM_CHARS, Staging, is_staging_name};
pub use volume::{
    FolderProbe, PROBE_PREFIX, ProbedFolder, Volume, nearest_existing, probe_folder,
    refuses_writes, volume_of,
};
