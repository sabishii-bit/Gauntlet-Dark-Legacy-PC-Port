//! Versions and their order, the platforms a release is built for, the release feed, downloads
//! through the system's proxies and trust store, and the checksum lists and signatures every
//! update is checked against.
#![forbid(unsafe_code)]

mod checksums;
mod download;
mod error;
mod feed;
mod http;
mod keyring;
mod names;
mod platform;
mod proxy;
mod version;

pub use checksums::{ChecksumList, MAX_LIST_LEN};
pub use download::{DOWNLOAD_ATTEMPTS, Downloaded};
pub use error::ReleaseError;
#[cfg(test)]
pub use feed::LoopbackError;
pub use feed::{
    Asset, CheckedList, Feed, FeedConfig, MAX_ASSET_LEN, MAX_NOTES_LEN, MAX_PAGE_BYTES, MAX_PAGES,
    PAGE_LEN, Release, ReleaseFiles, Scan,
};
pub use http::{
    API_ACCEPT, API_VERSION, DOWNLOAD_ACCEPT, MAX_REDIRECTS, READ_TIMEOUT, SECONDARY_LIMIT_WAIT,
    USER_AGENT_PRODUCT,
};
pub use keyring::Keyring;
pub use names::{
    ASSET_STEM, GITHUB_API_HOST, GITHUB_HOST, RELEASE_HOSTS, REPOSITORY, SIGNATURE_SUFFIX,
    checksums_asset, launcher_asset, licences_asset, linux_member, signature_asset,
};
pub use platform::Platform;
pub use proxy::{InternetSettings, ProxySettings};
pub use version::{Version, VersionError};
