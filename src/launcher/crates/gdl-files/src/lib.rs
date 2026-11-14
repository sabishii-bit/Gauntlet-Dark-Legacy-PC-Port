//! How the launcher writes files: names, staging, publishing by rename, atomic JSON, link checks
//! and retries. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]

mod names;
mod retry;

pub use names::{
    ALPHA_NAMES, DEVICE_NAMES, FORBIDDEN_CHARS, MAX_COMPONENT_CHARS, MAX_PAYLOAD_PATH_CHARS,
    NameError, NameRule, NameRules, NameSet, PAYLOAD_NAMES, RelPath, check_component,
};
pub use retry::{RETRIED_WINDOWS_ERRORS, RETRY_INTERVAL, RETRY_WINDOW, retry_locked};
