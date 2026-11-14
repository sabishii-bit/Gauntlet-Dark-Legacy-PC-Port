//! Release versions, read and ordered the way the release scripts read and order them, so that
//! the launcher and the publisher agree on what a version is and on which release is newer.

use std::cmp::Ordering;
use std::fmt;
use std::hash::{Hash, Hasher};
use std::str::FromStr;

/// A release's semantic version. Versions that differ only in build metadata are the same
/// release: they compare equal, hash alike and neither is newer than the other.
#[derive(Clone, Debug)]
pub struct Version(semver::Version);

impl Version {
    /// Reads a version as a release's own files record it: a semantic version and nothing else,
    /// with no `v` and no whitespace around it.
    pub fn parse(text: &str) -> Result<Version, VersionError> {
        semver::Version::parse(text)
            .map(Version)
            .map_err(|_| VersionError { text: text.into() })
    }

    /// Reads a version as a release tag names it: at most one leading lowercase `v`, then a
    /// version as [`Version::parse`] reads it.
    pub fn parse_tag(text: &str) -> Result<Version, VersionError> {
        let version = text.strip_prefix('v').unwrap_or(text);
        Version::parse(version).map_err(|_| VersionError { text: text.into() })
    }

    /// Whether the version has prerelease identifiers, as every beta and release candidate has.
    pub fn is_prerelease(&self) -> bool {
        !self.0.pre.is_empty()
    }

    /// The whole version, with the build metadata that its order and equality leave out.
    pub fn semver(&self) -> &semver::Version {
        &self.0
    }
}

impl FromStr for Version {
    type Err = VersionError;

    fn from_str(text: &str) -> Result<Version, VersionError> {
        Version::parse(text)
    }
}

/// The text the version was read from, build metadata included; a tag's `v` is not part of it.
impl fmt::Display for Version {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.0, formatter)
    }
}

impl PartialEq for Version {
    fn eq(&self, other: &Version) -> bool {
        self.cmp(other).is_eq()
    }
}

impl Eq for Version {}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Version) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// Precedence: build metadata never makes one release newer than another.
impl Ord for Version {
    fn cmp(&self, other: &Version) -> Ordering {
        self.0.cmp_precedence(&other.0)
    }
}

/// Hashes what precedence compares, so that equal versions hash alike.
impl Hash for Version {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let version = &self.0;
        (version.major, version.minor, version.patch, &version.pre).hash(state);
    }
}

/// A text that is not a semantic version, or not a tag that names one.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("Invalid semantic version: {text:?}")]
pub struct VersionError {
    /// The text as it was given, a tag's `v` included.
    pub text: Box<str>,
}

#[cfg(test)]
mod tests {
    use std::cmp::Ordering;
    use std::hash::{BuildHasher, RandomState};

    use serde::Deserialize;

    use super::Version;

    /// The version cases the release scripts' tests read too.
    const VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/semver-vectors.json"
    ));

    /// The launcher's first release, where every installation it makes starts.
    const FIRST_LAUNCHER_RELEASE: &str = "1.0.0-beta";

    /// The shared file's groups. An unknown group is refused, so none goes untested here.
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields, rename_all = "camelCase")]
    struct Vectors {
        strict: Strict,
        tag: Tags,
        ascending: Vec<Vec<String>>,
        equal: Vec<[String; 2]>,
        prerelease: Prereleases,
        beta_boundary: BetaBoundary,
    }

    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Strict {
        valid: Vec<String>,
        invalid: Vec<String>,
    }

    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Tags {
        invalid: Vec<String>,
    }

    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Prereleases {
        yes: Vec<String>,
        no: Vec<String>,
    }

    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct BetaBoundary {
        below: Vec<String>,
        above: Vec<String>,
    }

    fn vectors() -> Vectors {
        serde_json::from_str(VECTORS).expect("the shared vectors have the groups read here")
    }

    /// A group's cases, refusing an empty group that would pass without testing anything.
    fn cases<'a, T>(group: &'a [T], name: &str) -> &'a [T] {
        assert!(
            !group.is_empty(),
            "the shared vectors' {name} group is empty"
        );
        group
    }

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    fn tag(text: &str) -> Version {
        Version::parse_tag(text).unwrap_or_else(|error| panic!("{error}"))
    }

    #[test]
    fn strict_versions_match_the_shared_vectors() {
        let vectors = vectors();
        for text in cases(&vectors.strict.valid, "strict.valid") {
            let read: Version = text.parse().unwrap_or_else(|error| panic!("{error}"));
            assert_eq!(read.semver(), version(text).semver(), "{text:?}");
        }
        for text in cases(&vectors.strict.invalid, "strict.invalid") {
            let error = Version::parse(text).expect_err(text);
            assert_eq!(&*error.text, text.as_str());
            assert_eq!(text.parse::<Version>().expect_err(text), error);
        }
        let error = Version::parse("main").expect_err("main is a branch, not a version");
        assert_eq!(error.to_string(), r#"Invalid semantic version: "main""#);
    }

    #[test]
    fn tags_take_one_optional_lowercase_v() {
        let vectors = vectors();
        for text in cases(&vectors.strict.valid, "strict.valid") {
            let prefixed = format!("v{text}");
            assert!(Version::parse(&prefixed).is_err(), "{prefixed:?} is a tag");
            let (plain, tagged) = (tag(text), tag(&prefixed));
            assert_eq!(plain, tagged);
            assert_eq!(plain.semver(), tagged.semver(), "{text:?}");
        }
        assert_eq!(tag("1.2.3").semver(), version("1.2.3").semver());
        let unprefixed = vectors
            .strict
            .invalid
            .iter()
            .filter(|text| !text.starts_with('v'));
        for text in unprefixed.chain(cases(&vectors.tag.invalid, "tag.invalid")) {
            let error = Version::parse_tag(text).expect_err(text);
            assert_eq!(
                &*error.text,
                text.as_str(),
                "the refusal names the tag as given"
            );
        }
    }

    #[test]
    fn ascending_chains_follow_precedence() {
        let vectors = vectors();
        for chain in cases(&vectors.ascending, "ascending") {
            assert!(chain.len() >= 2, "{chain:?} orders nothing");
            let chain: Vec<Version> = chain.iter().map(String::as_str).map(version).collect();
            for (index, older) in chain.iter().enumerate() {
                for newer in &chain[index + 1..] {
                    assert!(older < newer, "{older} sorts before {newer}");
                    assert!(newer > older, "{newer} sorts after {older}");
                    assert_ne!(older, newer);
                    assert_eq!(older.cmp(newer), Ordering::Less, "{older} < {newer}");
                    assert_eq!(newer.cmp(older), Ordering::Greater, "{newer} > {older}");
                }
            }
        }
    }

    #[test]
    fn build_metadata_never_orders_or_distinguishes() {
        let vectors = vectors();
        let hasher = RandomState::new();
        for [first, second] in cases(&vectors.equal, "equal") {
            let (first, second) = (version(first), version(second));
            assert_ne!(
                first.semver().build,
                second.semver().build,
                "{first} and {second} differ in their build metadata"
            );
            assert_eq!(first, second);
            assert_eq!(second, first);
            assert_eq!(first.cmp(&second), Ordering::Equal, "{first} = {second}");
            assert_eq!(first.partial_cmp(&second), Some(Ordering::Equal));
            assert_eq!(hasher.hash_one(&first), hasher.hash_one(&second));
        }
    }

    #[test]
    fn prerelease_follows_the_vectors() {
        let vectors = vectors();
        for text in cases(&vectors.prerelease.yes, "prerelease.yes") {
            assert!(tag(text).is_prerelease(), "{text:?} is a prerelease");
        }
        for text in cases(&vectors.prerelease.no, "prerelease.no") {
            assert!(!tag(text).is_prerelease(), "{text:?} is not a prerelease");
        }
    }

    #[test]
    fn the_alpha_line_sorts_around_the_new_line() {
        // The published alpha tags fall on both sides of the launcher's first release, so being
        // newer never tells the two lines apart.
        let vectors = vectors();
        let first = version(FIRST_LAUNCHER_RELEASE);
        for text in cases(&vectors.beta_boundary.below, "betaBoundary.below") {
            assert!(tag(text) < first, "{text} sorts before {first}");
        }
        for text in cases(&vectors.beta_boundary.above, "betaBoundary.above") {
            assert!(tag(text) > first, "{text} sorts after {first}");
        }
    }

    #[test]
    fn numbers_beyond_u64_are_refused() {
        // The release scripts read any number here, so the shared vectors leave such numbers out.
        for text in [
            "18446744073709551616.0.0",
            "0.18446744073709551616.0",
            "0.0.18446744073709551616",
        ] {
            assert!(Version::parse(text).is_err(), "{text:?}");
        }
        let largest = "18446744073709551615.18446744073709551615.18446744073709551615";
        assert_eq!(version(largest).to_string(), largest);
        // Prerelease numbers compare digit by digit, so they have no such bound.
        assert!(version("1.0.0-18446744073709551615") < version("1.0.0-18446744073709551616"));
    }

    #[test]
    fn display_keeps_the_parsed_text() {
        let vectors = vectors();
        for text in cases(&vectors.strict.valid, "strict.valid") {
            assert_eq!(version(text).to_string(), *text);
            assert_eq!(
                tag(&format!("v{text}")).to_string(),
                *text,
                "a tag's v is not kept"
            );
        }
    }
}
