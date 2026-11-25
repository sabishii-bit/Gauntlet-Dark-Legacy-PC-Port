//! What the release feed, its transport and its downloads report when they fail: the rule a
//! failure breaks and the values it was found with, as technical detail for logs, in the alpha
//! updater's own words where it has fixed words for the failure. What a player reads is chosen
//! from the failure elsewhere.

use std::io;
use std::path::PathBuf;

use gdl_files::Cancelled;

/// Why reading the release feed, checking a release or downloading one of its files failed.
#[derive(Debug, thiserror::Error)]
pub enum ReleaseError {
    /// The feed or a download could not be reached: the host's name did not resolve, the
    /// connection was refused, reset or broken off, the network or the host could not be reached,
    /// or the host did not answer in time.
    #[error("The release feed could not be reached: {detail}")]
    Offline {
        /// What failed, in the transport's words.
        detail: Box<str>,
    },
    /// A request failed in another way than an unreachable host, such as a secure connection that
    /// could not be made or a redirect to no place a request can go.
    #[error("The release feed failed: {detail}")]
    Failed {
        /// What failed, in the transport's words.
        detail: Box<str>,
    },
    /// The feed answered with a status that is neither success, nor a redirect, nor a rate limit.
    #[error("GitHub answered HTTP {status} for {url}")]
    Status {
        /// The HTTP status of the answer.
        status: u16,
        /// The URL that answered it.
        url: Box<str>,
    },
    /// GitHub refused the request because the hourly or a secondary limit on requests made
    /// without an account is spent.
    #[error("GitHub's limit of unauthenticated requests is spent{}", limit_reset(*.reset_at))]
    RateLimited {
        /// When the feed may be asked again, in seconds since the Unix epoch; `None` when the
        /// answer gave no time that could be read.
        reset_at: Option<u64>,
    },
    /// A request, or a redirect it was sent on, would reach a place the launcher never fetches
    /// from: a host other than GitHub's release hosts, one of them by a URL that is not HTTPS,
    /// names a user or names another port than HTTPS's own, or another origin than a loopback
    /// feed's own.
    #[error("Update download left GitHub's secure release hosts: {url}")]
    UntrustedHost {
        /// The URL refused.
        url: Box<str>,
    },
    /// A request was redirected more times than one may be, or back to a URL it had reached.
    #[error("Too many redirects, or a redirect loop, from {url}")]
    Redirects {
        /// The URL that was being fetched.
        url: Box<str>,
    },
    /// A page of the releases list holds more bytes than the launcher reads of one.
    #[error("GitHub release response is too large")]
    ResponseTooLarge,
    /// A page of the releases list is not JSON, or not a list of releases.
    #[error("Invalid GitHub release response: {detail}")]
    InvalidResponse {
        /// What is wrong with it.
        detail: Box<str>,
    },
    /// A release's assets, as the feed gives them, are not a list.
    #[error("Invalid GitHub release asset list: {tag}")]
    AssetList {
        /// The release's tag.
        tag: Box<str>,
    },
    /// The releases list runs on past the most pages the launcher reads.
    #[error("Too many releases to check safely")]
    TooManyReleases,
    /// An asset a release must carry is not there, is still being uploaded, gives a size out of
    /// its bounds, lacks GitHub's SHA-256 digest, or names a download URL other than its own.
    #[error("Release asset is incomplete or lacks its GitHub SHA-256 digest: {asset}")]
    Incomplete {
        /// The asset's name.
        asset: Box<str>,
    },
    /// A release lists two assets of one name.
    #[error("Ambiguous release asset: {asset}")]
    Ambiguous {
        /// The name the assets share.
        asset: Box<str>,
    },
    /// A download runs past the size its release publishes for it.
    #[error("Update download exceeds its published size: {asset}")]
    Oversize {
        /// The asset's name.
        asset: Box<str>,
    },
    /// A download ended short of its published size, or its bytes do not have its asset's
    /// digest.
    #[error("Update download is incomplete or failed its SHA-256 check: {asset}")]
    Corrupt {
        /// The asset's name.
        asset: Box<str>,
    },
    /// A release's checksum list is not one the release scripts write: lines of a SHA-256 digest
    /// and a name, each name once.
    #[error("The release's checksum list is malformed: {detail}")]
    ChecksumList {
        /// Which line breaks it, and how.
        detail: Box<str>,
    },
    /// A release's checksum list does not list one of its assets, or gives it another digest
    /// than GitHub's.
    #[error("The release's checksum list does not give {asset} GitHub's digest")]
    ChecksumMismatch {
        /// The asset's name.
        asset: Box<str>,
    },
    /// A release's checksum list carries no signature the launcher accepts: the signature is
    /// malformed, made by no key the launcher trusts, or names another file or version.
    #[error("The release's checksum list is not signed as this launcher requires: {detail}")]
    Signature {
        /// Why the signature is refused.
        detail: Box<str>,
    },
    /// The system's proxy setting for a request cannot be used: it names a SOCKS proxy, or a proxy
    /// URL that cannot be read.
    #[error("The system's proxy setting cannot be used: {detail}")]
    Proxy {
        /// Why it cannot be used.
        detail: Box<str>,
    },
    /// Writing or reading a downloaded file failed.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The file.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The job was asked to stop before the feed was read or the download was done.
    #[error("cancelled")]
    Cancelled,
}

impl ReleaseError {
    /// The failure's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        // Every variant is named, with no wildcard, so that a failure added later is given its
        // rule on purpose.
        match self {
            ReleaseError::Offline { .. } => "offline",
            ReleaseError::Failed { .. } => "failed",
            ReleaseError::Status { .. } => "status",
            ReleaseError::RateLimited { .. } => "rateLimited",
            ReleaseError::UntrustedHost { .. } => "untrustedHost",
            ReleaseError::Redirects { .. } => "redirects",
            ReleaseError::ResponseTooLarge => "responseTooLarge",
            ReleaseError::InvalidResponse { .. } => "invalidResponse",
            ReleaseError::AssetList { .. } => "assetList",
            ReleaseError::TooManyReleases => "tooManyReleases",
            ReleaseError::Incomplete { .. } => "incomplete",
            ReleaseError::Ambiguous { .. } => "ambiguous",
            ReleaseError::Oversize { .. } => "oversize",
            ReleaseError::Corrupt { .. } => "corrupt",
            ReleaseError::ChecksumList { .. } => "checksumList",
            ReleaseError::ChecksumMismatch { .. } => "checksumMismatch",
            ReleaseError::Signature { .. } => "signature",
            ReleaseError::Proxy { .. } => "proxy",
            ReleaseError::Io { .. } => "io",
            ReleaseError::Cancelled => "cancelled",
        }
    }
}

impl From<Cancelled> for ReleaseError {
    /// A read of the feed or a download stops where its job's cancel tells it to, between two
    /// requests or two blocks of a file.
    fn from(_: Cancelled) -> ReleaseError {
        ReleaseError::Cancelled
    }
}

/// How a rate limit's refusal tells when the feed may be asked again: the time GitHub gave, in
/// seconds since the Unix epoch, or that it gave none.
fn limit_reset(reset_at: Option<u64>) -> String {
    match reset_at {
        Some(at) => format!(" until {at} (seconds since the Unix epoch)"),
        None => ", with no time given for its reset".to_owned(),
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::error::Error;
    use std::io;
    use std::path::Path;

    use gdl_files::Cancelled;

    use super::ReleaseError;
    use crate::VersionError;

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// An asset every failure about one can name.
    const ASSET: &str = "GauntletDarkLegacy-1.0.0-beta-windows-x64-launcher.exe";

    /// A URL every failure about a request can name.
    const URL: &str = "https://api.github.com/repos/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases?per_page=100&page=1";

    #[test]
    fn release_rules_are_stable() {
        let boxed = |text: &str| -> Box<str> { text.into() };
        let file = Path::new(".gdl-launcher")
            .join("download-AbC123xy")
            .join(ASSET);
        let cases = [
            (
                ReleaseError::Offline {
                    detail: boxed("connection refused"),
                },
                "offline",
                "The release feed could not be reached: connection refused".to_owned(),
            ),
            (
                ReleaseError::Failed {
                    detail: boxed("invalid peer certificate"),
                },
                "failed",
                "The release feed failed: invalid peer certificate".to_owned(),
            ),
            (
                ReleaseError::Status {
                    status: 404,
                    url: boxed(URL),
                },
                "status",
                format!("GitHub answered HTTP 404 for {URL}"),
            ),
            (
                ReleaseError::RateLimited {
                    reset_at: Some(1_764_003_600),
                },
                "rateLimited",
                "GitHub's limit of unauthenticated requests is spent until 1764003600 (seconds \
                 since the Unix epoch)"
                    .to_owned(),
            ),
            // A limit whose end the answer did not give is the same failure, which says so.
            (
                ReleaseError::RateLimited { reset_at: None },
                "rateLimited",
                "GitHub's limit of unauthenticated requests is spent, with no time given for its \
                 reset"
                    .to_owned(),
            ),
            (
                ReleaseError::UntrustedHost {
                    url: boxed("http://github.com/file"),
                },
                "untrustedHost",
                "Update download left GitHub's secure release hosts: http://github.com/file"
                    .to_owned(),
            ),
            (
                ReleaseError::Redirects { url: boxed(URL) },
                "redirects",
                format!("Too many redirects, or a redirect loop, from {URL}"),
            ),
            (
                ReleaseError::ResponseTooLarge,
                "responseTooLarge",
                "GitHub release response is too large".to_owned(),
            ),
            (
                ReleaseError::InvalidResponse {
                    detail: boxed("not a list"),
                },
                "invalidResponse",
                "Invalid GitHub release response: not a list".to_owned(),
            ),
            (
                ReleaseError::AssetList {
                    tag: boxed("v1.0.0-beta"),
                },
                "assetList",
                "Invalid GitHub release asset list: v1.0.0-beta".to_owned(),
            ),
            (
                ReleaseError::TooManyReleases,
                "tooManyReleases",
                "Too many releases to check safely".to_owned(),
            ),
            (
                ReleaseError::Incomplete {
                    asset: boxed(ASSET),
                },
                "incomplete",
                format!("Release asset is incomplete or lacks its GitHub SHA-256 digest: {ASSET}"),
            ),
            (
                ReleaseError::Ambiguous {
                    asset: boxed(ASSET),
                },
                "ambiguous",
                format!("Ambiguous release asset: {ASSET}"),
            ),
            (
                ReleaseError::Oversize {
                    asset: boxed(ASSET),
                },
                "oversize",
                format!("Update download exceeds its published size: {ASSET}"),
            ),
            (
                ReleaseError::Corrupt {
                    asset: boxed(ASSET),
                },
                "corrupt",
                format!("Update download is incomplete or failed its SHA-256 check: {ASSET}"),
            ),
            (
                ReleaseError::ChecksumList {
                    detail: boxed("line 2 is not a digest and a name"),
                },
                "checksumList",
                "The release's checksum list is malformed: line 2 is not a digest and a name"
                    .to_owned(),
            ),
            (
                ReleaseError::ChecksumMismatch {
                    asset: boxed(ASSET),
                },
                "checksumMismatch",
                format!("The release's checksum list does not give {ASSET} GitHub's digest"),
            ),
            (
                ReleaseError::Signature {
                    detail: boxed("signed by no key this launcher trusts"),
                },
                "signature",
                "The release's checksum list is not signed as this launcher requires: signed by \
                 no key this launcher trusts"
                    .to_owned(),
            ),
            (
                ReleaseError::Proxy {
                    detail: boxed("SOCKS proxies are not supported"),
                },
                "proxy",
                "The system's proxy setting cannot be used: SOCKS proxies are not supported"
                    .to_owned(),
            ),
            (
                ReleaseError::Io {
                    path: file.clone(),
                    source: io::Error::new(io::ErrorKind::PermissionDenied, "denied"),
                },
                "io",
                format!("{}: denied", file.display()),
            ),
            // The rule is the variant's, whatever the system answered: a full disk is `io` too.
            (
                ReleaseError::Io {
                    path: file.clone(),
                    source: io::ErrorKind::StorageFull.into(),
                },
                "io",
                format!(
                    "{}: {}",
                    file.display(),
                    io::Error::from(io::ErrorKind::StorageFull)
                ),
            ),
            (ReleaseError::Cancelled, "cancelled", "cancelled".to_owned()),
        ];

        let mut rules = BTreeSet::new();
        for (error, rule, shown) in &cases {
            assert_eq!(error.rule(), *rule, "{error}");
            assert_eq!(error.to_string(), *shown, "{rule}");
            rules.insert(*rule);
            // Only a failure of the system has a cause of its own; every other is the launcher's
            // refusal or what the feed answered, said in full by its own text.
            match error {
                ReleaseError::Io { source, .. } => {
                    let cause = error.source().expect("an I/O failure keeps its cause");
                    assert_eq!(cause.to_string(), source.to_string());
                }
                _ => assert!(error.source().is_none(), "{rule}"),
            }
        }
        // Every variant has a rule of its own.
        let expected = [
            "offline",
            "failed",
            "status",
            "rateLimited",
            "untrustedHost",
            "redirects",
            "responseTooLarge",
            "invalidResponse",
            "assetList",
            "tooManyReleases",
            "incomplete",
            "ambiguous",
            "oversize",
            "corrupt",
            "checksumList",
            "checksumMismatch",
            "signature",
            "proxy",
            "io",
            "cancelled",
        ];
        assert_eq!(rules, BTreeSet::from(expected));
        assert_eq!(rules.len(), expected.len());
    }

    #[test]
    fn a_cancelled_job_is_the_feeds_cancel() {
        let error = ReleaseError::from(Cancelled);
        assert!(matches!(error, ReleaseError::Cancelled));
        assert_eq!(error.rule(), "cancelled");
    }

    #[test]
    fn errors_stay_small() {
        assert!(size_of::<ReleaseError>() <= MOST_ERROR_BYTES);
        assert!(size_of::<VersionError>() <= MOST_ERROR_BYTES);
    }
}
