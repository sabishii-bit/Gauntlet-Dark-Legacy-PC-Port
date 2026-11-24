//! Rows of GitHub's releases list, as its API gives them, for the tests of the release feed: a
//! release of the launcher's new line, with its launcher file, checksum list and signature, and a
//! release of the alpha installer, whose assets the launcher never takes. A test serves them as a
//! page of the list, and their assets at the URLs the rows name.

use serde_json::{Map, Value};

use crate::runtime::{WINDOWS_PLATFORM, sha256_hex};

/// The repository whose releases the rows are, as their download URLs name it.
const REPOSITORY: &str = "sabishii-bit/Gauntlet-Dark-Legacy-PC-Port";

/// Where GitHub serves a release's downloads, and so the base of an alpha release's URLs.
const GITHUB_BASE: &str = "https://github.com/";

/// The first word of every asset name.
const ASSET_STEM: &str = "GauntletDarkLegacy";

/// The state of an asset GitHub holds whole.
const UPLOADED: &str = "uploaded";

/// What a digest GitHub gives an asset begins with, before its hexadecimal SHA-256.
const DIGEST_PREFIX: &str = "sha256:";

/// What a tag puts before its release's version.
const TAG_PREFIX: &str = "v";

/// How a download URL writes the `+` before a version's build metadata.
const ESCAPED_PLUS: &str = "%2B";

/// The suffix of the new line's Windows launcher asset after its version and platform.
const WINDOWS_LAUNCHER_SUFFIX: &str = "-launcher.exe";

/// The suffix of the new line's launcher asset for every other platform: an archive of one file.
const ARCHIVE_LAUNCHER_SUFFIX: &str = "-launcher.tar.gz";

/// The suffix of a checksum list's name.
const CHECKSUMS_SUFFIX: &str = ".sha256";

/// The suffix of a signature's name after its checksum list's.
const SIGNATURE_SUFFIX: &str = ".sig";

/// What separates a digest from its file's name on a line of a checksum list.
const LIST_SEPARATOR: &str = "  ";

/// The platforms each alpha release was built for.
const ALPHA_PLATFORMS: [&str; 2] = ["linux-x64", WINDOWS_PLATFORM];

/// What the names of an alpha's installer, its licences and its checksum list put after the
/// version and platform.
const ALPHA_SETUP: &str = "-setup";

/// The alpha installer of Windows, after its version, platform and `-setup`.
const ALPHA_WINDOWS_SETUP: &str = ".exe";

/// The alpha installer of every other platform, an archive.
const ALPHA_ARCHIVE_SETUP: &str = ".tar.gz";

/// The alpha installer's licences, after its version, platform and `-setup`.
const ALPHA_LICENCES_SUFFIX: &str = "-licenses.zip";

/// An alpha's runtime zip, after its version and platform.
const ALPHA_RUNTIME_SUFFIX: &str = "-runtime.zip";

/// A release row as GitHub's list gives one, for the feed's tests. Every field may be changed to
/// make the row a test needs, a malformed one included.
#[derive(Clone, Debug, PartialEq)]
pub struct ReleaseRow {
    /// The release's tag, `v` and its version.
    pub tag: String,
    /// Whether the release is a draft, which the list shows only to the repository's writers.
    pub draft: bool,
    /// Whether GitHub labels the release a prerelease.
    pub prerelease: bool,
    /// The release's notes, or `None` for a release without them.
    pub body: Option<String>,
    /// The release's assets, in the order GitHub lists them.
    pub assets: Vec<AssetRow>,
}

/// An asset of a release row. The size, digest and URL are JSON values, so that a test can make
/// one `null`, `-1` or text.
#[derive(Clone, Debug, PartialEq)]
pub struct AssetRow {
    /// The asset's file name.
    pub name: String,
    /// Whether GitHub holds the asset whole: `uploaded`, or `new` while it is being uploaded.
    pub state: String,
    /// The asset's length in bytes.
    pub size: Value,
    /// The asset's SHA-256 digest as GitHub gives it, `sha256:` and the lower-case hexadecimal.
    pub digest: Value,
    /// Where the asset is downloaded from, the row's `browser_download_url`.
    pub url: Value,
}

impl ReleaseRow {
    /// A new-line release of `version` for `platform` (`windows-x64`, `linux-x64`) served from
    /// `base` (`https://github.com/` or a loopback base): its launcher asset holding `launcher`,
    /// its checksum list naming it, and, given one, its signature. A version with prerelease
    /// identifiers is labelled a prerelease. Panics unless `base` ends with `/`.
    pub fn new_line(
        base: &str,
        version: &str,
        platform: &str,
        launcher: &[u8],
        signature: Option<&[u8]>,
    ) -> ReleaseRow {
        let tag = format!("{TAG_PREFIX}{version}");
        let suffix = if platform == WINDOWS_PLATFORM {
            WINDOWS_LAUNCHER_SUFFIX
        } else {
            ARCHIVE_LAUNCHER_SUFFIX
        };
        let launcher_name = format!("{ASSET_STEM}-{version}-{platform}{suffix}");
        let launcher = AssetRow::holding(base, &tag, &launcher_name, launcher);
        let list_name = format!("{ASSET_STEM}-{version}-{platform}{CHECKSUMS_SUFFIX}");
        let list = AssetRow::holding(base, &tag, &list_name, &checksum_lines([&launcher]));
        let mut assets = vec![launcher, list];
        if let Some(signature) = signature {
            let signature_name = format!("{list_name}{SIGNATURE_SUFFIX}");
            assets.push(AssetRow::holding(base, &tag, &signature_name, signature));
        }
        ReleaseRow {
            tag,
            draft: false,
            prerelease: has_prerelease(version),
            body: None,
            assets,
        }
    }

    /// A release of the alpha installer, as each of them is published: for both platforms the
    /// runtime zip, the setup, its licences zip and the checksum list of those three, nothing of
    /// the new line, and no prerelease label. Each asset holds a few bytes of its own name, which
    /// its size and digest give, and each list lists them as release.py lists an alpha's files.
    pub fn alpha(version: &str) -> ReleaseRow {
        let tag = format!("{TAG_PREFIX}{version}");
        let mut assets = Vec::new();
        for platform in ALPHA_PLATFORMS {
            let stem = format!("{ASSET_STEM}-{version}-{platform}");
            let setup = if platform == WINDOWS_PLATFORM {
                ALPHA_WINDOWS_SETUP
            } else {
                ALPHA_ARCHIVE_SETUP
            };
            let files: Vec<AssetRow> = [
                format!("{stem}{ALPHA_SETUP}{setup}"),
                format!("{stem}{ALPHA_SETUP}{ALPHA_LICENCES_SUFFIX}"),
                format!("{stem}{ALPHA_RUNTIME_SUFFIX}"),
            ]
            .iter()
            .map(|name| AssetRow::holding(GITHUB_BASE, &tag, name, &stand_in(name)))
            .collect();
            let list = checksum_lines(&files);
            let list_name = format!("{stem}{ALPHA_SETUP}{CHECKSUMS_SUFFIX}");
            assets.push(AssetRow::holding(GITHUB_BASE, &tag, &list_name, &list));
            assets.extend(files);
        }
        assets.sort_by(|one, other| one.name.cmp(&other.name));
        ReleaseRow {
            tag,
            draft: false,
            prerelease: false,
            body: None,
            assets,
        }
    }

    /// The row as the list gives it: the release's tag, whether it is a draft or a prerelease, its
    /// assets and its notes (`null` without them), under GitHub's keys.
    pub fn to_json(&self) -> Value {
        let mut row = Map::new();
        row.insert("tag_name".to_owned(), Value::from(self.tag.clone()));
        row.insert("draft".to_owned(), Value::from(self.draft));
        row.insert("prerelease".to_owned(), Value::from(self.prerelease));
        row.insert(
            "assets".to_owned(),
            Value::Array(self.assets.iter().map(AssetRow::to_json).collect()),
        );
        row.insert(
            "body".to_owned(),
            self.body.clone().map_or(Value::Null, Value::from),
        );
        Value::Object(row)
    }

    /// The checksum list of the row's files, as release.py writes one: a line `<hex>  <name>` for
    /// each asset that is neither a checksum list nor a signature, in the row's order, each ended
    /// by LF. Panics when such an asset's digest is no `sha256:` text.
    pub fn checksum_list(&self) -> Vec<u8> {
        checksum_lines(self.assets.iter().filter(|asset| {
            !asset.name.ends_with(CHECKSUMS_SUFFIX) && !asset.name.ends_with(SIGNATURE_SUFFIX)
        }))
    }
}

impl AssetRow {
    /// An uploaded asset of the release tagged `tag`, named `name` and holding `data`, served
    /// from `base`.
    fn holding(base: &str, tag: &str, name: &str, data: &[u8]) -> AssetRow {
        assert!(
            base.ends_with('/'),
            "a base URL ends with '/', not {base:?}"
        );
        let url = format!(
            "{base}{REPOSITORY}/releases/download/{}/{}",
            escaped(tag),
            escaped(name)
        );
        AssetRow {
            name: name.to_owned(),
            state: UPLOADED.to_owned(),
            size: Value::from(data.len() as u64),
            digest: Value::from(format!("{DIGEST_PREFIX}{}", sha256_hex(data))),
            url: Value::from(url),
        }
    }

    /// The asset as the list gives it, under GitHub's keys.
    fn to_json(&self) -> Value {
        let mut asset = Map::new();
        asset.insert("name".to_owned(), Value::from(self.name.clone()));
        asset.insert("state".to_owned(), Value::from(self.state.clone()));
        asset.insert("size".to_owned(), self.size.clone());
        asset.insert("digest".to_owned(), self.digest.clone());
        asset.insert("browser_download_url".to_owned(), self.url.clone());
        Value::Object(asset)
    }
}

/// A page of GitHub's releases list holding `rows`: their JSON array.
pub fn page(rows: &[ReleaseRow]) -> Vec<u8> {
    let rows = Value::Array(rows.iter().map(ReleaseRow::to_json).collect());
    serde_json::to_vec(&rows).expect("a JSON value always has a JSON form")
}

/// The lines of a checksum list of `files`, in their order.
fn checksum_lines<'a>(files: impl IntoIterator<Item = &'a AssetRow>) -> Vec<u8> {
    files
        .into_iter()
        .map(|file| {
            let hex = file
                .digest
                .as_str()
                .and_then(|digest| digest.strip_prefix(DIGEST_PREFIX))
                .unwrap_or_else(|| panic!("{} has no SHA-256 digest to list", file.name));
            format!("{hex}{LIST_SEPARATOR}{}\n", file.name)
        })
        .collect::<String>()
        .into_bytes()
}

/// Whether `version` carries prerelease identifiers: a `-` before any build metadata.
fn has_prerelease(version: &str) -> bool {
    version
        .split_once('+')
        .map_or(version, |(precedence, _)| precedence)
        .contains('-')
}

/// `text` as a download URL's path writes it: its `+` as `%2B`, as GitHub writes them.
fn escaped(text: &str) -> String {
    text.replace('+', ESCAPED_PLUS)
}

/// The bytes an alpha asset holds in place of its real contents.
fn stand_in(name: &str) -> Vec<u8> {
    format!("stand-in for {name}\n").into_bytes()
}

#[cfg(test)]
mod tests {
    use serde_json::{Value, json};

    use super::{AssetRow, ReleaseRow, page};
    use crate::runtime::sha256_hex;

    /// The platforms a release is built for, and the suffix of each one's launcher asset.
    const PLATFORMS: [(&str, &str); 2] = [
        ("windows-x64", "-launcher.exe"),
        ("linux-x64", "-launcher.tar.gz"),
    ];

    /// The bases a row is served from: GitHub's, and a loopback feed's.
    const BASES: [&str; 2] = ["https://github.com/", "http://127.0.0.1:8123/"];

    /// The download URL of `name` in release `tag` from `base`, as GitHub writes it.
    fn url(base: &str, tag: &str, name: &str) -> String {
        format!(
            "{base}sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/{}/{}",
            tag.replace('+', "%2B"),
            name.replace('+', "%2B")
        )
    }

    /// What an asset of `name` holding `data` in release `tag` from `base` must be.
    fn asset(base: &str, tag: &str, name: &str, data: &[u8]) -> AssetRow {
        AssetRow {
            name: name.to_owned(),
            state: "uploaded".to_owned(),
            size: json!(data.len()),
            digest: json!(format!("sha256:{}", sha256_hex(data))),
            url: json!(url(base, tag, name)),
        }
    }

    #[test]
    fn a_new_line_row_names_its_assets_and_their_digests() {
        let launcher = b"a launcher file".as_slice();
        let signature = b"untrusted comment: a signature\n".as_slice();
        for base in BASES {
            for (platform, suffix) in PLATFORMS {
                for (version, prerelease) in [("1.0.0-beta", true), ("1.0.0+build.1", false)] {
                    let tag = format!("v{version}");
                    let launcher_name = format!("GauntletDarkLegacy-{version}-{platform}{suffix}");
                    let list_name = format!("GauntletDarkLegacy-{version}-{platform}.sha256");
                    let list = format!("{}  {launcher_name}\n", sha256_hex(launcher)).into_bytes();

                    let row = ReleaseRow::new_line(base, version, platform, launcher, None);
                    let case = format!("{base} {platform} {version}");
                    assert_eq!(row.tag, tag, "{case}");
                    assert!(!row.draft, "{case}");
                    assert_eq!(row.prerelease, prerelease, "{case}");
                    assert_eq!(row.body, None, "{case}");
                    assert_eq!(
                        row.assets,
                        [
                            asset(base, &tag, &launcher_name, launcher),
                            asset(base, &tag, &list_name, &list),
                        ],
                        "{case}"
                    );
                    assert_eq!(row.checksum_list(), list, "{case}");

                    // A signature follows the list, named after it, and the list still names the
                    // launcher alone.
                    let signed =
                        ReleaseRow::new_line(base, version, platform, launcher, Some(signature));
                    assert_eq!(signed.assets[..2], row.assets[..], "{case}");
                    assert_eq!(
                        signed.assets[2],
                        asset(base, &tag, &format!("{list_name}.sig"), signature),
                        "{case}"
                    );
                    assert_eq!(signed.assets.len(), 3, "{case}");
                    assert_eq!(signed.checksum_list(), list, "{case}");
                }
            }
        }

        // Build metadata's `+` is escaped in the URL, but kept in the tag and the names.
        let row = ReleaseRow::new_line(BASES[0], "1.0.0+build.1", "linux-x64", launcher, None);
        assert_eq!(row.tag, "v1.0.0+build.1");
        assert_eq!(
            row.assets[0].url,
            "https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/\
             v1.0.0%2Bbuild.1/GauntletDarkLegacy-1.0.0%2Bbuild.1-linux-x64-launcher.tar.gz"
        );
        assert_eq!(
            row.assets[0].name,
            "GauntletDarkLegacy-1.0.0+build.1-linux-x64-launcher.tar.gz"
        );
        // Prerelease identifiers before build metadata label a prerelease; a `-` within the
        // metadata does not.
        for (version, prerelease) in [
            ("1.0.0-rc.1+build.2", true),
            ("1.0.0+build-2", false),
            ("2.0.0", false),
        ] {
            let row = ReleaseRow::new_line(BASES[0], version, "linux-x64", launcher, None);
            assert_eq!(row.prerelease, prerelease, "{version}");
        }
    }

    #[test]
    fn a_row_and_a_page_take_githubs_keys() {
        let mut row = ReleaseRow::new_line(
            "https://github.com/",
            "1.0.0-beta.2",
            "windows-x64",
            b"launcher",
            None,
        );
        let launcher = row.assets[0].clone();
        let list = row.assets[1].clone();
        assert_eq!(
            row.to_json(),
            json!({
                "tag_name": "v1.0.0-beta.2",
                "draft": false,
                "prerelease": true,
                "assets": [
                    {
                        "name": launcher.name,
                        "state": "uploaded",
                        "size": 8,
                        "digest": launcher.digest,
                        "browser_download_url": launcher.url,
                    },
                    {
                        "name": list.name,
                        "state": "uploaded",
                        "size": list.size,
                        "digest": list.digest,
                        "browser_download_url": list.url,
                    },
                ],
                "body": null,
            })
        );

        // What a test changes is what the row gives, values of another type included.
        row.draft = true;
        row.body = Some("## Added\n- Updates\n".to_owned());
        row.assets[0].size = json!(-1);
        row.assets[0].digest = Value::Null;
        row.assets[1].url = json!("https://evil.example/payload.zip");
        row.assets[1].state = "new".to_owned();
        let json = row.to_json();
        assert_eq!(json["draft"], true);
        assert_eq!(json["body"], "## Added\n- Updates\n");
        assert_eq!(json["assets"][0]["size"], -1);
        assert_eq!(json["assets"][0]["digest"], Value::Null);
        assert_eq!(
            json["assets"][1]["browser_download_url"],
            "https://evil.example/payload.zip"
        );
        assert_eq!(json["assets"][1]["state"], "new");

        // A page is the rows' JSON array, in their order, and an empty page an empty array.
        let alpha = ReleaseRow::alpha("4.0.0-alpha.3");
        let read: Value = serde_json::from_slice(&page(&[row.clone(), alpha.clone()])).unwrap();
        assert_eq!(read, json!([row.to_json(), alpha.to_json()]));
        assert_eq!(page(&[]), b"[]");
    }

    #[test]
    fn an_alpha_row_carries_no_new_line_name() {
        let version = "4.0.0-alpha.3";
        let row = ReleaseRow::alpha(version);
        assert_eq!(row.tag, "v4.0.0-alpha.3");
        assert!(!row.draft);
        assert!(
            !row.prerelease,
            "the alpha releases carry no prerelease label"
        );
        assert_eq!(row.body, None);

        // The assets GitHub lists for every alpha release, in its order.
        let names: Vec<&str> = row.assets.iter().map(|asset| asset.name.as_str()).collect();
        assert_eq!(
            names,
            [
                "GauntletDarkLegacy-4.0.0-alpha.3-linux-x64-runtime.zip",
                "GauntletDarkLegacy-4.0.0-alpha.3-linux-x64-setup-licenses.zip",
                "GauntletDarkLegacy-4.0.0-alpha.3-linux-x64-setup.sha256",
                "GauntletDarkLegacy-4.0.0-alpha.3-linux-x64-setup.tar.gz",
                "GauntletDarkLegacy-4.0.0-alpha.3-windows-x64-runtime.zip",
                "GauntletDarkLegacy-4.0.0-alpha.3-windows-x64-setup-licenses.zip",
                "GauntletDarkLegacy-4.0.0-alpha.3-windows-x64-setup.exe",
                "GauntletDarkLegacy-4.0.0-alpha.3-windows-x64-setup.sha256",
            ]
        );

        // None is a name of the new line, for either platform.
        for (platform, suffix) in PLATFORMS {
            let new_line = ReleaseRow::new_line(
                "https://github.com/",
                version,
                platform,
                b"launcher",
                Some(b"signature"),
            );
            assert!(new_line.assets[0].name.ends_with(suffix));
            for asset in &new_line.assets {
                assert!(
                    !names.contains(&asset.name.as_str()),
                    "{} is an alpha name",
                    asset.name
                );
            }
        }
        assert!(
            names
                .iter()
                .all(|name| !name.ends_with("-launcher.exe") && !name.ends_with("-launcher.tar.gz"))
        );

        // Each asset holds a stand-in, which its size and digest give, at GitHub's URL; each
        // platform's list lists its setup, licences and runtime as release.py lists them.
        let stand_in = |name: &str| format!("stand-in for {name}\n").into_bytes();
        for platform in ["linux-x64", "windows-x64"] {
            let stem = format!("GauntletDarkLegacy-{version}-{platform}");
            let setup = if platform == "windows-x64" {
                format!("{stem}-setup.exe")
            } else {
                format!("{stem}-setup.tar.gz")
            };
            let listed = [
                setup,
                format!("{stem}-setup-licenses.zip"),
                format!("{stem}-runtime.zip"),
            ];
            let list: String = listed
                .iter()
                .map(|name| format!("{}  {name}\n", sha256_hex(&stand_in(name))))
                .collect();
            let list_name = format!("{stem}-setup.sha256");
            let found = |name: &str| row.assets.iter().find(|asset| asset.name == name).unwrap();
            assert_eq!(
                *found(&list_name),
                asset("https://github.com/", &row.tag, &list_name, list.as_bytes())
            );
            for name in &listed {
                assert_eq!(
                    *found(name),
                    asset("https://github.com/", &row.tag, name, &stand_in(name))
                );
            }
        }

        // The row's own list is of every file both platforms carry, lists left out.
        let lines = String::from_utf8(row.checksum_list()).unwrap();
        assert_eq!(lines.lines().count(), 6, "{lines}");
        assert!(!lines.contains(".sha256"), "{lines}");
    }

    #[test]
    #[should_panic(expected = "has no SHA-256 digest to list")]
    fn a_list_of_an_asset_without_a_digest_panics() {
        let mut row = ReleaseRow::alpha("1.4.1-alpha.3");
        row.assets[0].digest = Value::Null;
        let _ = row.checksum_list();
    }

    #[test]
    #[should_panic(expected = "a base URL ends with '/'")]
    fn a_base_without_its_slash_panics() {
        let _ = ReleaseRow::new_line(
            "http://127.0.0.1:8123",
            "1.0.0",
            "linux-x64",
            b"launcher",
            None,
        );
    }
}
