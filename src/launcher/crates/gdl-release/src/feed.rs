//! GitHub's releases list read as the launcher's feed: every page of it, each release held to the
//! new line's names and its assets to the rules a download needs, and this platform's newest
//! release chosen by semantic version, whatever order the list gives. A release that carries no
//! launcher file of the new line, as every alpha release, is passed over without an error. A
//! release's checksum list is fetched and checked, against its signature when the launcher trusts
//! a key, before anything else of the release is downloaded.

use std::cmp::Ordering;
use std::path::Path;

use gdl_files::{Cancel, Job};
use gdl_media::Sha256Digest;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use crate::http::{Origins, Transport, Wanted, percent_decoded, read_bounded};
use crate::{
    ChecksumList, Downloaded, GITHUB_API_HOST, GITHUB_HOST, Keyring, MAX_LIST_LEN, Platform,
    ProxySettings, REPOSITORY, ReleaseError, Version, checksums_asset, launcher_asset,
    signature_asset,
};

/// How many releases a page of the list holds, asked for and counted: a page of fewer is the
/// last.
pub const PAGE_LEN: usize = 100;

/// The most pages of the list read: a list that fills them all is refused.
pub const MAX_PAGES: u32 = 20;

/// The most bytes a page of the list may hold.
pub const MAX_PAGE_BYTES: u64 = 8 << 20;

/// The most bytes a release's launcher file may hold.
pub const MAX_ASSET_LEN: u64 = 2 << 30;

/// The most bytes of a release's notes kept; longer notes are dropped.
pub const MAX_NOTES_LEN: usize = 64 << 10;

/// The state of an asset GitHub holds whole.
const UPLOADED: &str = "uploaded";

/// What GitHub's digest of an asset begins with, before its SHA-256 in lowercase hexadecimal.
const DIGEST_PREFIX: &str = "sha256:";

/// What a release's tag puts before its version.
const TAG_PREFIX: char = 'v';

/// The scheme of GitHub's download URLs.
const HTTPS: &str = "https";

/// The scheme of a loopback feed's download URLs.
#[cfg(test)]
const HTTP: &str = "http";

/// What parts a URL's scheme from its authority.
const SCHEME_SEPARATOR: &str = "://";

/// The hosts a loopback feed may name, each before a colon and its port.
#[cfg(test)]
const LOOPBACK_HOSTS: [&str; 2] = ["127.0.0.1", "[::1]"];

/// A release row's tag.
const TAG_NAME: &str = "tag_name";

/// Whether a release row is a draft.
const DRAFT: &str = "draft";

/// A release row's assets.
const ASSETS: &str = "assets";

/// A release row's notes.
const BODY: &str = "body";

/// An asset's file name.
const NAME: &str = "name";

/// Whether GitHub holds an asset whole.
const STATE: &str = "state";

/// An asset's length in bytes.
const SIZE: &str = "size";

/// GitHub's digest of an asset.
const DIGEST: &str = "digest";

/// Where an asset is downloaded from.
const DOWNLOAD_URL: &str = "browser_download_url";

/// Where the feed reads: GitHub, or a loopback feed of a test build.
#[derive(Clone, Debug)]
pub struct FeedConfig {
    /// The releases list's URL, without the page asked for.
    api: String,
    /// Where the list and the downloads it names may be fetched from.
    origins: Origins,
    /// The proxies each request is sent through.
    proxies: ProxySettings,
    /// The running launcher's version, which every request names.
    launcher_version: String,
}

impl FeedConfig {
    /// GitHub's releases list of the launcher's repository, read through `proxies`, as launcher
    /// `launcher_version`.
    pub fn github(launcher_version: &str, proxies: ProxySettings) -> FeedConfig {
        FeedConfig {
            api: format!("{HTTPS}{SCHEME_SEPARATOR}{GITHUB_API_HOST}/repos/{REPOSITORY}/releases"),
            origins: Origins::GitHub,
            proxies,
            launcher_version: launcher_version.to_owned(),
        }
    }

    /// A loopback feed at `base`, which must be `http://127.0.0.1:<port>/` or
    /// `http://[::1]:<port>/`: its list at `<base>repos/<repository>/releases`, its downloads
    /// under `<base>`, and no proxy.
    #[cfg(test)]
    pub fn loopback(base: &str, launcher_version: &str) -> Result<FeedConfig, LoopbackError> {
        let authority =
            loopback_authority(base).ok_or_else(|| LoopbackError { given: base.into() })?;
        Ok(FeedConfig {
            api: format!("{base}repos/{REPOSITORY}/releases"),
            origins: Origins::Loopback {
                authority: authority.to_owned(),
            },
            proxies: ProxySettings::none(),
            launcher_version: launcher_version.to_owned(),
        })
    }

    /// Whether `url` is where the asset `name` of the release tagged `tag` is downloaded from:
    /// the feed's own download host and scheme, exactly, then no query or fragment, and a path
    /// that, percent-decoded, is exactly `/<repository>/releases/download/<tag>/<name>`.
    fn downloads_from(&self, url: &str, tag: &str, name: &str) -> bool {
        let (scheme, authority) = match &self.origins {
            Origins::GitHub => (HTTPS, GITHUB_HOST),
            #[cfg(test)]
            Origins::Loopback { authority } => (HTTP, authority.as_str()),
        };
        let Some(path) = url
            .strip_prefix(scheme)
            .and_then(|rest| rest.strip_prefix(SCHEME_SEPARATOR))
            .and_then(|rest| rest.strip_prefix(authority))
        else {
            return false;
        };
        path.starts_with('/')
            && !path.contains(['?', '#'])
            && percent_decoded(path)
                .is_some_and(|path| path == format!("/{REPOSITORY}/releases/download/{tag}/{name}"))
    }
}

/// The host and port of a loopback feed's `base`, `http://<host>:<port>/` for one of
/// [`LOOPBACK_HOSTS`] and a port from 1 written without a sign or leading zeros.
#[cfg(test)]
fn loopback_authority(base: &str) -> Option<&str> {
    let authority = base
        .strip_prefix(HTTP)?
        .strip_prefix(SCHEME_SEPARATOR)?
        .strip_suffix('/')?;
    let port = LOOPBACK_HOSTS
        .iter()
        .find_map(|host| authority.strip_prefix(host)?.strip_prefix(':'))?;
    let number: u16 = port.parse().ok()?;
    (number != 0 && number.to_string() == port).then_some(authority)
}

/// A base URL that names no loopback feed.
#[cfg(test)]
#[derive(Debug, thiserror::Error)]
#[error("A loopback feed is http://127.0.0.1:<port>/ or http://[::1]:<port>/, not {given:?}")]
pub struct LoopbackError {
    /// The base as it was given.
    pub given: Box<str>,
}

/// A release asset, as the feed lists it and the rules accept it. Its JSON form, which an update
/// check's kept answer holds, gives the digest as its lowercase hexadecimal text.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Asset {
    /// The asset's file name.
    pub name: String,
    /// Where it is downloaded from, as the feed gives it.
    pub url: String,
    /// Its length in bytes.
    pub size: u64,
    /// GitHub's SHA-256 digest of it.
    #[serde(with = "crate::cache::digest_text")]
    pub sha256: Sha256Digest,
}

/// A release of the launcher's new line for one platform: its launcher file, with the game inside,
/// its checksum list and, when it carries one, the list's signature. Its JSON form, which an
/// update check's kept answer holds, leaves out the signature and the notes when there are none.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Release {
    /// The release's version, its tag without the `v`.
    pub version: Version,
    /// The release's tag, as the feed gives it.
    pub tag: String,
    /// The platform whose files these are.
    pub platform: Platform,
    /// The launcher file.
    pub launcher: Asset,
    /// The checksum list of the release's files.
    pub checksums: Asset,
    /// The checksum list's signature, when the release carries one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub signature: Option<Asset>,
    /// The release's notes, when they are text within [`MAX_NOTES_LEN`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notes: Option<String>,
}

/// What one read of the feed found.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Scan {
    /// This platform's newest release above the floor, when the feed has one.
    pub candidate: Option<Release>,
    /// How many rows the list's pages held.
    pub rows: usize,
    /// How many releases above the floor were passed over for carrying no launcher file of this
    /// platform's on the new line.
    pub passed_over: usize,
}

/// A release's checksum list, fetched and checked: what an update downloads before the release's
/// launcher file, and holds that file to.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CheckedList {
    /// The list, read.
    pub list: ChecksumList,
    /// The list's bytes, as the release carries them.
    pub bytes: Vec<u8>,
    /// The list's signature, as the release carries it, when it was checked against the keyring;
    /// `None` when the keyring holds no key, and so no signature was fetched.
    pub signature: Option<Vec<u8>>,
}

impl CheckedList {
    /// Whether the list's signature was checked: whether the keyring held a key.
    pub fn signed(&self) -> bool {
        self.signature.is_some()
    }
}

/// Where an update fetches a release's checksum list and launcher file from: the feed, or a
/// test's stand-in that holds the files itself.
pub trait ReleaseFiles {
    /// `release`'s checksum list, fetched and checked as [`Feed::check_list`] checks it.
    fn check_list(&self, release: &Release, cancel: &Cancel) -> Result<CheckedList, ReleaseError>;

    /// `asset` downloaded into `folder`, as [`Feed::download`] downloads it.
    fn download(
        &self,
        asset: &Asset,
        folder: &Path,
        job: &Job<'_>,
    ) -> Result<Downloaded, ReleaseError>;
}

impl ReleaseFiles for Feed {
    fn check_list(&self, release: &Release, cancel: &Cancel) -> Result<CheckedList, ReleaseError> {
        Feed::check_list(self, release, cancel)
    }

    fn download(
        &self,
        asset: &Asset,
        folder: &Path,
        job: &Job<'_>,
    ) -> Result<Downloaded, ReleaseError> {
        Feed::download(self, asset, folder, job)
    }
}

/// The release feed: GitHub's releases list, or a loopback feed of a test build, and the keys a
/// release's checksum list must be signed by.
pub struct Feed {
    config: FeedConfig,
    /// The transport every request of the feed and its downloads goes through.
    pub(crate) transport: Transport,
    keyring: Keyring,
}

impl Feed {
    /// The feed `config` names, trusting the keys built into this launcher.
    pub fn new(config: FeedConfig) -> Feed {
        Feed::with_keyring(config, Keyring::release())
    }

    /// The feed `config` names, trusting `keyring`'s keys: a release must carry a signature when
    /// the keyring holds a key.
    pub fn with_keyring(config: FeedConfig, keyring: Keyring) -> Feed {
        let transport = Transport::new(
            config.origins.clone(),
            config.proxies.clone(),
            &config.launcher_version,
        );
        Feed {
            config,
            transport,
            keyring,
        }
    }

    /// The keys the feed's releases are checked against.
    pub fn keyring(&self) -> &Keyring {
        &self.keyring
    }

    /// Reads every page of the list and gives `platform`'s newest new-line release above `floor`
    /// (any, given none). A release above the floor that carries this platform's launcher file is
    /// a candidate, and every candidate's assets must meet the rules, or the read fails; one that
    /// carries none is passed over and counted. The cancel is looked at before each page.
    pub fn scan(
        &self,
        floor: Option<&Version>,
        platform: Platform,
        cancel: &Cancel,
    ) -> Result<Scan, ReleaseError> {
        let mut scan = Scan {
            candidate: None,
            rows: 0,
            passed_over: 0,
        };
        for page in 1..=MAX_PAGES {
            cancel.check()?;
            let url = format!("{}?per_page={PAGE_LEN}&page={page}", self.config.api);
            let mut response = self.transport.get(&url, Wanted::Api, cancel)?;
            let body = read_bounded(
                response.body_mut(),
                MAX_PAGE_BYTES,
                ReleaseError::ResponseTooLarge,
            )?;
            let rows = rows_of(&body)?;
            scan.rows += rows.len();
            for row in &rows {
                self.take_row(row, floor, platform, &mut scan)?;
            }
            if rows.len() < PAGE_LEN {
                return Ok(scan);
            }
        }
        Err(ReleaseError::TooManyReleases)
    }

    /// `platform`'s newest new-line release above the installed version, when the feed has one.
    pub fn newest(
        &self,
        installed: &Version,
        platform: Platform,
        cancel: &Cancel,
    ) -> Result<Option<Release>, ReleaseError> {
        Ok(self.scan(Some(installed), platform, cancel)?.candidate)
    }

    /// Fetches `release`'s checksum list and, when the keyring holds a key, the list's signature,
    /// each held to the size and digest the feed published for it; checks the signature against
    /// the keyring, for the list's own name and the release's version; then reads the list, which
    /// must give the launcher file GitHub's digest, so that the launcher file downloaded after it
    /// is held to both. Nothing of the launcher file is fetched here. With a key, a release that
    /// carries no signature is [`ReleaseError::Incomplete`] before anything is asked; without
    /// one, a signature the release carries is not fetched.
    pub fn check_list(
        &self,
        release: &Release,
        cancel: &Cancel,
    ) -> Result<CheckedList, ReleaseError> {
        let signature = if self.keyring.is_empty() {
            None
        } else {
            let asset = release
                .signature
                .as_ref()
                .ok_or_else(|| ReleaseError::Incomplete {
                    asset: signature_asset(&release.version, release.platform).into(),
                })?;
            Some(asset)
        };
        let bytes = self.fetch_small(&release.checksums, cancel)?;
        let signature = signature
            .map(|asset| self.fetch_small(asset, cancel))
            .transpose()?;
        if let Some(signature) = &signature {
            // The list is read only once a trusted key has vouched for its bytes.
            self.keyring.verify_list(
                &bytes,
                &release.checksums.name,
                &release.version,
                signature,
            )?;
        }
        let list = ChecksumList::parse(&bytes)?;
        if list.digest_of(&release.launcher.name) != Some(release.launcher.sha256) {
            return Err(ReleaseError::ChecksumMismatch {
                asset: release.launcher.name.as_str().into(),
            });
        }
        Ok(CheckedList {
            list,
            bytes,
            signature,
        })
    }

    /// Judges one row of the list: a published release above `floor` whose assets include
    /// `platform`'s launcher file is a candidate, kept when it is the newest so far; one whose
    /// assets do not is counted as passed over; any other row is left as no release.
    fn take_row(
        &self,
        row: &Value,
        floor: Option<&Version>,
        platform: Platform,
        scan: &mut Scan,
    ) -> Result<(), ReleaseError> {
        let Some((row, tag, version)) = published(row, floor) else {
            return Ok(());
        };
        let assets = match row.get(ASSETS) {
            None => &[][..],
            Some(Value::Array(assets)) => assets.as_slice(),
            Some(_) => return Err(ReleaseError::AssetList { tag: tag.into() }),
        };
        let launcher_name = launcher_asset(&version, platform);
        let Some(launcher) = self.asset(assets, tag, &launcher_name, MAX_ASSET_LEN)? else {
            scan.passed_over += 1;
            return Ok(());
        };
        let list_name = checksums_asset(&version, platform);
        let checksums = self
            .asset(assets, tag, &list_name, MAX_LIST_LEN)?
            .ok_or_else(|| ReleaseError::Incomplete {
                asset: list_name.as_str().into(),
            })?;
        let signature_name = signature_asset(&version, platform);
        let signature = self.asset(assets, tag, &signature_name, MAX_LIST_LEN)?;
        if signature.is_none() && !self.keyring.is_empty() {
            return Err(ReleaseError::Incomplete {
                asset: signature_name.into(),
            });
        }
        let notes = row
            .get(BODY)
            .and_then(Value::as_str)
            .filter(|notes| notes.len() <= MAX_NOTES_LEN)
            .map(str::to_owned);
        let release = Release {
            version,
            tag: tag.to_owned(),
            platform,
            launcher,
            checksums,
            signature,
            notes,
        };
        let newest = scan
            .candidate
            .as_ref()
            .is_none_or(|kept| is_newer(&release.version, &kept.version));
        if newest {
            scan.candidate = Some(release);
        }
        Ok(())
    }

    /// The asset named exactly `name` among `assets`, held to the rules: `None` when there is
    /// none, [`ReleaseError::Ambiguous`] when there are more, and [`ReleaseError::Incomplete`]
    /// unless it is uploaded whole, of a whole number of bytes from 1 to `most`, with GitHub's
    /// SHA-256 digest, at its own download URL.
    fn asset(
        &self,
        assets: &[Value],
        tag: &str,
        name: &str,
        most: u64,
    ) -> Result<Option<Asset>, ReleaseError> {
        let mut named = assets
            .iter()
            .filter_map(Value::as_object)
            .filter(|asset| asset.get(NAME).and_then(Value::as_str) == Some(name));
        let Some(asset) = named.next() else {
            return Ok(None);
        };
        if named.next().is_some() {
            return Err(ReleaseError::Ambiguous { asset: name.into() });
        }
        let incomplete = || ReleaseError::Incomplete { asset: name.into() };
        if asset.get(STATE).and_then(Value::as_str) != Some(UPLOADED) {
            return Err(incomplete());
        }
        let size = asset
            .get(SIZE)
            .and_then(Value::as_u64)
            .filter(|size| (1..=most).contains(size))
            .ok_or_else(incomplete)?;
        let sha256 = asset
            .get(DIGEST)
            .and_then(Value::as_str)
            .and_then(|digest| digest.strip_prefix(DIGEST_PREFIX))
            .and_then(Sha256Digest::from_hex)
            .ok_or_else(incomplete)?;
        let url = asset
            .get(DOWNLOAD_URL)
            .and_then(Value::as_str)
            .filter(|url| self.config.downloads_from(url, tag, name))
            .ok_or_else(incomplete)?;
        Ok(Some(Asset {
            name: name.to_owned(),
            url: url.to_owned(),
            size,
            sha256,
        }))
    }
}

/// The rows a page of the list holds: its JSON array.
fn rows_of(page: &[u8]) -> Result<Vec<Value>, ReleaseError> {
    match serde_json::from_slice(page) {
        Ok(Value::Array(rows)) => Ok(rows),
        Ok(_) => Err(ReleaseError::InvalidResponse {
            detail: "a page of the releases list is not a JSON array".into(),
        }),
        Err(error) => Err(ReleaseError::InvalidResponse {
            detail: error.to_string().into(),
        }),
    }
}

/// The row, tag and version of `row` when it is a published release above `floor`: an object,
/// not a draft, whose tag is a `v` and a semantic version. Any other row is no release.
fn published<'a>(
    row: &'a Value,
    floor: Option<&Version>,
) -> Option<(&'a Map<String, Value>, &'a str, Version)> {
    let row = row.as_object()?;
    if row.get(DRAFT) != Some(&Value::Bool(false)) {
        return None;
    }
    let tag = row.get(TAG_NAME)?.as_str()?;
    if !tag.starts_with(TAG_PREFIX) {
        return None;
    }
    let version = Version::parse_tag(tag).ok()?;
    if floor.is_some_and(|floor| version <= *floor) {
        return None;
    }
    Some((row, tag, version))
}

/// Whether `version` is newer than `kept`: by precedence, then, between versions of equal
/// precedence, by their build metadata, so that the list's order never decides.
fn is_newer(version: &Version, kept: &Version) -> bool {
    version
        .cmp(kept)
        .then_with(|| version.semver().cmp(kept.semver()))
        == Ordering::Greater
}

#[cfg(test)]
mod tests {
    use std::fs;

    use gdl_files::{Cancel, Job, NoProgress};
    use gdl_fixtures::{
        AssetRow, LoopbackServer, ReleaseRow, Reply, sign_with_test_key, test_public_key,
    };
    use gdl_media::Sha256Digest;
    use serde_json::{Value, json};

    use super::{
        Asset, Feed, FeedConfig, MAX_ASSET_LEN, MAX_NOTES_LEN, MAX_PAGE_BYTES, MAX_PAGES, Origins,
        PAGE_LEN, Release, ReleaseFiles, Scan,
    };
    use crate::{
        DOWNLOAD_ATTEMPTS, Keyring, MAX_LIST_LEN, Platform, ProxySettings, ReleaseError, Version,
        checksums_asset, launcher_asset, licences_asset, signature_asset,
    };

    /// The launcher version the tests' feeds are read as.
    const RUNNING: &str = "1.0.0-beta";

    /// Windows' name as the release rows give it.
    const WINDOWS: &str = "windows-x64";

    /// Linux's name as the release rows give it.
    const LINUX: &str = "linux-x64";

    /// What every test release's launcher file holds.
    const LAUNCHER: &[u8] = b"a launcher file";

    /// What a test release's signature holds where no key checks it: no minisign signature at
    /// all, which a scan takes as it takes any asset.
    const SIGNATURE: &[u8] = b"untrusted comment: a signature\n";

    /// The release whose checksum list the tests check.
    const RELEASE: &str = "1.0.0-beta.1";

    /// What the release's licences hold, as its checksum list gives their digest.
    const LICENCES: &[u8] = b"the licences";

    /// When the tests' signatures say they were made, as the release signing writes it.
    const SIGNED_AT: &str = "timestamp:1764003600";

    /// The ten newest alpha releases, as GitHub's first page of the list gives them.
    const ALPHA_VERSIONS: [&str; 10] = [
        "4.0.0-alpha.3",
        "3.0.0-alpha.3",
        "2.2.0-alpha.3",
        "2.1.1-alpha.3",
        "2.1.0-alpha.3",
        "2.0.1-alpha.3",
        "2.0.0-alpha.3",
        "1.4.3-alpha.3",
        "1.4.2-alpha.3",
        "1.4.1-alpha.3",
    ];

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The target of page `page` of the list, as the feed asks for it.
    fn page_target(page: u32) -> String {
        format!(
            "/repos/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases?per_page=100&page={page}"
        )
    }

    /// The feed `server` serves, read as the running launcher.
    fn feed(server: &LoopbackServer) -> Feed {
        Feed::new(FeedConfig::loopback(&server.base(), RUNNING).expect("a loopback base"))
    }

    /// Serves `rows` as page `page` of the list.
    fn serve(server: &LoopbackServer, page: u32, rows: Vec<Value>) {
        let body = serde_json::to_vec(&Value::Array(rows)).expect("rows have a JSON form");
        server.route(&page_target(page), Reply::ok(body));
    }

    /// A new-line release of `version` for `platform`, served from `server`, unsigned.
    fn row(server: &LoopbackServer, version: &str, platform: &str) -> ReleaseRow {
        ReleaseRow::new_line(&server.base(), version, platform, LAUNCHER, None)
    }

    /// `feed`'s scan for `platform` above `floor`.
    fn scan(feed: &Feed, floor: Option<&str>, platform: Platform) -> Result<Scan, ReleaseError> {
        let floor = floor.map(version);
        feed.scan(floor.as_ref(), platform, &Cancel::default())
    }

    /// The version text of the candidate `feed` finds for Windows above `floor`.
    fn newest(feed: &Feed, floor: &str) -> Option<String> {
        scan(feed, Some(floor), Platform::WindowsX64)
            .unwrap_or_else(|error| panic!("{error}"))
            .candidate
            .map(|release| release.version.to_string())
    }

    /// The asset a scan refused as incomplete, or a panic naming what came instead.
    fn incomplete(result: Result<Scan, ReleaseError>) -> String {
        match result {
            Err(ReleaseError::Incomplete { asset }) => asset.into(),
            other => panic!("not refused as incomplete: {other:?}"),
        }
    }

    /// The feed `server` serves, read as the running launcher and trusting the test key alone.
    fn keyed_feed(server: &LoopbackServer) -> Feed {
        let config = FeedConfig::loopback(&server.base(), RUNNING).expect("a loopback base");
        let keyring = Keyring::of(&[&test_public_key()]).expect("the test key is a key");
        Feed::with_keyring(config, keyring)
    }

    /// A line of a checksum list giving `name` the digest of `bytes`, as release.py writes one.
    fn line(bytes: &[u8], name: &str) -> String {
        format!("{}  {name}\n", Sha256Digest::of(bytes))
    }

    /// The checksum list release.py writes of [`RELEASE`]'s Windows files: its launcher file's
    /// digest and its licences'.
    fn sound_list() -> Vec<u8> {
        let release = version(RELEASE);
        [
            line(LAUNCHER, &launcher_asset(&release, Platform::WindowsX64)),
            line(LICENCES, &licences_asset(&release, Platform::WindowsX64)),
        ]
        .concat()
        .into_bytes()
    }

    /// `list` with its first line's two spaces made one, a form release.py never writes.
    fn malformed(list: &[u8]) -> Vec<u8> {
        let text = std::str::from_utf8(list).expect("a test list is text");
        text.replacen("  ", " ", 1).into_bytes()
    }

    /// The test key's signature of `list`, whose trusted comment names the file `name` and the
    /// release `release`, as the release signing writes one.
    fn signed(list: &[u8], name: &str, release: &str) -> Vec<u8> {
        let comment = format!("{SIGNED_AT}\tfile:{name}\tversion:{release}");
        sign_with_test_key(list, &comment).into_bytes()
    }

    /// The request target that `url`, a URL of `server`'s, names.
    fn target_of(server: &LoopbackServer, url: &str) -> String {
        let base = server.base();
        url.strip_prefix(base.trim_end_matches('/'))
            .unwrap_or_else(|| panic!("{url} is not served from {base}"))
            .to_owned()
    }

    /// Serves `bytes` as `asset` at the target its URL names.
    fn serve_asset(server: &LoopbackServer, asset: &Asset, bytes: &[u8]) {
        server.route(&target_of(server, &asset.url), Reply::ok(bytes));
    }

    /// The targets of the requests `server` read after the first `skipped`.
    fn targets_after(server: &LoopbackServer, skipped: usize) -> Vec<String> {
        server.requests()[skipped..]
            .iter()
            .map(|request| request.target.clone())
            .collect()
    }

    /// The Windows release [`RELEASE`] as `server`'s list gives it, read by a feed that trusts no
    /// key: its launcher file [`LAUNCHER`], its checksum list published as holding `list` and,
    /// given one, the list's signature published as holding `signature`. Only the launcher file
    /// is served.
    fn published(server: &LoopbackServer, list: &[u8], signature: Option<&[u8]>) -> Release {
        let mut row = ReleaseRow::new_line(&server.base(), RELEASE, WINDOWS, LAUNCHER, signature);
        row.assets[1].size = json!(list.len());
        row.assets[1].digest = json!(format!("sha256:{}", Sha256Digest::of(list)));
        serve(server, 1, vec![row.to_json()]);
        let release = scan(&feed(server), Some(RUNNING), Platform::WindowsX64)
            .unwrap_or_else(|error| panic!("{error}"))
            .candidate
            .expect("the release published");
        serve_asset(server, &release.launcher, LAUNCHER);
        release
    }

    /// [`published`]'s release, its checksum list and signature served as they were published.
    fn served(server: &LoopbackServer, list: &[u8], signature: Option<&[u8]>) -> Release {
        let release = published(server, list, signature);
        serve_asset(server, &release.checksums, list);
        if let (Some(asset), Some(bytes)) = (&release.signature, signature) {
            serve_asset(server, asset, bytes);
        }
        release
    }

    /// Why `feed` refused `release`'s checksum list, or a panic naming `what` was taken.
    fn refused_list(feed: &Feed, release: &Release, what: &str) -> ReleaseError {
        match feed.check_list(release, &Cancel::default()) {
            Err(error) => error,
            Ok(checked) => panic!("{what} was taken: {checked:?}"),
        }
    }

    #[test]
    fn drafts_other_platforms_old_versions_and_bad_tags_are_passed_over() {
        let server = LoopbackServer::start().unwrap();
        let mut draft = row(&server, "1.1.0", WINDOWS);
        draft.draft = true;
        let mut untagged = row(&server, "1.0.0-beta.22", WINDOWS);
        untagged.tag = "1.0.0-beta.22".to_owned();
        let rows = vec![
            row(&server, "1.0.0-beta.9", WINDOWS).to_json(),
            row(&server, "1.0.0-beta.11", LINUX).to_json(),
            draft.to_json(),
            row(&server, "1.0.0-beta.10", WINDOWS).to_json(),
            json!({"tag_name": "main", "draft": false}),
            json!({"tag_name": null, "draft": false}),
            json!({"tag_name": "v1.0.0-beta.99", "draft": false, "assets": []}),
        ];
        serve(&server, 1, rows.clone());
        let feed = feed(&server);
        let found = scan(&feed, Some("1.0.0-beta"), Platform::WindowsX64).unwrap();
        let release = found.candidate.expect("a release for Windows");
        assert_eq!(release.version.to_string(), "1.0.0-beta.10");
        assert_eq!(release.tag, "v1.0.0-beta.10");
        assert_eq!(release.platform, Platform::WindowsX64);
        // The Linux release and the one without assets were passed over; the draft and the rows
        // that name no release were not releases at all.
        assert_eq!((found.rows, found.passed_over), (7, 2));
        let found = scan(&feed, Some("1.0.0-beta.10"), Platform::WindowsX64).unwrap();
        assert_eq!(found.candidate, None);
        assert_eq!((found.rows, found.passed_over), (7, 2));
        // For Linux, the Linux release is the newest, and the Windows ones are passed over.
        let found = scan(&feed, Some("1.0.0-beta"), Platform::LinuxX64).unwrap();
        let release = found.candidate.expect("a release for Linux");
        assert_eq!(release.version.to_string(), "1.0.0-beta.11");
        assert_eq!(release.platform, Platform::LinuxX64);
        assert_eq!(found.passed_over, 3);

        // Rows that are not published releases, whatever they carry, are no releases either.
        let mut more = rows;
        more.extend([
            json!(7),
            json!(null),
            json!({"tag_name": "v1.0.0-beta.20", "assets": []}),
            json!({"tag_name": "v1.0.0-beta.21", "draft": "false", "assets": []}),
            json!({"tag_name": "V1.0.0-beta.23", "draft": false, "assets": []}),
            json!({"tag_name": "vv1.0.0-beta.24", "draft": false, "assets": []}),
            json!({"tag_name": "v1.0.0-beta.025", "draft": false, "assets": []}),
            untagged.to_json(),
        ]);
        serve(&server, 1, more);
        let found = scan(&feed, Some("1.0.0-beta"), Platform::WindowsX64).unwrap();
        assert_eq!(
            found.candidate.map(|release| release.version.to_string()),
            Some("1.0.0-beta.10".to_owned())
        );
        assert_eq!((found.rows, found.passed_over), (15, 2));
    }

    #[test]
    fn releases_follow_semver_not_feed_order() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let versions = [
            "1.0.0-beta.2",
            "1.0.0-beta.10",
            "1.0.0-rc.1",
            "1.0.0",
            "1.0.1",
            "1.1.0-preview.1",
        ];
        for (index, newest_version) in versions.iter().enumerate() {
            let rows = versions[..=index]
                .iter()
                .rev()
                .map(|text| row(&server, text, WINDOWS).to_json())
                .collect();
            serve(&server, 1, rows);
            assert_eq!(
                newest(&feed, RUNNING).as_deref(),
                Some(*newest_version),
                "{versions:?} up to {newest_version}"
            );
            assert_eq!(newest(&feed, newest_version), None, "{newest_version}");
        }
        let mut draft = row(&server, "9.0.0", WINDOWS);
        draft.draft = true;
        serve(&server, 1, vec![draft.to_json()]);
        assert_eq!(newest(&feed, "1.0.0"), None);
    }

    #[test]
    fn a_plus_escaped_url_works_and_metadata_alone_is_no_upgrade() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let escaped = row(&server, "1.0.0+build.1", WINDOWS);
        assert!(escaped.assets[0].url.as_str().unwrap().contains("%2B"));
        serve(&server, 1, vec![escaped.to_json()]);
        let found = scan(&feed, Some("1.0.0-rc.1"), Platform::WindowsX64).unwrap();
        let release = found.candidate.expect("the release with build metadata");
        assert_eq!(release.version.to_string(), "1.0.0+build.1");
        assert_eq!(release.tag, "v1.0.0+build.1");
        assert_eq!(
            release.launcher.name,
            "GauntletDarkLegacy-1.0.0+build.1-windows-x64-launcher.exe"
        );
        assert_eq!(release.launcher.url, escaped.assets[0].url);
        assert_eq!(newest(&feed, "1.0.0+build.0"), None);

        // A URL that writes the `+` as it is names the same file.
        let mut plain = escaped.clone();
        for asset in &mut plain.assets {
            let url = asset.url.as_str().unwrap().replace("%2B", "+");
            asset.url = json!(url);
        }
        serve(&server, 1, vec![plain.to_json()]);
        assert_eq!(
            newest(&feed, "1.0.0-rc.1").as_deref(),
            Some("1.0.0+build.1")
        );
    }

    #[test]
    fn unverified_assets_are_refused() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let sound = row(&server, "1.0.0-beta.1", WINDOWS);
        let base = server.base();
        let port = base
            .trim_end_matches('/')
            .rsplit_once(':')
            .unwrap()
            .1
            .to_owned();
        let other_port = (port.parse::<u16>().unwrap() ^ 1).to_string();

        // A change to an asset, given the bound on its size.
        type Edit = Box<dyn Fn(&mut AssetRow, u64)>;
        fn edit(change: impl Fn(&mut AssetRow, u64) + 'static) -> Edit {
            Box::new(change)
        }
        // A change to an asset's URL, given its own.
        fn url_edit(change: impl Fn(&str) -> String + 'static) -> Edit {
            edit(move |asset, _| {
                let changed = change(asset.url.as_str().expect("a test URL is text"));
                asset.url = json!(changed);
            })
        }
        let edits: Vec<(&str, Edit)> = vec![
            ("no digest", edit(|asset, _| asset.digest = Value::Null)),
            (
                "an upper-case digest",
                edit(|asset, _| {
                    let digest = asset.digest.as_str().unwrap().to_uppercase();
                    asset.digest = json!(digest.replace("SHA256:", "sha256:"));
                }),
            ),
            (
                "a digest without its prefix",
                edit(|asset, _| {
                    let digest = asset.digest.as_str().unwrap().replace("sha256:", "");
                    asset.digest = json!(digest);
                }),
            ),
            (
                "a SHA-1 digest",
                edit(|asset, _| asset.digest = json!(format!("sha1:{}", "0".repeat(40)))),
            ),
            (
                "a digest one digit short",
                edit(|asset, _| {
                    let digest = asset.digest.as_str().unwrap().to_owned();
                    asset.digest = json!(digest[..digest.len() - 1]);
                }),
            ),
            (
                "a digest of a number",
                edit(|asset, _| asset.digest = json!(7)),
            ),
            ("a negative size", edit(|asset, _| asset.size = json!(-1))),
            ("no bytes", edit(|asset, _| asset.size = json!(0))),
            (
                "a fractional size",
                edit(|asset, _| asset.size = json!(15.0)),
            ),
            ("a size of text", edit(|asset, _| asset.size = json!("15"))),
            ("a size of truth", edit(|asset, _| asset.size = json!(true))),
            ("no size", edit(|asset, _| asset.size = Value::Null)),
            (
                "a size past the bound",
                edit(|asset, most| asset.size = json!(most + 1)),
            ),
            (
                "an upload under way",
                edit(|asset, _| asset.state = "new".to_owned()),
            ),
            ("no state", edit(|asset, _| asset.state = String::new())),
            (
                "another host",
                url_edit(|_| "https://evil.example/payload.zip".to_owned()),
            ),
            ("a query", url_edit(|own| format!("{own}?x=1"))),
            ("an empty query", url_edit(|own| format!("{own}?"))),
            ("a fragment", url_edit(|own| format!("{own}#f"))),
            (
                "another tag's path",
                url_edit(|own| own.replace("/v1.0.0-beta.1/", "/v1.0.0-beta.2/")),
            ),
            (
                "HTTPS to a loopback feed",
                url_edit(|own| own.replace("http://", "https://")),
            ),
            (
                "another port",
                url_edit(move |own| {
                    own.replacen(&format!(":{port}/"), &format!(":{other_port}/"), 1)
                }),
            ),
            (
                "a malformed escape",
                url_edit(|own| own.replace("/releases/", "/rele%zzases/")),
            ),
            ("no URL", edit(|asset, _| asset.url = Value::Null)),
            (
                "another asset's URL",
                url_edit(|own| {
                    let (folder, _) = own.rsplit_once('/').unwrap();
                    format!("{folder}/GauntletDarkLegacy-other")
                }),
            ),
        ];
        // The launcher file and the checksum list are each held to every rule.
        for (index, most) in [(0, MAX_ASSET_LEN), (1, MAX_LIST_LEN)] {
            let name = sound.assets[index].name.clone();
            for (what, edit) in &edits {
                let mut edited = sound.clone();
                edit(&mut edited.assets[index], most);
                serve(&server, 1, vec![edited.to_json()]);
                let refused = incomplete(scan(&feed, Some(RUNNING), Platform::WindowsX64));
                assert_eq!(refused, name, "{what} of {name}");
            }
        }

        // A size at the bound, and a path whose escapes decode to the asset's own, are taken.
        for (index, most) in [(0, MAX_ASSET_LEN), (1, MAX_LIST_LEN)] {
            let mut edited = sound.clone();
            edited.assets[index].size = json!(most);
            let url = edited.assets[index]
                .url
                .as_str()
                .unwrap()
                .replace("/releases/", "/%72eleases/");
            edited.assets[index].url = json!(url);
            serve(&server, 1, vec![edited.to_json()]);
            let release = scan(&feed, Some(RUNNING), Platform::WindowsX64)
                .unwrap()
                .candidate
                .expect("an asset at its bounds");
            let asset = if index == 0 {
                release.launcher
            } else {
                release.checksums
            };
            assert_eq!(asset.size, most);
            assert_eq!(asset.url, url);
        }
    }

    #[test]
    fn two_assets_of_one_name_are_ambiguous() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let signed = ReleaseRow::new_line(
            &server.base(),
            "1.0.0-beta.1",
            WINDOWS,
            LAUNCHER,
            Some(SIGNATURE),
        );
        for index in 0..signed.assets.len() {
            let mut doubled = signed.clone();
            doubled.assets.push(signed.assets[index].clone());
            serve(&server, 1, vec![doubled.to_json()]);
            match scan(&feed, Some(RUNNING), Platform::WindowsX64) {
                Err(error @ ReleaseError::Ambiguous { .. }) => assert_eq!(
                    error.to_string(),
                    format!("Ambiguous release asset: {}", signed.assets[index].name)
                ),
                other => panic!("{} twice: {other:?}", signed.assets[index].name),
            }
        }
    }

    #[test]
    fn a_full_page_asks_for_the_next() {
        let server = LoopbackServer::start().unwrap();
        let old = vec![json!({"tag_name": "old", "draft": false}); PAGE_LEN];
        serve(&server, 1, old);
        serve(
            &server,
            2,
            vec![row(&server, "1.0.0-beta.10", WINDOWS).to_json()],
        );
        let found = scan(&feed(&server), Some(RUNNING), Platform::WindowsX64).unwrap();
        assert_eq!(
            found.candidate.map(|release| release.version.to_string()),
            Some("1.0.0-beta.10".to_owned())
        );
        assert_eq!((found.rows, found.passed_over), (PAGE_LEN + 1, 0));
        let targets: Vec<String> = server
            .requests()
            .into_iter()
            .map(|request| request.target)
            .collect();
        assert_eq!(targets, [page_target(1), page_target(2)]);
        assert!(targets[1].contains("page=2"));
    }

    #[test]
    fn a_page_that_is_no_array_is_refused() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let pages: [&[u8]; 6] = [b"{}", b"null", b"\"releases\"", b"not json", b"[", b""];
        for page in pages {
            server.route(&page_target(1), Reply::ok(page));
            assert!(
                matches!(
                    scan(&feed, Some(RUNNING), Platform::WindowsX64),
                    Err(ReleaseError::InvalidResponse { .. })
                ),
                "{:?}",
                String::from_utf8_lossy(page)
            );
        }
        // A page of the most bytes is read; one byte more is refused before it is parsed.
        let bound = usize::try_from(MAX_PAGE_BYTES).unwrap();
        let padded = |len: usize| {
            let mut page = vec![b' '; len - 2];
            page.extend_from_slice(b"[]");
            page
        };
        server.route(&page_target(1), Reply::ok(padded(bound)));
        let found = scan(&feed, Some(RUNNING), Platform::WindowsX64).unwrap();
        assert_eq!((found.candidate, found.rows), (None, 0));
        server.route(&page_target(1), Reply::ok(padded(bound + 1)));
        assert!(matches!(
            scan(&feed, Some(RUNNING), Platform::WindowsX64),
            Err(ReleaseError::ResponseTooLarge)
        ));
    }

    #[test]
    fn twenty_full_pages_are_too_many() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let old = vec![json!({"tag_name": "old", "draft": false}); PAGE_LEN];
        for page in 1..MAX_PAGES {
            serve(&server, page, old.clone());
        }
        // Nineteen full pages and a shorter one are read whole.
        serve(&server, MAX_PAGES, old[1..].to_vec());
        let found = scan(&feed, Some(RUNNING), Platform::WindowsX64).unwrap();
        assert_eq!(found.rows, MAX_PAGES as usize * PAGE_LEN - 1);
        assert_eq!(server.requests().len(), MAX_PAGES as usize);

        // A twentieth full page is refused, and no page after it is asked for.
        serve(&server, MAX_PAGES, old);
        assert!(matches!(
            scan(&feed, Some(RUNNING), Platform::WindowsX64),
            Err(ReleaseError::TooManyReleases)
        ));
        assert_eq!(server.requests().len(), 2 * MAX_PAGES as usize);
        assert!(
            server
                .requests()
                .iter()
                .all(|request| !request.target.ends_with(&format!("page={}", MAX_PAGES + 1)))
        );
    }

    #[test]
    fn a_feed_of_alpha_releases_alone_offers_nothing() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let rows = ALPHA_VERSIONS
            .iter()
            .map(|text| ReleaseRow::alpha(text).to_json())
            .collect();
        serve(&server, 1, rows);
        for platform in Platform::ALL {
            for floor in [Some(RUNNING), None] {
                let found = scan(&feed, floor, platform).unwrap_or_else(|error| panic!("{error}"));
                assert_eq!(
                    found,
                    Scan {
                        candidate: None,
                        rows: 10,
                        passed_over: 10,
                    },
                    "{platform:?} above {floor:?}"
                );
            }
        }
    }

    #[test]
    fn a_release_without_this_platforms_launcher_is_passed_over() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let linux_only = row(&server, "1.0.0-beta.2", LINUX);
        let mut list_only = row(&server, "1.0.0-beta.3", WINDOWS);
        list_only.assets.remove(0);
        let rows = vec![
            linux_only.to_json(),
            list_only.to_json(),
            json!({"tag_name": "v1.0.0-beta.4", "draft": false}),
        ];
        serve(&server, 1, rows);
        let found = scan(&feed, Some(RUNNING), Platform::WindowsX64).unwrap();
        assert_eq!(
            found,
            Scan {
                candidate: None,
                rows: 3,
                passed_over: 3,
            }
        );

        // A launcher file without its checksum list is a broken release, not one passed over.
        let mut no_list = row(&server, "1.0.0-beta.5", WINDOWS);
        no_list.assets.truncate(1);
        serve(&server, 1, vec![no_list.to_json()]);
        let refused = incomplete(scan(&feed, Some(RUNNING), Platform::WindowsX64));
        assert_eq!(
            refused,
            checksums_asset(&version("1.0.0-beta.5"), Platform::WindowsX64)
        );
    }

    #[test]
    fn a_candidate_below_the_newest_is_held_to_the_rules() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let mut broken = row(&server, "1.0.0-beta.2", WINDOWS);
        broken.assets[0].digest = Value::Null;
        let sound = row(&server, "1.0.0-beta.3", WINDOWS);
        for rows in [
            vec![sound.to_json(), broken.to_json()],
            vec![broken.to_json(), sound.to_json()],
        ] {
            serve(&server, 1, rows);
            let refused = incomplete(scan(&feed, Some(RUNNING), Platform::WindowsX64));
            assert_eq!(refused, broken.assets[0].name);
        }
        // At or below the floor, a broken release is no candidate and not held to the rules.
        serve(&server, 1, vec![sound.to_json(), broken.to_json()]);
        assert_eq!(
            newest(&feed, "1.0.0-beta.2").as_deref(),
            Some("1.0.0-beta.3")
        );
    }

    #[test]
    fn a_release_still_uploading_fails_the_check() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let sound = row(&server, "1.0.0-beta.1", WINDOWS);
        let uploading = ReleaseRow::new_line(
            &server.base(),
            "1.0.0-beta.2",
            WINDOWS,
            LAUNCHER,
            Some(SIGNATURE),
        );
        // Each of the newest release's files still being uploaded fails the check, rather than
        // falling back to the release before it, its signature's too where no key asks for one.
        for index in 0..uploading.assets.len() {
            let mut partial = uploading.clone();
            partial.assets[index].state = "new".to_owned();
            serve(&server, 1, vec![partial.to_json(), sound.to_json()]);
            let refused = incomplete(scan(&feed, Some(RUNNING), Platform::WindowsX64));
            assert_eq!(refused, uploading.assets[index].name);
        }
        serve(&server, 1, vec![uploading.to_json(), sound.to_json()]);
        assert_eq!(newest(&feed, RUNNING).as_deref(), Some("1.0.0-beta.2"));
    }

    #[test]
    fn equal_precedence_is_decided_by_build_metadata() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let first = row(&server, "1.0.0+build.1", WINDOWS).to_json();
        let second = row(&server, "1.0.0+build.2", WINDOWS).to_json();
        for rows in [
            vec![first.clone(), second.clone()],
            vec![second.clone(), first.clone()],
        ] {
            serve(&server, 1, rows);
            let release = scan(&feed, Some("1.0.0-rc.1"), Platform::WindowsX64)
                .unwrap()
                .candidate
                .expect("a release of 1.0.0");
            assert_eq!(release.version.to_string(), "1.0.0+build.2");
            assert_eq!(release.tag, "v1.0.0+build.2");
        }
    }

    #[test]
    fn a_keyring_with_a_key_needs_the_signature() {
        let server = LoopbackServer::start().unwrap();
        let config = FeedConfig::loopback(&server.base(), RUNNING).unwrap();
        let keyring = Keyring::of(&[&test_public_key()]).unwrap();
        let keyed = Feed::with_keyring(config, keyring);
        let plain = feed(&server);
        assert!(!keyed.keyring().is_empty());
        assert!(plain.keyring().is_empty(), "no release key is built in yet");

        let release = version("1.0.0-beta.1");
        let unsigned = row(&server, "1.0.0-beta.1", WINDOWS);
        serve(&server, 1, vec![unsigned.to_json()]);
        let refused = incomplete(scan(&keyed, Some(RUNNING), Platform::WindowsX64));
        assert_eq!(refused, signature_asset(&release, Platform::WindowsX64));
        let found = scan(&plain, Some(RUNNING), Platform::WindowsX64).unwrap();
        assert_eq!(
            found.candidate.expect("an unsigned release").signature,
            None
        );

        let signed = ReleaseRow::new_line(
            &server.base(),
            "1.0.0-beta.1",
            WINDOWS,
            LAUNCHER,
            Some(SIGNATURE),
        );
        serve(&server, 1, vec![signed.to_json()]);
        for reader in [&keyed, &plain] {
            let found = reader
                .scan(
                    Some(&version(RUNNING)),
                    Platform::WindowsX64,
                    &Cancel::default(),
                )
                .unwrap();
            let signature = found
                .candidate
                .and_then(|release| release.signature)
                .expect("the release's signature");
            assert_eq!(signature.name, signed.assets[2].name);
            assert_eq!(signature.size, SIGNATURE.len() as u64);
            assert_eq!(signature.url, signed.assets[2].url);
        }
    }

    #[test]
    fn notes_longer_than_their_bound_are_dropped() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let longest = "n".repeat(MAX_NOTES_LEN);
        let notes = [
            (
                json!("## Added\n- Updates\n"),
                Some("## Added\n- Updates\n"),
            ),
            (json!(longest), Some(longest.as_str())),
            (json!(format!("{longest}n")), None),
            (Value::Null, None),
            (json!(7), None),
            (json!(["## Added"]), None),
        ];
        for (body, kept) in notes {
            let mut release = row(&server, "1.0.0-beta.1", WINDOWS).to_json();
            release["body"] = body;
            serve(&server, 1, vec![release]);
            let found = scan(&feed, Some(RUNNING), Platform::WindowsX64).unwrap();
            let release = found.candidate.expect("the release");
            assert_eq!(release.notes.as_deref(), kept);
        }
    }

    #[test]
    fn assets_that_are_no_list_are_refused() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        for assets in [json!({}), json!("assets"), json!(null)] {
            let mut release = row(&server, "1.0.0-beta.1", WINDOWS).to_json();
            release["assets"] = assets.clone();
            serve(&server, 1, vec![release]);
            match scan(&feed, Some(RUNNING), Platform::WindowsX64) {
                Err(error @ ReleaseError::AssetList { .. }) => assert_eq!(
                    error.to_string(),
                    "Invalid GitHub release asset list: v1.0.0-beta.1"
                ),
                other => panic!("assets of {assets}: {other:?}"),
            }
        }
        // Entries of the list that are no assets are not looked at.
        let mut release = row(&server, "1.0.0-beta.1", WINDOWS).to_json();
        let assets = release["assets"].as_array_mut().unwrap();
        assets.insert(0, json!(7));
        assets.insert(1, json!({"name": null}));
        assets.push(json!(["a", "list"]));
        serve(&server, 1, vec![release]);
        assert_eq!(newest(&feed, RUNNING).as_deref(), Some("1.0.0-beta.1"));
    }

    #[test]
    fn a_release_holds_what_its_row_gives() {
        let server = LoopbackServer::start().unwrap();
        let mut signed = ReleaseRow::new_line(
            &server.base(),
            "1.0.0-beta.1",
            LINUX,
            LAUNCHER,
            Some(SIGNATURE),
        );
        signed.body = Some("## Fixed\n".to_owned());
        serve(&server, 1, vec![signed.to_json()]);
        let release = feed(&server)
            .newest(&version(RUNNING), Platform::LinuxX64, &Cancel::default())
            .unwrap()
            .expect("the release");
        let text = version("1.0.0-beta.1");
        assert_eq!(release.tag, "v1.0.0-beta.1");
        assert_eq!(release.platform, Platform::LinuxX64);
        assert_eq!(release.notes.as_deref(), Some("## Fixed\n"));
        let names = [
            launcher_asset(&text, Platform::LinuxX64),
            checksums_asset(&text, Platform::LinuxX64),
            signature_asset(&text, Platform::LinuxX64),
        ];
        let signature = release.signature.as_ref().expect("the signature");
        for ((asset, row), name) in [&release.launcher, &release.checksums, signature]
            .into_iter()
            .zip(&signed.assets)
            .zip(&names)
        {
            assert_eq!(&asset.name, name);
            assert_eq!(&row.name, name);
            assert_eq!(json!(asset.url), row.url);
            assert_eq!(json!(asset.size), row.size);
            assert_eq!(json!(format!("sha256:{}", asset.sha256)), row.digest);
        }
        assert_eq!(
            release.launcher.sha256,
            gdl_media::Sha256Digest::of(LAUNCHER)
        );
    }

    #[test]
    fn a_cancelled_scan_asks_nothing() {
        let server = LoopbackServer::start().unwrap();
        serve(
            &server,
            1,
            vec![row(&server, "1.0.0-beta.1", WINDOWS).to_json()],
        );
        let cancel = Cancel::default();
        cancel.cancel();
        let feed = feed(&server);
        assert!(matches!(
            feed.scan(None, Platform::WindowsX64, &cancel),
            Err(ReleaseError::Cancelled)
        ));
        assert!(server.requests().is_empty());
    }

    #[test]
    fn a_failing_page_fails_the_scan() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let old = vec![json!({"tag_name": "old", "draft": false}); PAGE_LEN];
        serve(&server, 1, old);
        server.route(
            &page_target(2),
            Reply::status(403)
                .with_header("X-RateLimit-Remaining", "0")
                .with_header("X-RateLimit-Reset", "1764003600"),
        );
        assert!(matches!(
            scan(&feed, Some(RUNNING), Platform::WindowsX64),
            Err(ReleaseError::RateLimited {
                reset_at: Some(1_764_003_600)
            })
        ));
        server.route(&page_target(1), Reply::status(404));
        assert!(matches!(
            scan(&feed, Some(RUNNING), Platform::WindowsX64),
            Err(ReleaseError::Status { status: 404, .. })
        ));
    }

    #[test]
    fn githubs_download_urls_are_held_to_githubs_own_form() {
        let config = FeedConfig::github(RUNNING, ProxySettings::none());
        assert_eq!(
            config.api,
            "https://api.github.com/repos/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases"
        );
        assert_eq!(config.origins, Origins::GitHub);
        assert_eq!(config.launcher_version, RUNNING);
        // Nothing is asked of GitHub here: the rows are judged as a page of its list would be.
        let github = Feed::new(config);
        let floor = version(RUNNING);
        let judged = |base: &str| -> Result<Option<Release>, ReleaseError> {
            let row = ReleaseRow::new_line(base, "1.0.0-beta.1", WINDOWS, LAUNCHER, None).to_json();
            let mut scan = Scan {
                candidate: None,
                rows: 1,
                passed_over: 0,
            };
            github.take_row(&row, Some(&floor), Platform::WindowsX64, &mut scan)?;
            Ok(scan.candidate)
        };
        let release = judged("https://github.com/")
            .unwrap()
            .expect("GitHub's release");
        assert!(release.launcher.url.starts_with(
            "https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/"
        ));
        for base in [
            "http://github.com/",
            "https://github.com:443/",
            "https://GitHub.com/",
            "https://api.github.com/",
            "https://objects.githubusercontent.com/",
            "https://github.com.evil.example/",
            "https://user@github.com/",
            "http://127.0.0.1:8080/",
        ] {
            assert!(
                matches!(judged(base), Err(ReleaseError::Incomplete { .. })),
                "{base}"
            );
        }
    }

    #[test]
    fn a_loopback_feed_takes_only_loopback_urls() {
        for (base, authority) in [
            ("http://127.0.0.1:8080/", "127.0.0.1:8080"),
            ("http://[::1]:8080/", "[::1]:8080"),
            ("http://127.0.0.1:65535/", "127.0.0.1:65535"),
            ("http://127.0.0.1:1/", "127.0.0.1:1"),
        ] {
            let config =
                FeedConfig::loopback(base, RUNNING).unwrap_or_else(|error| panic!("{error}"));
            assert_eq!(
                config.api,
                format!("{base}repos/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases")
            );
            assert_eq!(
                config.origins,
                Origins::Loopback {
                    authority: authority.to_owned()
                }
            );
            assert_eq!(config.proxies, ProxySettings::none());
        }
        for base in [
            "http://localhost:8080/",
            "https://127.0.0.1:8080/",
            "http://127.0.0.1/",
            "http://127.0.0.1:/",
            "http://127.0.0.1:8080",
            "http://127.0.0.1:8080/x/",
            "http://10.0.0.1:8080/",
            "http://127.0.0.2:8080/",
            "http://::1:8080/",
            "http://127.0.0.1:0/",
            "http://127.0.0.1:65536/",
            "http://127.0.0.1:08080/",
            "http://127.0.0.1:+8080/",
            "http://user@127.0.0.1:8080/",
            "HTTP://127.0.0.1:8080/",
            "",
        ] {
            match FeedConfig::loopback(base, RUNNING) {
                Err(error) => {
                    assert_eq!(&*error.given, base);
                    assert_eq!(
                        error.to_string(),
                        format!(
                            "A loopback feed is http://127.0.0.1:<port>/ or http://[::1]:<port>/, \
                             not {base:?}"
                        )
                    );
                }
                Ok(config) => panic!("{base:?} was taken as {config:?}"),
            }
        }
    }

    #[test]
    fn a_list_without_the_launcher_or_with_another_digest_is_refused() {
        let release_version = version(RELEASE);
        let launcher = launcher_asset(&release_version, Platform::WindowsX64);
        let licences_name = licences_asset(&release_version, Platform::WindowsX64);
        let licences = line(LICENCES, &licences_name);

        // A list that gives the launcher file GitHub's digest is taken whole, and nothing but the
        // list is asked for: the launcher file comes after it.
        let server = LoopbackServer::start().unwrap();
        let list = sound_list();
        let release = served(&server, &list, None);
        let asked = server.requests().len();
        let checked = feed(&server)
            .check_list(&release, &Cancel::default())
            .unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(
            checked.list.digest_of(&launcher),
            Some(Sha256Digest::of(LAUNCHER))
        );
        assert_eq!(
            checked.list.digest_of(&licences_name),
            Some(Sha256Digest::of(LICENCES))
        );
        assert_eq!(checked.bytes, list);
        assert_eq!(checked.signature, None);
        assert_eq!(
            targets_after(&server, asked),
            [target_of(&server, &release.checksums.url)]
        );

        let other_release = version("1.0.0-beta.2");
        let lists = [
            ("the licences alone", licences.clone()),
            (
                "another release's launcher file",
                line(
                    LAUNCHER,
                    &launcher_asset(&other_release, Platform::WindowsX64),
                ) + &licences,
            ),
            (
                "the other platform's launcher file",
                line(
                    LAUNCHER,
                    &launcher_asset(&release_version, Platform::LinuxX64),
                ) + &licences,
            ),
            (
                "the launcher file in other letters",
                line(LAUNCHER, &launcher.to_lowercase()) + &licences,
            ),
            (
                "another digest of the launcher file",
                line(b"another launcher file", &launcher) + &licences,
            ),
        ];
        for (what, list) in lists {
            let server = LoopbackServer::start().unwrap();
            let release = served(&server, list.as_bytes(), None);
            let error = refused_list(&feed(&server), &release, what);
            assert!(
                matches!(error, ReleaseError::ChecksumMismatch { .. }),
                "{what}: {error}"
            );
            assert_eq!(
                error.to_string(),
                format!("The release's checksum list does not give {launcher} GitHub's digest"),
                "{what}"
            );
        }
    }

    #[test]
    fn a_list_that_breaks_its_form_or_its_digest_is_refused() {
        let list = sound_list();
        let release_version = version(RELEASE);

        // A list that matches GitHub's digest but not release.py's form is refused as it is read.
        let server = LoopbackServer::start().unwrap();
        let release = served(&server, &malformed(&list), None);
        let error = refused_list(&feed(&server), &release, "a list of one space");
        assert!(
            matches!(error, ReleaseError::ChecksumList { .. }),
            "{error}"
        );
        assert_eq!(
            error.to_string(),
            "The release's checksum list is malformed: line 1 does not part its digest from its \
             name by two spaces"
        );

        // A list served other than it was published is refused after a second try.
        let server = LoopbackServer::start().unwrap();
        let release = published(&server, &list, None);
        let mut other = list.clone();
        other[0] ^= 1;
        serve_asset(&server, &release.checksums, &other);
        let asked = server.requests().len();
        let error = refused_list(&feed(&server), &release, "another list");
        assert_eq!(
            error.to_string(),
            format!(
                "Update download is incomplete or failed its SHA-256 check: {}",
                release.checksums.name
            )
        );
        assert_eq!(
            targets_after(&server, asked),
            vec![target_of(&server, &release.checksums.url); DOWNLOAD_ATTEMPTS as usize]
        );

        // A list that is not there is the status the feed answered.
        let server = LoopbackServer::start().unwrap();
        let release = published(&server, &list, None);
        let error = refused_list(&feed(&server), &release, "no list");
        assert!(
            matches!(error, ReleaseError::Status { status: 404, .. }),
            "{error}"
        );

        // So is a signature served other than it was published, when it is checked.
        let server = LoopbackServer::start().unwrap();
        let name = checksums_asset(&release_version, Platform::WindowsX64);
        let signature = signed(&list, &name, RELEASE);
        let release = published(&server, &list, Some(&signature));
        serve_asset(&server, &release.checksums, &list);
        let mut other = signature.clone();
        other[0] ^= 1;
        let signature_file = release.signature.as_ref().expect("the release's signature");
        serve_asset(&server, signature_file, &other);
        let error = refused_list(&keyed_feed(&server), &release, "another signature");
        assert_eq!(
            error.to_string(),
            format!(
                "Update download is incomplete or failed its SHA-256 check: {}",
                signature_file.name
            )
        );
    }

    #[test]
    fn an_unsigned_release_is_refused_by_a_keyring_with_a_key() {
        let list = sound_list();
        let release_version = version(RELEASE);

        // Without a signature to check, nothing is asked.
        let server = LoopbackServer::start().unwrap();
        let release = served(&server, &list, None);
        assert_eq!(release.signature, None);
        let asked = server.requests().len();
        match refused_list(&keyed_feed(&server), &release, "an unsigned list") {
            ReleaseError::Incomplete { asset } => assert_eq!(
                &*asset,
                signature_asset(&release_version, Platform::WindowsX64)
            ),
            other => panic!("an unsigned list: {other}"),
        }
        assert_eq!(server.requests().len(), asked);

        // Signed by the test key for its own name and release, the list is taken, and its
        // signature is fetched after it.
        let server = LoopbackServer::start().unwrap();
        let name = checksums_asset(&release_version, Platform::WindowsX64);
        let signature = signed(&list, &name, RELEASE);
        let release = served(&server, &list, Some(&signature));
        let asked = server.requests().len();
        let checked = keyed_feed(&server)
            .check_list(&release, &Cancel::default())
            .unwrap_or_else(|error| panic!("{error}"));
        assert!(checked.signed());
        assert_eq!(checked.signature.as_deref(), Some(signature.as_slice()));
        assert_eq!(checked.bytes, list);
        assert_eq!(
            checked.list.digest_of(&release.launcher.name),
            Some(release.launcher.sha256)
        );
        let signature_url = &release.signature.as_ref().expect("the signature").url;
        assert_eq!(
            targets_after(&server, asked),
            [
                target_of(&server, &release.checksums.url),
                target_of(&server, signature_url)
            ]
        );
    }

    #[test]
    fn a_list_is_held_to_its_signature_before_it_is_read() {
        let list = sound_list();
        let release_version = version(RELEASE);
        let name = checksums_asset(&release_version, Platform::WindowsX64);
        let linux_name = checksums_asset(&release_version, Platform::LinuxX64);
        let changed = [
            line(
                LAUNCHER,
                &launcher_asset(&release_version, Platform::WindowsX64),
            ),
            line(
                b"other licences",
                &licences_asset(&release_version, Platform::WindowsX64),
            ),
        ]
        .concat()
        .into_bytes();
        let malformed = malformed(&list);
        let another_release =
            "its trusted comment gives the version \"1.0.0-beta.2\", not 1.0.0-beta.1";
        let cases = [
            (
                "a list signed for another release",
                list.clone(),
                signed(&list, &name, "1.0.0-beta.2"),
                another_release.to_owned(),
            ),
            (
                "a list signed as the other platform's",
                list.clone(),
                signed(&list, &linux_name, RELEASE),
                format!("its trusted comment gives the file {linux_name:?}, not {name}"),
            ),
            (
                "a list changed after it was signed",
                changed,
                signed(&list, &name, RELEASE),
                "it does not verify over this list and its trusted comment".to_owned(),
            ),
            (
                "a malformed list signed for another release",
                malformed.clone(),
                signed(&malformed, &name, "1.0.0-beta.2"),
                another_release.to_owned(),
            ),
        ];
        for (what, served_list, signature, why) in cases {
            let server = LoopbackServer::start().unwrap();
            let release = served(&server, &served_list, Some(&signature));
            let error = refused_list(&keyed_feed(&server), &release, what);
            assert!(
                matches!(error, ReleaseError::Signature { .. }),
                "{what}: {error}"
            );
            assert!(error.to_string().ends_with(&why), "{what}: {error}");
        }

        // A malformed list its signature vouches for is read, and refused as it is read.
        let server = LoopbackServer::start().unwrap();
        let signature = signed(&malformed, &name, RELEASE);
        let release = served(&server, &malformed, Some(&signature));
        let error = refused_list(&keyed_feed(&server), &release, "a signed malformed list");
        assert!(
            matches!(error, ReleaseError::ChecksumList { .. }),
            "{error}"
        );
    }

    #[test]
    fn an_empty_keyring_checks_no_signature() {
        let server = LoopbackServer::start().unwrap();
        let list = sound_list();
        // The release carries a signature that is no minisign signature at all.
        let release = served(&server, &list, Some(SIGNATURE));
        assert!(release.signature.is_some());
        let plain = feed(&server);
        assert!(plain.keyring().is_empty());
        let asked = server.requests().len();
        let checked = plain
            .check_list(&release, &Cancel::default())
            .unwrap_or_else(|error| panic!("{error}"));
        assert!(!checked.signed());
        assert_eq!(checked.signature, None);
        assert_eq!(checked.bytes, list);
        assert_eq!(
            targets_after(&server, asked),
            [target_of(&server, &release.checksums.url)]
        );

        // A keyring with a key fetches the same signature and refuses it.
        let error = refused_list(&keyed_feed(&server), &release, "no minisign signature");
        assert!(matches!(error, ReleaseError::Signature { .. }), "{error}");
    }

    #[test]
    fn an_update_fetches_a_releases_files_through_the_feed() {
        let server = LoopbackServer::start().unwrap();
        let list = sound_list();
        let release = served(&server, &list, None);
        let feed = feed(&server);
        let files: &dyn ReleaseFiles = &feed;
        let cancel = Cancel::default();
        let checked = files
            .check_list(&release, &cancel)
            .unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(checked, feed.check_list(&release, &cancel).unwrap());

        let folder = tempfile::tempdir().unwrap();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let downloaded = files
            .download(&release.launcher, folder.path(), &job)
            .unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(downloaded.path, folder.path().join(&release.launcher.name));
        assert_eq!(fs::read(&downloaded.path).unwrap(), LAUNCHER);
        assert_eq!(
            checked.list.digest_of(&release.launcher.name),
            Some(downloaded.sha256)
        );

        // A cancelled job asks nothing.
        cancel.cancel();
        let asked = server.requests().len();
        assert!(matches!(
            files.check_list(&release, &cancel),
            Err(ReleaseError::Cancelled)
        ));
        assert_eq!(server.requests().len(), asked);
    }
}
