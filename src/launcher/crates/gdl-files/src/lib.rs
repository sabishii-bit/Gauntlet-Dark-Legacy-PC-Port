//! How the launcher writes files: names, staging, publishing by rename, atomic JSON, link checks
//! and retries. Only its `platform` module may call the OS directly.
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

pub use error::FilesError;
pub use job::{BLOCK_LEN, Cancel, Cancelled, Job, NoProgress, PhaseId, Progress};
pub use json::{
    MAX_JSON_LEN, PENDING_SUFFIX, pending_path, read_bounded, read_json_bounded, write_json_atomic,
};
pub use links::{absolute_lexical, no_links};
pub use long_path::{MAX_INSTALL_ROOT_UTF16, root_fits, root_utf16_len, strip_verbatim};
pub use names::{
    ALPHA_NAMES, DEVICE_NAMES, FORBIDDEN_CHARS, MAX_COMPONENT_CHARS, MAX_PAYLOAD_PATH_CHARS,
    NameError, NameRule, NameRules, NameSet, PAYLOAD_NAMES, RelPath, check_component,
};
pub use rename::{rename_noreplace, replace};
pub use retry::{RETRIED_WINDOWS_ERRORS, RETRY_INTERVAL, RETRY_WINDOW, retry_locked};
pub use staging::{Journal, STAGING_RANDOM_CHARS, Staging};
