//! Where an update a session runs comes from: the release feed's newest release, or the runtime
//! this launcher file carries, which updates an installation without the network. The newer of
//! the two wins, and this file a tie, since its update needs no download; a feed that cannot be
//! read still leaves this file's update, so that a player offline updates from the file in hand.

use gdl_release::{ProxySettings, Release, ReleaseError, Version};

/// Where a run's update comes from, or why it has none.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SourceChoice {
    /// The runtime this launcher file carries, newer than the installation's.
    Bundled {
        /// The version of the runtime this file carries.
        version: Version,
    },
    /// The feed's release, newer than the installation and than this file's runtime.
    Release(Box<Release>),
    /// Nothing newer than the installation is at hand.
    UpToDate {
        /// The installed release's version.
        version: Version,
    },
    /// This launcher file is older than the installation, which it may not change.
    OlderLauncher {
        /// The installed release's version.
        installed: Version,
    },
}

/// Chooses where the update of an installation of `installed` comes from, for a launcher of
/// `running` whose file carries the runtime `own` (`None` when it carries none), given the feed's
/// answer: its newest release above the installation, none, or why it could not be read; `feed`
/// is `None` for a run that asks no feed.
///
/// A launcher older than the installation is [`SourceChoice::OlderLauncher`], whatever either
/// source holds. Otherwise the feed's release when it is newer than both the installation and
/// this file's runtime; else this file's runtime when it is newer than the installation, whether
/// the feed was read or not; else [`SourceChoice::UpToDate`]. A feed that could not be read, with
/// nothing newer in this file, is its error. A read the job's cancel stopped is that cancel, never
/// an update from this file: the player asked the run to stop, not to update without the feed.
pub fn choose_source(
    running: &Version,
    own: Option<&Version>,
    installed: &Version,
    feed: Option<Result<Option<Release>, ReleaseError>>,
) -> Result<SourceChoice, ReleaseError> {
    if running < installed {
        return Ok(SourceChoice::OlderLauncher {
            installed: installed.clone(),
        });
    }
    let bundled = own.filter(|own| *own > installed);
    match feed {
        Some(Ok(Some(release)))
            if release.version > *installed && bundled.is_none_or(|own| release.version > *own) =>
        {
            Ok(SourceChoice::Release(Box::new(release)))
        }
        Some(Err(ReleaseError::Cancelled)) => Err(ReleaseError::Cancelled),
        Some(Err(error)) if bundled.is_none() => Err(error),
        Some(Ok(_) | Err(_)) | None => Ok(match bundled {
            Some(version) => SourceChoice::Bundled {
                version: version.clone(),
            },
            None => SourceChoice::UpToDate {
                version: installed.clone(),
            },
        }),
    }
}

/// The system's proxy settings, which a run reads the release feed through: the process's
/// environment and, on Windows, the user's Internet Settings.
pub fn system_proxies() -> ProxySettings {
    ProxySettings::of_process(gdl_install::internet_settings())
}

#[cfg(test)]
mod tests {
    use gdl_media::Sha256Digest;
    use gdl_release::{
        Asset, Platform, REPOSITORY, Release, ReleaseError, Version, checksums_asset,
        launcher_asset,
    };

    use super::{SourceChoice, choose_source};

    /// A release before the installed one.
    const OLDER: &str = "1.0.0-beta";

    /// The installed release.
    const INSTALLED: &str = "1.0.0-beta.1";

    /// A release after the installed one.
    const NEWER: &str = "1.0.0-rc.1";

    /// A release after [`NEWER`].
    const NEWEST: &str = "1.0.0";

    /// [`NEWER`] as another build of it is named: the same release by precedence.
    const NEWER_REBUILT: &str = "1.0.0-rc.1+rebuilt.2";

    /// The version `text` names.
    fn version(text: &str) -> Version {
        Version::parse(text).unwrap()
    }

    /// The asset `name` of the release `release`, as the feed gives one.
    fn asset(release: &Version, name: String) -> Asset {
        Asset {
            url: format!("https://github.com/{REPOSITORY}/releases/download/v{release}/{name}"),
            sha256: Sha256Digest::of(name.as_bytes()),
            name,
            size: 1,
        }
    }

    /// The feed's Windows release of `text`.
    fn release(text: &str) -> Release {
        let release = version(text);
        let platform = Platform::WindowsX64;
        Release {
            tag: format!("v{text}"),
            platform,
            launcher: asset(&release, launcher_asset(&release, platform)),
            checksums: asset(&release, checksums_asset(&release, platform)),
            signature: None,
            notes: None,
            version: release,
        }
    }

    /// The feed's answer that names `text` its newest release.
    fn found(text: &str) -> Option<Result<Option<Release>, ReleaseError>> {
        Some(Ok(Some(release(text))))
    }

    /// The feed's answer that it could not be read, for `error`.
    fn failed(error: ReleaseError) -> Option<Result<Option<Release>, ReleaseError>> {
        Some(Err(error))
    }

    /// A feed that could not be reached.
    fn offline() -> ReleaseError {
        ReleaseError::Offline {
            detail: "connection refused".into(),
        }
    }

    /// Failures of every kind a read of the feed ends in but a cancel.
    fn failures() -> Vec<ReleaseError> {
        vec![
            offline(),
            ReleaseError::Failed {
                detail: "the secure connection failed".into(),
            },
            ReleaseError::Status {
                status: 502,
                url: "https://api.github.com/".into(),
            },
            ReleaseError::RateLimited { reset_at: Some(60) },
            ReleaseError::UntrustedHost {
                url: "https://example.com/".into(),
            },
            ReleaseError::Incomplete {
                asset: "GauntletDarkLegacy-1.0.0-windows-x64-launcher.exe".into(),
            },
            ReleaseError::Signature {
                detail: "no trusted key".into(),
            },
            ReleaseError::Proxy {
                detail: "SOCKS proxies are not supported".into(),
            },
        ]
    }

    /// What a launcher of `running`, whose file carries the runtime `own`, chooses for the
    /// installation of [`INSTALLED`] given the feed's answer `feed`.
    fn choice(
        running: &str,
        own: Option<&str>,
        feed: Option<Result<Option<Release>, ReleaseError>>,
    ) -> Result<SourceChoice, ReleaseError> {
        let own = own.map(version);
        choose_source(&version(running), own.as_ref(), &version(INSTALLED), feed)
    }

    /// The choice of this file's runtime of `text`.
    fn bundled(text: &str) -> SourceChoice {
        SourceChoice::Bundled {
            version: version(text),
        }
    }

    /// The choice that nothing newer than [`INSTALLED`] is at hand.
    fn up_to_date() -> SourceChoice {
        SourceChoice::UpToDate {
            version: version(INSTALLED),
        }
    }

    #[test]
    fn a_newer_release_wins_over_this_file() {
        for (what, running, own) in [
            ("a file carrying a newer runtime", NEWER, Some(NEWER)),
            (
                "a file carrying the installed runtime",
                INSTALLED,
                Some(INSTALLED),
            ),
            ("a file carrying an older runtime", NEWER, Some(OLDER)),
            ("a file carrying no runtime", INSTALLED, None),
        ] {
            assert_eq!(
                choice(running, own, found(NEWEST)).unwrap(),
                SourceChoice::Release(Box::new(release(NEWEST))),
                "{what}"
            );
        }
    }

    #[test]
    fn this_file_wins_a_tie_and_needs_no_download() {
        for (what, own, feed) in [
            ("the same release", NEWER, NEWER),
            ("another build of the same release", NEWER, NEWER_REBUILT),
            ("an older release", NEWEST, NEWER),
        ] {
            let chosen = choice(own, Some(own), found(feed)).unwrap();
            let SourceChoice::Bundled { version } = &chosen else {
                panic!("{what}: {chosen:?}");
            };
            // The version is this file's own, build and all, and not the feed's.
            assert_eq!(version.to_string(), own, "{what}");
        }
    }

    #[test]
    fn a_failed_feed_falls_back_to_this_file() {
        for error in failures() {
            let rule = error.rule();
            assert_eq!(
                choice(NEWER, Some(NEWER), failed(error)).unwrap(),
                bundled(NEWER),
                "{rule}"
            );
        }
    }

    #[test]
    fn a_failed_feed_with_nothing_newer_here_is_its_error() {
        for (what, running, own) in [
            (
                "a file carrying the installed runtime",
                INSTALLED,
                Some(INSTALLED),
            ),
            ("a file carrying an older runtime", NEWER, Some(OLDER)),
            ("a file carrying no runtime", NEWER, None),
        ] {
            for error in failures() {
                let rule = error.rule();
                let refused = choice(running, own, failed(error)).unwrap_err();
                assert_eq!(refused.rule(), rule, "{what}");
            }
        }
    }

    #[test]
    fn an_older_file_offers_nothing_and_never_downgrades() {
        let older = SourceChoice::OlderLauncher {
            installed: version(INSTALLED),
        };
        for (what, own) in [
            ("its own runtime", Some(OLDER)),
            ("no runtime", None),
            // A file older than the launcher that installed the folder is refused even when it
            // carries a newer runtime, since its rules are older than the installation's.
            ("a newer runtime", Some(NEWEST)),
        ] {
            for (feed, answer) in [
                ("asked no feed", None),
                ("finding nothing", Some(Ok(None))),
                ("finding a newer release", found(NEWEST)),
                ("failing", failed(offline())),
                ("cancelled", failed(ReleaseError::Cancelled)),
            ] {
                assert_eq!(choice(OLDER, own, answer).unwrap(), older, "{what}, {feed}");
            }
        }
    }

    #[test]
    fn an_offline_run_never_asks_the_feed() {
        // With no feed asked, nothing newer at hand is no failure.
        for (what, running, own, chosen) in [
            (
                "a file carrying a newer runtime",
                NEWER,
                Some(NEWER),
                bundled(NEWER),
            ),
            (
                "a file carrying the installed runtime",
                INSTALLED,
                Some(INSTALLED),
                up_to_date(),
            ),
            (
                "a file carrying an older runtime",
                NEWER,
                Some(OLDER),
                up_to_date(),
            ),
            ("a file carrying no runtime", NEWER, None, up_to_date()),
        ] {
            assert_eq!(choice(running, own, None).unwrap(), chosen, "{what}");
        }
    }

    #[test]
    fn a_feed_with_nothing_newer_leaves_this_file() {
        for (what, newest) in [
            ("finding nothing", None),
            ("finding the installed release", Some(INSTALLED)),
            ("finding an older release", Some(OLDER)),
        ] {
            let answer = || Some(Ok(newest.map(release)));
            assert_eq!(
                choice(NEWER, Some(NEWER), answer()).unwrap(),
                bundled(NEWER),
                "{what}"
            );
            assert_eq!(
                choice(NEWER, None, answer()).unwrap(),
                up_to_date(),
                "{what}, a file carrying no runtime"
            );
        }
    }

    #[test]
    fn a_cancelled_feed_read_never_falls_back_to_this_file() {
        for (what, own) in [
            ("a file carrying a newer runtime", Some(NEWER)),
            ("a file carrying no runtime", None),
        ] {
            let refused = choice(NEWER, own, failed(ReleaseError::Cancelled)).unwrap_err();
            assert!(
                matches!(refused, ReleaseError::Cancelled),
                "{what}: {refused:?}"
            );
        }
    }
}
