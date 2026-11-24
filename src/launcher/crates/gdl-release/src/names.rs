//! The names of the launcher's releases: the repository that publishes them, the hosts its feed
//! and downloads may reach, and the file name of each asset a release carries, spelt here alone so
//! that the feed, the update and the publisher agree on them. None of them is a name an alpha
//! release carries, so the launcher never takes an alpha release for one of its own.

use crate::{Platform, Version};

/// The GitHub repository whose releases are the launcher's.
pub const REPOSITORY: &str = "sabishii-bit/Gauntlet-Dark-Legacy-PC-Port";

/// The host of GitHub's API, which lists the repository's releases.
pub const GITHUB_API_HOST: &str = "api.github.com";

/// The host a release asset's download URL names, which redirects it to GitHub's storage.
pub const GITHUB_HOST: &str = "github.com";

/// The hosts a request and each of its redirects may reach: the API, the download host and the
/// two storage hosts GitHub sends downloads on to.
pub const RELEASE_HOSTS: [&str; 4] = [
    GITHUB_API_HOST,
    GITHUB_HOST,
    "release-assets.githubusercontent.com",
    "objects.githubusercontent.com",
];

/// The first word of every asset name.
pub const ASSET_STEM: &str = "GauntletDarkLegacy";

/// The suffix of a signature's name after its checksum list's.
pub const SIGNATURE_SUFFIX: &str = ".sig";

/// What the launcher's names put after the version and platform: the Linux archive's member, and
/// the start of the suffix of each platform's launcher asset.
const LAUNCHER: &str = "-launcher";

/// The suffix of the Windows launcher asset, which is the program itself.
const WINDOWS_PROGRAM_SUFFIX: &str = ".exe";

/// The suffix of the Linux launcher asset, a gzip-compressed tar archive of the program alone.
const ARCHIVE_SUFFIX: &str = ".tar.gz";

/// The suffix of a checksum list's name after the version and platform.
const CHECKSUMS_SUFFIX: &str = ".sha256";

/// The suffix of the licences' name after the version and platform: a zip of the notices, which
/// can be read before anything is installed.
const LICENCES_SUFFIX: &str = "-licenses.zip";

/// The name of `version`'s asset for `platform` that ends in `suffix`.
fn asset_name(version: &Version, platform: Platform, suffix: &str) -> String {
    format!("{ASSET_STEM}-{version}-{}{suffix}", platform.as_str())
}

/// The asset that carries `version`'s launcher, with the game inside, for `platform`: the program
/// on Windows; on Linux an archive of it, named after the one file it holds, [`linux_member`].
pub fn launcher_asset(version: &Version, platform: Platform) -> String {
    let suffix = match platform {
        Platform::WindowsX64 => WINDOWS_PROGRAM_SUFFIX,
        Platform::LinuxX64 => ARCHIVE_SUFFIX,
    };
    format!("{}{suffix}", asset_name(version, platform, LAUNCHER))
}

/// The one file the Linux launcher asset of `version` holds: the launcher program.
pub fn linux_member(version: &Version) -> String {
    asset_name(version, Platform::LinuxX64, LAUNCHER)
}

/// The checksum list of `version`'s assets for `platform`.
pub fn checksums_asset(version: &Version, platform: Platform) -> String {
    asset_name(version, platform, CHECKSUMS_SUFFIX)
}

/// The signature of `version`'s checksum list for `platform`.
pub fn signature_asset(version: &Version, platform: Platform) -> String {
    format!("{}{SIGNATURE_SUFFIX}", checksums_asset(version, platform))
}

/// The licences of `version`'s launcher for `platform`.
pub fn licences_asset(version: &Version, platform: Platform) -> String {
    asset_name(version, platform, LICENCES_SUFFIX)
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;

    use serde_json::Value;

    use super::{
        ASSET_STEM, GITHUB_API_HOST, GITHUB_HOST, RELEASE_HOSTS, REPOSITORY, SIGNATURE_SUFFIX,
        checksums_asset, launcher_asset, licences_asset, linux_member, signature_asset,
    };
    use crate::{Platform, Version};

    /// The version cases the release scripts' tests read too.
    const VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/semver-vectors.json"
    ));

    /// The groups of the shared vectors that list the published alpha tags, below and above the
    /// launcher's first release.
    const ALPHA_GROUPS: [&str; 2] = ["below", "above"];

    /// What the alpha installer's releases put after an asset's version and platform: the runtime
    /// zip, the Windows and Linux setups, the setup's checksum list and its licences.
    const ALPHA_SUFFIXES: [&str; 5] = [
        "-runtime.zip",
        "-setup.exe",
        "-setup.tar.gz",
        "-setup.sha256",
        "-setup-licenses.zip",
    ];

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// Every name a release of `version` carries or holds, for both platforms.
    fn new_line_names(version: &Version) -> Vec<String> {
        let mut names = vec![linux_member(version)];
        for platform in Platform::ALL {
            names.extend([
                launcher_asset(version, platform),
                checksums_asset(version, platform),
                signature_asset(version, platform),
                licences_asset(version, platform),
            ]);
        }
        names
    }

    /// The published alpha releases' versions, from the shared vectors.
    fn alpha_versions() -> Vec<Version> {
        let vectors: Value = serde_json::from_str(VECTORS).expect("the shared vectors are JSON");
        let mut versions = Vec::new();
        for group in ALPHA_GROUPS {
            let tags = vectors["betaBoundary"][group]
                .as_array()
                .unwrap_or_else(|| panic!("the shared vectors list betaBoundary.{group}"));
            assert!(!tags.is_empty(), "betaBoundary.{group} is empty");
            for tag in tags {
                let tag = tag.as_str().expect("an alpha tag is text");
                let version = Version::parse_tag(tag).unwrap_or_else(|error| panic!("{error}"));
                versions.push(version);
            }
        }
        versions
    }

    #[test]
    fn the_new_line_names_its_assets() {
        let expected = [
            (
                "1.0.0-beta",
                [
                    "GauntletDarkLegacy-1.0.0-beta-windows-x64-launcher.exe",
                    "GauntletDarkLegacy-1.0.0-beta-windows-x64.sha256",
                    "GauntletDarkLegacy-1.0.0-beta-windows-x64.sha256.sig",
                    "GauntletDarkLegacy-1.0.0-beta-windows-x64-licenses.zip",
                    "GauntletDarkLegacy-1.0.0-beta-linux-x64-launcher.tar.gz",
                    "GauntletDarkLegacy-1.0.0-beta-linux-x64.sha256",
                    "GauntletDarkLegacy-1.0.0-beta-linux-x64.sha256.sig",
                    "GauntletDarkLegacy-1.0.0-beta-linux-x64-licenses.zip",
                    "GauntletDarkLegacy-1.0.0-beta-linux-x64-launcher",
                ],
            ),
            (
                "1.0.0+build.1",
                [
                    "GauntletDarkLegacy-1.0.0+build.1-windows-x64-launcher.exe",
                    "GauntletDarkLegacy-1.0.0+build.1-windows-x64.sha256",
                    "GauntletDarkLegacy-1.0.0+build.1-windows-x64.sha256.sig",
                    "GauntletDarkLegacy-1.0.0+build.1-windows-x64-licenses.zip",
                    "GauntletDarkLegacy-1.0.0+build.1-linux-x64-launcher.tar.gz",
                    "GauntletDarkLegacy-1.0.0+build.1-linux-x64.sha256",
                    "GauntletDarkLegacy-1.0.0+build.1-linux-x64.sha256.sig",
                    "GauntletDarkLegacy-1.0.0+build.1-linux-x64-licenses.zip",
                    "GauntletDarkLegacy-1.0.0+build.1-linux-x64-launcher",
                ],
            ),
        ];
        for (text, [assets @ .., member]) in expected {
            let version = version(text);
            let mut names = assets.into_iter();
            for platform in [Platform::WindowsX64, Platform::LinuxX64] {
                let mut next = || names.next().expect("four names a platform");
                assert_eq!(launcher_asset(&version, platform), next(), "{text}");
                assert_eq!(checksums_asset(&version, platform), next(), "{text}");
                assert_eq!(signature_asset(&version, platform), next(), "{text}");
                assert_eq!(licences_asset(&version, platform), next(), "{text}");
            }
            assert_eq!(names.next(), None);
            assert_eq!(linux_member(&version), member, "{text}");

            // Each name is a release's own, and a signature is named after the list it signs.
            let all = new_line_names(&version);
            let distinct: BTreeSet<&String> = all.iter().collect();
            assert_eq!(distinct.len(), all.len(), "{all:?}");
            for name in &all {
                assert!(name.starts_with(&format!("{ASSET_STEM}-{text}-")), "{name}");
            }
            for platform in Platform::ALL {
                assert_eq!(
                    signature_asset(&version, platform),
                    format!("{}{SIGNATURE_SUFFIX}", checksums_asset(&version, platform))
                );
            }
        }

        // The repository and the hosts are the ones the alpha installer reads its releases from.
        assert_eq!(REPOSITORY, "sabishii-bit/Gauntlet-Dark-Legacy-PC-Port");
        assert_eq!(
            RELEASE_HOSTS,
            [
                "api.github.com",
                "github.com",
                "release-assets.githubusercontent.com",
                "objects.githubusercontent.com",
            ]
        );
        assert_eq!(
            (RELEASE_HOSTS[0], RELEASE_HOSTS[1]),
            (GITHUB_API_HOST, GITHUB_HOST)
        );
    }

    #[test]
    fn no_alpha_asset_name_is_a_new_line_name() {
        let versions = alpha_versions();
        let mut alpha = BTreeSet::new();
        let mut new_line = BTreeSet::new();
        for version in &versions {
            for platform in Platform::ALL {
                for suffix in ALPHA_SUFFIXES {
                    alpha.insert(format!(
                        "{ASSET_STEM}-{version}-{}{suffix}",
                        platform.as_str()
                    ));
                }
            }
            new_line.extend(new_line_names(version));
        }
        assert_eq!(
            alpha.len(),
            versions.len() * Platform::ALL.len() * ALPHA_SUFFIXES.len()
        );
        // An alpha release's names, checked against those of the same version and of every other.
        let shared: Vec<&String> = alpha.intersection(&new_line).collect();
        assert!(shared.is_empty(), "alpha names on the new line: {shared:?}");
        for name in &new_line {
            assert!(
                !ALPHA_SUFFIXES.iter().any(|suffix| name.ends_with(suffix)),
                "{name} ends as an alpha asset does"
            );
        }
    }
}
