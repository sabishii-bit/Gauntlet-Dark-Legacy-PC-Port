//! A release's files fetched from the feed: each read a block at a time and held as it comes to the
//! size and the SHA-256 digest the feed published for it, the job's cancel looked at between
//! blocks, and tried once more when the transfer itself failed, since a download link may expire
//! or a connection break part way. A file downloaded is made new and removed again on any failure,
//! so that a failed download leaves nothing behind.

use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};

use gdl_files::{BLOCK_LEN, Cancel, Job, NoProgress, Progress, fill_from, retry_locked};
use gdl_media::Sha256Digest;
use sha2::{Digest, Sha256};

use crate::http::{Wanted, read_failure};
use crate::{Asset, Feed, MAX_ASSET_LEN, MAX_LIST_LEN, ReleaseError};

/// How many times a download is tried: once, and once more after a transfer that failed.
pub const DOWNLOAD_ATTEMPTS: u32 = 2;

/// A file downloaded whole and checked against its asset's digest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Downloaded {
    /// Where the file was written.
    pub path: PathBuf,
    /// Its length in bytes, the size its asset publishes.
    pub size: u64,
    /// Its SHA-256 digest, the one its asset publishes.
    pub sha256: Sha256Digest,
}

impl Feed {
    /// Downloads `asset` into `folder` under its own name, as a file made new, so that it is
    /// never written through a link and never replaces a file: one already there is refused as
    /// [`ReleaseError::Io`] and left as it is. The file is written at most [`BLOCK_LEN`] bytes at
    /// a time, the cancel looked at before each block and the progress told after it; it is
    /// refused past its published size ([`ReleaseError::Oversize`]) or short of it or of another
    /// digest ([`ReleaseError::Corrupt`]), synced and closed once whole, and removed on any
    /// failure. A failed transfer, [`ReleaseError::Oversize`], [`ReleaseError::Corrupt`],
    /// [`ReleaseError::Offline`] or [`ReleaseError::Failed`], is tried once more from the asset's
    /// own URL. An asset whose name is no plain file name, or whose size is out of its bounds, is
    /// refused as [`ReleaseError::Incomplete`] before anything is asked or made.
    pub fn download(
        &self,
        asset: &Asset,
        folder: &Path,
        job: &Job<'_>,
    ) -> Result<Downloaded, ReleaseError> {
        within_bounds(asset, MAX_ASSET_LEN)?;
        let path = folder.join(plain_name(asset)?);
        job.cancel.check()?;
        let failed_at = |source| ReleaseError::Io {
            path: path.clone(),
            source,
        };
        let mut tries = 0;
        loop {
            tries += 1;
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&path)
                .map_err(failed_at)?;
            let written = self
                .transfer(asset, job.cancel, job.progress, |block| {
                    file.write_all(block).map_err(failed_at)
                })
                .and_then(|sha256| file.sync_all().map(|()| sha256).map_err(failed_at));
            drop(file);
            let error = match written {
                Ok(sha256) => {
                    return Ok(Downloaded {
                        path,
                        size: asset.size,
                        sha256,
                    });
                }
                Err(error) => error,
            };
            // A file that cannot be removed cannot be made new for another try, so the download
            // ends with its own failure and what is left goes with the folder it was made in.
            let removed = retry_locked(|| fs::remove_file(&path)).is_ok();
            if !removed || !tries_again(&error, tries) {
                return Err(error);
            }
        }
    }

    /// Reads `asset`, at most [`MAX_LIST_LEN`] bytes, into memory, checked and tried again as
    /// [`Feed::download`] checks and tries a file: what a checksum list and its signature are
    /// fetched by.
    pub fn fetch_small(&self, asset: &Asset, cancel: &Cancel) -> Result<Vec<u8>, ReleaseError> {
        within_bounds(asset, MAX_LIST_LEN)?;
        let mut tries = 0;
        loop {
            tries += 1;
            let mut bytes = Vec::new();
            let fetched = self.transfer(asset, cancel, &NoProgress, |block| {
                bytes.extend_from_slice(block);
                Ok(())
            });
            match fetched {
                Ok(_) => return Ok(bytes),
                Err(error) if !tries_again(&error, tries) => return Err(error),
                Err(_) => {}
            }
        }
    }

    /// One try at `asset`'s bytes from its URL: each block of at most [`BLOCK_LEN`] given to
    /// `keep` as it comes, the cancel looked at before each block and `progress` told after it.
    /// One byte past the published size is asked of the answer, so that a longer one is refused
    /// as [`ReleaseError::Oversize`] before any of its excess is kept; one that ends short of the
    /// size, or whose bytes have another digest, is [`ReleaseError::Corrupt`]. Gives the digest
    /// of what was kept.
    fn transfer(
        &self,
        asset: &Asset,
        cancel: &Cancel,
        progress: &dyn Progress,
        mut keep: impl FnMut(&[u8]) -> Result<(), ReleaseError>,
    ) -> Result<Sha256Digest, ReleaseError> {
        let mut response = self.transport.get(&asset.url, Wanted::File, cancel)?;
        let mut body = response
            .body_mut()
            .as_reader()
            .take(asset.size.saturating_add(1));
        let mut block = vec![0; BLOCK_LEN];
        let mut hasher = Sha256::new();
        let mut done = 0_u64;
        loop {
            cancel.check()?;
            let filled = fill_from(&mut body, &mut block).map_err(read_failure)?;
            if filled == 0 {
                break;
            }
            done += filled as u64;
            if done > asset.size {
                return Err(ReleaseError::Oversize {
                    asset: asset.name.as_str().into(),
                });
            }
            let bytes = &block[..filled];
            keep(bytes)?;
            hasher.update(bytes);
            progress.advance(done, asset.size, None);
        }
        let sha256 = Sha256Digest(hasher.finalize().into());
        if done != asset.size || sha256 != asset.sha256 {
            return Err(ReleaseError::Corrupt {
                asset: asset.name.as_str().into(),
            });
        }
        Ok(sha256)
    }
}

/// Refuses `asset` as [`ReleaseError::Incomplete`] unless its published size is a whole number
/// of bytes from 1 to `most`, the bounds the feed holds the asset to.
fn within_bounds(asset: &Asset, most: u64) -> Result<(), ReleaseError> {
    if (1..=most).contains(&asset.size) {
        Ok(())
    } else {
        Err(ReleaseError::Incomplete {
            asset: asset.name.as_str().into(),
        })
    }
}

/// `asset`'s name when it is a plain file name, one part of a path and nothing else, so that the
/// file lands in the folder it is downloaded to; anything else is [`ReleaseError::Incomplete`].
fn plain_name(asset: &Asset) -> Result<&str, ReleaseError> {
    let name = asset.name.as_str();
    let mut parts = Path::new(name).components();
    match (parts.next(), parts.next()) {
        (Some(Component::Normal(part)), None) if part == name => Ok(name),
        _ => Err(ReleaseError::Incomplete { asset: name.into() }),
    }
}

/// Whether a download that failed with `error` on its `tries`th try is tried again: only after a
/// failure of the transfer itself, which another try may not meet, and at most
/// [`DOWNLOAD_ATTEMPTS`] times in all.
fn tries_again(error: &ReleaseError, tries: u32) -> bool {
    tries < DOWNLOAD_ATTEMPTS
        && matches!(
            error,
            ReleaseError::Oversize { .. }
                | ReleaseError::Corrupt { .. }
                | ReleaseError::Offline { .. }
                | ReleaseError::Failed { .. }
        )
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};
    use std::slice;
    use std::sync::{Mutex, MutexGuard, PoisonError};

    use gdl_files::{BLOCK_LEN, Cancel, Job, PhaseId, Progress, RelPath};
    use gdl_fixtures::{LoopbackServer, Reply, Request, symlink_file_where_allowed};
    use gdl_media::Sha256Digest;

    use super::{DOWNLOAD_ATTEMPTS, Downloaded, tries_again};
    use crate::{
        Asset, DOWNLOAD_ACCEPT, Feed, FeedConfig, MAX_ASSET_LEN, MAX_LIST_LEN, Platform,
        REPOSITORY, ReleaseError, Version, checksums_asset, launcher_asset,
    };

    /// The launcher version the tests' requests name.
    const RUNNING: &str = "1.0.0-beta";

    /// The release whose files the tests download.
    const RELEASE: &str = "1.0.0-beta.1";

    /// What the tests' launcher file holds.
    const PAYLOAD: &[u8] = b"payload";

    /// Progress that writes down every step it is told and, when asked to, cancels its job at
    /// one of them.
    #[derive(Default)]
    struct Told {
        /// The bytes done and the total of each step, in order.
        steps: Mutex<Vec<(u64, u64)>>,
        /// The job's cancel.
        cancel: Cancel,
        /// The step, counted from 1, at which the job is cancelled.
        cancel_at: Option<usize>,
    }

    impl Told {
        /// Progress that cancels its job at step `at`.
        fn cancelling_at(at: usize) -> Told {
            Told {
                cancel_at: Some(at),
                ..Told::default()
            }
        }

        /// The steps told so far.
        fn steps(&self) -> MutexGuard<'_, Vec<(u64, u64)>> {
            self.steps.lock().unwrap_or_else(PoisonError::into_inner)
        }
    }

    impl Progress for Told {
        fn phase(&self, phase: PhaseId) {
            panic!("a download begins no phase of its own: {phase:?}");
        }

        fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
            assert!(item.is_none(), "a download names no payload file");
            let mut steps = self.steps();
            steps.push((done, total));
            if self.cancel_at == Some(steps.len()) {
                self.cancel.cancel();
            }
        }
    }

    /// The feed `server` serves, read as the running launcher.
    fn feed(server: &LoopbackServer) -> Feed {
        Feed::new(FeedConfig::loopback(&server.base(), RUNNING).expect("a loopback base"))
    }

    /// The release's launcher file's name for Windows.
    fn launcher_name() -> String {
        launcher_asset(&Version::parse(RELEASE).unwrap(), Platform::WindowsX64)
    }

    /// The request target the asset `name` is served at, as the feed's rows name it.
    fn target_of(name: &str) -> String {
        format!("/{REPOSITORY}/releases/download/v{RELEASE}/{name}")
    }

    /// The asset `name`, served from `server` and published as holding `bytes`.
    fn asset_named(server: &LoopbackServer, name: &str, bytes: &[u8]) -> Asset {
        let base = server.base();
        Asset {
            name: name.to_owned(),
            url: format!("{}{}", base.trim_end_matches('/'), target_of(name)),
            size: bytes.len() as u64,
            sha256: Sha256Digest::of(bytes),
        }
    }

    /// The release's launcher file, served from `server` and published as holding `bytes`.
    fn asset_of(server: &LoopbackServer, bytes: &[u8]) -> Asset {
        asset_named(server, &launcher_name(), bytes)
    }

    /// `feed`'s download of `asset` into `folder`, told to `progress`, cancelled through it.
    fn download(
        feed: &Feed,
        asset: &Asset,
        folder: &Path,
        progress: &Told,
    ) -> Result<Downloaded, ReleaseError> {
        let job = Job {
            cancel: &progress.cancel,
            progress,
        };
        feed.download(asset, folder, &job)
    }

    /// The names `folder` holds, in order.
    fn entries(folder: &Path) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(folder)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
        names.sort();
        names
    }

    /// The targets of the requests `server` read after the first `skipped`.
    fn targets_after(server: &LoopbackServer, skipped: usize) -> Vec<String> {
        server.requests()[skipped..]
            .iter()
            .map(|request| request.target.clone())
            .collect()
    }

    /// The value of `request`'s header `name`, letter case aside.
    fn header<'a>(request: &'a Request, name: &str) -> Option<&'a str> {
        request
            .headers
            .iter()
            .find(|(given, _)| given.eq_ignore_ascii_case(name))
            .map(|(_, value)| value.as_str())
    }

    /// The failure `result` holds, or a panic naming what came instead.
    fn refused<T: std::fmt::Debug>(result: Result<T, ReleaseError>, what: &str) -> ReleaseError {
        match result {
            Err(error) => error,
            Ok(value) => panic!("{what} was taken: {value:?}"),
        }
    }

    #[test]
    fn a_download_is_bounded_verified_and_cancellable() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let folder = tempfile::tempdir().unwrap();
        let asset = asset_of(&server, PAYLOAD);
        let target = target_of(&asset.name);
        let path = folder.path().join(&asset.name);

        // Nothing, too little, the right length of other bytes and too much are each refused,
        // tried once more, and leave no file.
        let bodies: [(&[u8], &str); 4] = [
            (b"", "corrupt"),
            (b"pay", "corrupt"),
            (b"invalid", "corrupt"),
            (b"payload-extra", "oversize"),
        ];
        for (body, rule) in bodies {
            let shown = String::from_utf8_lossy(body);
            let asked = server.requests().len();
            server.route(&target, Reply::ok(body));
            let error = refused(
                download(&feed, &asset, folder.path(), &Told::default()),
                &shown,
            );
            assert_eq!(error.rule(), rule, "{shown:?}: {error}");
            assert!(error.to_string().ends_with(&asset.name), "{error}");
            assert!(entries(folder.path()).is_empty(), "{shown:?} left a file");
            assert_eq!(
                targets_after(&server, asked),
                vec![target.clone(); DOWNLOAD_ATTEMPTS as usize],
                "{shown:?} is tried once more"
            );
        }

        // A cancel at the first progress stops the download at its next look, with nothing left,
        // and is not tried again.
        server.route(&target, Reply::ok(PAYLOAD));
        let asked = server.requests().len();
        let cancelling = Told::cancelling_at(1);
        let error = refused(
            download(&feed, &asset, folder.path(), &cancelling),
            "a cancelled download",
        );
        assert!(matches!(error, ReleaseError::Cancelled), "{error}");
        assert_eq!(*cancelling.steps(), [(7, 7)]);
        assert!(entries(folder.path()).is_empty());
        assert_eq!(targets_after(&server, asked), slice::from_ref(&target));

        // The asset's own bytes are written whole, and told as they come.
        let told = Told::default();
        let downloaded = download(&feed, &asset, folder.path(), &told).unwrap();
        assert_eq!(
            downloaded,
            Downloaded {
                path: path.clone(),
                size: PAYLOAD.len() as u64,
                sha256: Sha256Digest::of(PAYLOAD),
            }
        );
        assert_eq!(fs::read(&path).unwrap(), PAYLOAD);
        assert_eq!(*told.steps(), [(7, 7)]);

        // A second download into the same file is refused before anything is asked, and the
        // file is kept as it was.
        let asked = server.requests().len();
        match download(&feed, &asset, folder.path(), &Told::default()) {
            Err(ReleaseError::Io {
                path: refused,
                source,
            }) => {
                assert_eq!(refused, path);
                assert_eq!(source.kind(), io::ErrorKind::AlreadyExists);
            }
            other => panic!("a download over a file: {other:?}"),
        }
        assert_eq!(fs::read(&path).unwrap(), PAYLOAD);
        assert_eq!(server.requests().len(), asked);
    }

    #[test]
    fn a_broken_transfer_is_tried_once_more() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let asset = asset_of(&server, PAYLOAD);
        let target = target_of(&asset.name);
        let first_tries: [(&str, Reply); 4] = [
            ("a body cut short", Reply::cut(&b"pay"[..], 7)),
            ("a short body", Reply::ok(&b"pay"[..])),
            ("a long body", Reply::ok(&b"payload!"[..])),
            ("another digest", Reply::ok(&b"PAYLOAD"[..])),
        ];
        for (what, first) in first_tries {
            let folder = tempfile::tempdir().unwrap();
            server.route(&target, first);
            server.route(&target, Reply::ok(PAYLOAD));
            let asked = server.requests().len();
            let told = Told::default();
            let downloaded = download(&feed, &asset, folder.path(), &told)
                .unwrap_or_else(|error| panic!("after {what}: {error}"));
            assert_eq!(fs::read(&downloaded.path).unwrap(), PAYLOAD, "{what}");
            assert_eq!(
                entries(folder.path()),
                slice::from_ref(&asset.name),
                "{what}"
            );
            assert_eq!(
                targets_after(&server, asked),
                [target.clone(), target.clone()],
                "{what}"
            );
            // The second try is told from its own start.
            assert_eq!(told.steps().last(), Some(&(7, 7)), "{what}");
        }
    }

    #[test]
    fn a_second_broken_transfer_fails() {
        let cases: [(Reply, Reply, &str); 3] = [
            (
                Reply::cut(&b"pay"[..], 7),
                Reply::cut(&b"payl"[..], 7),
                "failed",
            ),
            (
                Reply::ok(&b"pay"[..]),
                Reply::ok(&b"PAYLOAD"[..]),
                "corrupt",
            ),
            (
                Reply::cut(&b"pay"[..], 7),
                Reply::ok(&b"payload-extra"[..]),
                "oversize",
            ),
        ];
        for (first, second, rule) in cases {
            let server = LoopbackServer::start().unwrap();
            let feed = feed(&server);
            let asset = asset_of(&server, PAYLOAD);
            let target = target_of(&asset.name);
            let folder = tempfile::tempdir().unwrap();
            server.route(&target, first);
            server.route(&target, second);
            // A third try would succeed, so one would show.
            server.route(&target, Reply::ok(PAYLOAD));
            let error = refused(
                download(&feed, &asset, folder.path(), &Told::default()),
                rule,
            );
            assert_eq!(error.rule(), rule, "{error}");
            assert!(entries(folder.path()).is_empty(), "{rule} left a file");
            assert_eq!(
                server.requests().len(),
                DOWNLOAD_ATTEMPTS as usize,
                "{rule}"
            );
        }
    }

    #[test]
    fn a_redirected_download_follows_to_the_storage_host_alone() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let asset = asset_of(&server, PAYLOAD);
        let target = target_of(&asset.name);

        // The download URL sends the request on to where the file is stored; a transfer from
        // there that breaks is tried again from the download URL, since the place it was sent on
        // to may no longer hold.
        let storage = "/storage/release-asset?token=one-time";
        server.route(&target, Reply::redirect(302, storage));
        server.route(storage, Reply::cut(&b"pay"[..], 7));
        server.route(storage, Reply::ok(PAYLOAD));
        let folder = tempfile::tempdir().unwrap();
        let downloaded = download(&feed, &asset, folder.path(), &Told::default()).unwrap();
        assert_eq!(fs::read(&downloaded.path).unwrap(), PAYLOAD);
        assert_eq!(
            targets_after(&server, 0),
            [
                target.clone(),
                storage.to_owned(),
                target.clone(),
                storage.to_owned()
            ]
        );

        // A redirect out of the feed's own origin is refused, not tried again, and leaves
        // nothing.
        let base = server.base();
        let authority = base
            .strip_prefix("http://")
            .and_then(|rest| rest.strip_suffix('/'))
            .unwrap()
            .to_owned();
        let port = authority.rsplit_once(':').unwrap().1.to_owned();
        let other_port = (port.parse::<u16>().unwrap() ^ 1).to_string();
        let elsewhere = [
            format!("http://127.0.0.1:{other_port}/storage"),
            format!("http://localhost:{port}/storage"),
            format!("http://[::1]:{port}/storage"),
            format!("https://{authority}/storage"),
            "https://release-assets.githubusercontent.com/storage".to_owned(),
            "//example.com/storage".to_owned(),
        ];
        for location in elsewhere {
            let folder = tempfile::tempdir().unwrap();
            server.route(&target, Reply::redirect(302, &location));
            let asked = server.requests().len();
            let error = refused(
                download(&feed, &asset, folder.path(), &Told::default()),
                &location,
            );
            assert!(
                matches!(error, ReleaseError::UntrustedHost { .. }),
                "{location}: {error}"
            );
            assert!(entries(folder.path()).is_empty(), "{location}");
            assert_eq!(
                targets_after(&server, asked),
                slice::from_ref(&target),
                "{location}"
            );
        }
    }

    #[test]
    fn a_download_needs_no_api_header() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let asset = asset_of(&server, PAYLOAD);
        let storage = "/storage/release-asset";
        server.route(&target_of(&asset.name), Reply::redirect(302, storage));
        server.route(storage, Reply::ok(PAYLOAD));
        let folder = tempfile::tempdir().unwrap();
        download(&feed, &asset, folder.path(), &Told::default()).unwrap();
        assert_eq!(
            feed.fetch_small(&asset, &Cancel::default()).unwrap(),
            PAYLOAD
        );
        let requests = server.requests();
        assert_eq!(requests.len(), 4);
        for request in &requests {
            assert_eq!(request.method, "GET");
            assert_eq!(
                header(request, "Accept"),
                Some(DOWNLOAD_ACCEPT),
                "{request:?}"
            );
            assert_eq!(
                header(request, "User-Agent"),
                Some("GauntletDarkLegacy-Launcher/1.0.0-beta")
            );
            for name in [
                "X-GitHub-Api-Version",
                "Authorization",
                "Cookie",
                "Proxy-Authorization",
            ] {
                assert_eq!(header(request, name), None, "{name} in {request:?}");
            }
        }
    }

    #[test]
    fn a_small_file_is_checked_as_a_download_is() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let version = Version::parse(RELEASE).unwrap();
        let name = checksums_asset(&version, Platform::WindowsX64);
        let list = format!("{}  {}\n", Sha256Digest::of(PAYLOAD), launcher_name()).into_bytes();
        let asset = asset_named(&server, &name, &list);
        let target = target_of(&name);

        server.route(&target, Reply::ok(list.clone()));
        assert_eq!(feed.fetch_small(&asset, &Cancel::default()).unwrap(), list);

        // A short, long or other list is refused after a second try.
        let mut other = list.clone();
        other[0] ^= 1;
        let longer = [list.as_slice(), &b"x"[..]].concat();
        let cases: [(&[u8], &str); 3] = [
            (&list[..list.len() - 1], "corrupt"),
            (&longer, "oversize"),
            (&other, "corrupt"),
        ];
        for (body, rule) in cases {
            let asked = server.requests().len();
            server.route(&target, Reply::ok(body));
            let error = refused(feed.fetch_small(&asset, &Cancel::default()), rule);
            assert_eq!(error.rule(), rule, "{error}");
            assert_eq!(
                server.requests().len(),
                asked + DOWNLOAD_ATTEMPTS as usize,
                "{rule}"
            );
        }

        // A transfer cut short is tried again.
        server.route(&target, Reply::cut(&list[..10], list.len() as u64));
        server.route(&target, Reply::ok(list.clone()));
        let asked = server.requests().len();
        assert_eq!(feed.fetch_small(&asset, &Cancel::default()).unwrap(), list);
        assert_eq!(server.requests().len(), asked + 2);

        // A cancelled job asks nothing.
        let cancel = Cancel::default();
        cancel.cancel();
        let asked = server.requests().len();
        assert!(matches!(
            feed.fetch_small(&asset, &cancel),
            Err(ReleaseError::Cancelled)
        ));
        assert_eq!(server.requests().len(), asked);

        // A file of the most bytes is read whole.
        let longest = vec![b'l'; usize::try_from(MAX_LIST_LEN).unwrap()];
        let asset = asset_named(&server, &name, &longest);
        server.route(&target, Reply::ok(longest.clone()));
        assert_eq!(
            feed.fetch_small(&asset, &Cancel::default()).unwrap(),
            longest
        );
    }

    #[test]
    fn only_a_transfer_that_failed_is_tried_again() {
        let boxed = |text: &str| -> Box<str> { text.into() };
        let transfers = [
            ReleaseError::Oversize { asset: boxed("a") },
            ReleaseError::Corrupt { asset: boxed("a") },
            ReleaseError::Offline {
                detail: boxed("connection refused"),
            },
            ReleaseError::Failed {
                detail: boxed("Peer disconnected"),
            },
        ];
        for error in &transfers {
            assert!(tries_again(error, 1), "{error}");
            assert!(!tries_again(error, DOWNLOAD_ATTEMPTS), "{error}");
        }
        let others = [
            ReleaseError::Status {
                status: 404,
                url: boxed("u"),
            },
            ReleaseError::RateLimited { reset_at: None },
            ReleaseError::UntrustedHost { url: boxed("u") },
            ReleaseError::Redirects { url: boxed("u") },
            ReleaseError::Incomplete { asset: boxed("a") },
            ReleaseError::Proxy {
                detail: boxed("SOCKS proxies are not supported"),
            },
            ReleaseError::Io {
                path: PathBuf::from("a"),
                source: io::ErrorKind::StorageFull.into(),
            },
            ReleaseError::Cancelled,
        ];
        for error in &others {
            assert!(!tries_again(error, 1), "{error}");
        }

        // Served: an answer the transfer did not fail on is asked for once.
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let asset = asset_of(&server, PAYLOAD);
        let target = target_of(&asset.name);
        let answers = [
            (Reply::status(404), "status"),
            (
                Reply::status(403)
                    .with_header("X-RateLimit-Remaining", "0")
                    .with_header("X-RateLimit-Reset", "1764003600"),
                "rateLimited",
            ),
            (Reply::redirect(302, &target), "redirects"),
            (
                Reply::redirect(302, "https://github.com/x"),
                "untrustedHost",
            ),
        ];
        for (answer, rule) in answers {
            let folder = tempfile::tempdir().unwrap();
            server.route(&target, answer);
            let asked = server.requests().len();
            let error = refused(
                download(&feed, &asset, folder.path(), &Told::default()),
                rule,
            );
            assert_eq!(error.rule(), rule, "{error}");
            assert!(entries(folder.path()).is_empty(), "{rule}");
            assert_eq!(server.requests().len(), asked + 1, "{rule}");
            let error = refused(feed.fetch_small(&asset, &Cancel::default()), rule);
            assert_eq!(error.rule(), rule, "{error}");
            assert_eq!(server.requests().len(), asked + 2, "{rule}");
        }

        // A folder that is not there makes the file fail before anything is asked.
        server.route(&target, Reply::ok(PAYLOAD));
        let folder = tempfile::tempdir().unwrap();
        let missing = folder.path().join("missing");
        let asked = server.requests().len();
        match download(&feed, &asset, &missing, &Told::default()) {
            Err(ReleaseError::Io { path, source }) => {
                assert_eq!(path, missing.join(&asset.name));
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("a download into no folder: {other:?}"),
        }
        assert_eq!(server.requests().len(), asked);
    }

    #[test]
    fn an_asset_out_of_bounds_or_no_file_name_is_refused_unasked() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let folder = tempfile::tempdir().unwrap();
        let sound = asset_of(&server, PAYLOAD);
        server.route(&target_of(&sound.name), Reply::ok(PAYLOAD));

        let mut misnamed = vec!["", ".", "..", "../x", "x/..", "a/b", "/x", "x/", "./x"];
        if cfg!(windows) {
            misnamed.extend(["a\\b", "\\x", "C:x", "C:\\x", "\\\\server\\share\\x"]);
        }
        let mut refusals: Vec<Asset> = misnamed
            .iter()
            .map(|name| Asset {
                name: (*name).to_owned(),
                ..sound.clone()
            })
            .collect();
        for size in [0, MAX_ASSET_LEN + 1] {
            refusals.push(Asset {
                size,
                ..sound.clone()
            });
        }
        for asset in &refusals {
            match download(&feed, asset, folder.path(), &Told::default()) {
                Err(ReleaseError::Incomplete { asset: refused }) => {
                    assert_eq!(&*refused, asset.name.as_str(), "{asset:?}");
                }
                other => panic!("{asset:?} was downloaded: {other:?}"),
            }
        }
        for size in [0, MAX_LIST_LEN + 1] {
            let asset = Asset {
                size,
                ..sound.clone()
            };
            assert!(
                matches!(
                    feed.fetch_small(&asset, &Cancel::default()),
                    Err(ReleaseError::Incomplete { .. })
                ),
                "{size}"
            );
        }
        assert!(entries(folder.path()).is_empty());
        assert!(server.requests().is_empty());

        // The sound asset is taken by either.
        assert_eq!(
            feed.fetch_small(&sound, &Cancel::default()).unwrap(),
            PAYLOAD
        );
        download(&feed, &sound, folder.path(), &Told::default()).unwrap();
        assert_eq!(entries(folder.path()), slice::from_ref(&sound.name));
    }

    #[test]
    fn progress_is_told_after_each_block() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let body: Vec<u8> = (0..2 * BLOCK_LEN + 5)
            .map(|at| u8::try_from(at % 251).unwrap())
            .collect();
        let total = body.len() as u64;
        let block = BLOCK_LEN as u64;
        let asset = asset_of(&server, &body);
        server.route(&target_of(&asset.name), Reply::ok(body.clone()));

        let folder = tempfile::tempdir().unwrap();
        let told = Told::default();
        let downloaded = download(&feed, &asset, folder.path(), &told).unwrap();
        assert_eq!(fs::read(&downloaded.path).unwrap(), body);
        assert_eq!(
            *told.steps(),
            [(block, total), (2 * block, total), (total, total)]
        );

        // A cancel is heeded before the next block, whatever is still to come.
        let folder = tempfile::tempdir().unwrap();
        let cancelling = Told::cancelling_at(2);
        assert!(matches!(
            download(&feed, &asset, folder.path(), &cancelling),
            Err(ReleaseError::Cancelled)
        ));
        assert_eq!(*cancelling.steps(), [(block, total), (2 * block, total)]);
        assert!(entries(folder.path()).is_empty());
    }

    #[test]
    fn a_link_in_the_files_place_is_never_followed() {
        let server = LoopbackServer::start().unwrap();
        let feed = feed(&server);
        let asset = asset_of(&server, PAYLOAD);
        server.route(&target_of(&asset.name), Reply::ok(PAYLOAD));
        let folder = tempfile::tempdir().unwrap();
        let outside = tempfile::tempdir().unwrap();
        let link = folder.path().join(&asset.name);
        let aim = outside.path().join("aim");
        if !symlink_file_where_allowed(&link, &aim) {
            return;
        }
        match download(&feed, &asset, folder.path(), &Told::default()) {
            Err(ReleaseError::Io { path, source }) => {
                assert_eq!(path, link);
                assert_eq!(source.kind(), io::ErrorKind::AlreadyExists);
            }
            other => panic!("a download through a link: {other:?}"),
        }
        assert!(
            fs::symlink_metadata(&link)
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert!(fs::symlink_metadata(&aim).is_err(), "the link was followed");
        assert!(server.requests().is_empty());
    }
}
