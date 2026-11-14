//! Versions and their order, the platforms a release is built for, the release feed, downloads
//! through the system's proxies and trust store, and the signatures every update is checked
//! against.
#![forbid(unsafe_code)]

mod platform;
mod version;

pub use platform::Platform;
pub use version::{Version, VersionError};
