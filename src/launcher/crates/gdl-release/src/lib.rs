//! Versions and their order, the release feed, downloads through the system's proxies and trust
//! store, and the signatures every update is checked against.
#![forbid(unsafe_code)]

mod version;

pub use version::{Version, VersionError};
