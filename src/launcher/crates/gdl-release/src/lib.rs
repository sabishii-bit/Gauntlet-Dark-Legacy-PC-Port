//! Versions and their order, the platforms a release is built for, the release feed, downloads
//! through the system's proxies and trust store, and the checksum lists and signatures every
//! update is checked against.
#![forbid(unsafe_code)]

mod checksums;
mod error;
mod keyring;
mod names;
mod platform;
mod proxy;
mod version;

pub use checksums::{ChecksumList, MAX_LIST_LEN};
pub use error::ReleaseError;
pub use keyring::Keyring;
pub use names::{
    ASSET_STEM, GITHUB_API_HOST, GITHUB_HOST, RELEASE_HOSTS, REPOSITORY, SIGNATURE_SUFFIX,
    checksums_asset, launcher_asset, licences_asset, linux_member, signature_asset,
};
pub use platform::Platform;
pub use proxy::{InternetSettings, ProxySettings};
pub use version::{Version, VersionError};
