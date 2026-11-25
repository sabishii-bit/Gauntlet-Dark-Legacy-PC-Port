//! The keys the launcher trusts to sign its releases, and the check an update makes against them.
//! A release's checksum list gives the digest of every file the release carries, so a minisign
//! signature of that list by one of the keys, naming the list and the release it was made for,
//! vouches for each file an update then downloads.

use std::collections::BTreeSet;

use minisign_verify::{Error as MinisignError, PublicKey, Signature};

use crate::{MAX_LIST_LEN, ReleaseError, Version};

/// The public keys of the release signing keys, each a minisign public key's base64 line. Empty
/// until the keys are made and put here.
#[cfg(any(test, not(feature = "test-hooks")))]
const RELEASE_KEYS: &[&str] = &[];

/// The public half of the tests' own key, whose secret half is a fixed seed that no release
/// trusts: only a build with the test hooks trusts it.
#[cfg(any(test, feature = "test-hooks"))]
pub const TEST_PUBLIC_KEY: &str = "RWRHREwtVEVTVAIHRlatKKopKz17ZiTblA/hZSC2IbuaChLYupiY8oUG";

/// What a signature's first line begins with: minisign writes the untrusted comment there and
/// requires it, though the decoder takes any line.
const UNTRUSTED_COMMENT_PREFIX: &str = "untrusted comment: ";

/// What parts the fields of a trusted comment.
const FIELD_SEPARATOR: char = '\t';

/// What parts a field's key from its value.
const KEY_SEPARATOR: char = ':';

/// The trusted comment's field that names the file signed: the checksum list.
const FILE_FIELD: &str = "file";

/// The trusted comment's field that names the release the list was signed for.
const VERSION_FIELD: &str = "version";

/// Why a signature that no key of the keyring made is refused.
const NO_TRUSTED_KEY: &str = "signed by no key this launcher trusts";

/// The public keys a launcher trusts to sign its releases' checksum lists, tried in turn.
#[derive(Clone, Debug)]
#[cfg_attr(test, derive(PartialEq, Eq))]
pub struct Keyring {
    /// The keys, in the order a signature is checked against them.
    keys: Vec<PublicKey>,
}

impl Keyring {
    /// The keys built into this launcher: the release signing keys; in a build with the test
    /// hooks, the tests' own key alone, so that a rehearsal of an update checks the signature of a
    /// release that key signed, and nothing the release keys signed.
    pub fn release() -> Keyring {
        #[cfg(not(feature = "test-hooks"))]
        let keys = RELEASE_KEYS;
        #[cfg(feature = "test-hooks")]
        let keys = &[TEST_PUBLIC_KEY];
        Keyring::of(keys).expect("every key built in is a minisign public key")
    }

    /// A keyring of no key, by which no signature verifies.
    pub fn empty() -> Keyring {
        Keyring { keys: Vec::new() }
    }

    /// A keyring of `keys`, each a minisign public key's base64 line alone, tried in that order.
    /// A line that is no such key is [`ReleaseError::Signature`], naming its place.
    pub fn of(keys: &[&str]) -> Result<Keyring, ReleaseError> {
        let keys = keys
            .iter()
            .enumerate()
            .map(|(index, line)| {
                PublicKey::from_base64(line).map_err(|_| {
                    refused(format!(
                        "key {} of the keyring is no minisign public key",
                        index + 1
                    ))
                })
            })
            .collect::<Result<_, _>>()?;
        Ok(Keyring { keys })
    }

    /// Whether the keyring holds no key, so that no signature can be checked against it.
    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }

    /// Checks `signature`, a `.sig` file's text, over `list`, the checksum list named
    /// `list_name`, of release `version`: a minisign signature of at most [`MAX_LIST_LEN`] bytes,
    /// prehashed, made by one of the keys, whose trusted comment's tab-separated `key:value`
    /// fields give each key once, `file` as `list_name` and `version` as `version`'s text. Any
    /// other signature is [`ReleaseError::Signature`], saying why.
    pub fn verify_list(
        &self,
        list: &[u8],
        list_name: &str,
        version: &Version,
        signature: &[u8],
    ) -> Result<(), ReleaseError> {
        let signature = decoded(signature)?;
        self.signer(list, &signature)?;
        // The comment is read only once a trusted key has vouched for it, so nothing that key did
        // not sign is ever taken for what the list was signed for.
        check_comment(signature.trusted_comment(), list_name, version)
    }

    /// The place in the keyring of the key that made `signature` over `list`: the first key of
    /// the id the signature names, which must then verify it.
    fn signer(&self, list: &[u8], signature: &Signature) -> Result<usize, ReleaseError> {
        for (index, key) in self.keys.iter().enumerate() {
            // Only the prehashed form, an Ed25519 signature of the list's BLAKE2b-512, is taken,
            // which minisign makes unless it is asked for the legacy one.
            match key.verify(list, signature, false) {
                Ok(()) => return Ok(index),
                Err(MinisignError::UnexpectedKeyId) => {}
                Err(error) => return Err(refused(refusal_of(error))),
            }
        }
        Err(refused(NO_TRUSTED_KEY))
    }
}

/// The signature `bytes` hold, as minisign writes one: at most [`MAX_LIST_LEN`] bytes of UTF-8
/// text, a line each for the untrusted comment, the signature, the trusted comment and the global
/// signature over the other two.
fn decoded(bytes: &[u8]) -> Result<Signature, ReleaseError> {
    if bytes.len() as u64 > MAX_LIST_LEN {
        return Err(refused(format!("it holds more than {MAX_LIST_LEN} bytes")));
    }
    let text = std::str::from_utf8(bytes).map_err(|_| refused("it is not UTF-8 text"))?;
    let begins_untrusted = text
        .lines()
        .next()
        .is_some_and(|line| line.starts_with(UNTRUSTED_COMMENT_PREFIX));
    if !begins_untrusted {
        return Err(refused("its first line is not an untrusted comment"));
    }
    Signature::decode(text).map_err(|error| refused(refusal_of(error)))
}

/// Checks that `comment`, a verified signature's trusted comment, is tab-separated `key:value`
/// fields, each key lower-case ASCII letters and none twice, whose `file` is `list_name` and whose
/// `version` is `version`'s text. The signature covers the comment, so the list cannot stand in
/// for another release's or another platform's.
fn check_comment(comment: &str, list_name: &str, version: &Version) -> Result<(), ReleaseError> {
    let mut keys = BTreeSet::new();
    let mut file = None;
    let mut signed_version = None;
    for field in comment.split(FIELD_SEPARATOR) {
        let Some((key, value)) = field.split_once(KEY_SEPARATOR) else {
            return Err(refused(format!(
                "its trusted comment holds {field:?}, which is no key and value"
            )));
        };
        if key.is_empty() || !key.bytes().all(|byte| byte.is_ascii_lowercase()) {
            return Err(refused(format!(
                "its trusted comment holds the key {key:?}, which is not lower-case letters"
            )));
        }
        if !keys.insert(key) {
            return Err(refused(format!("its trusted comment gives {key} twice")));
        }
        match key {
            FILE_FIELD => file = Some(value),
            VERSION_FIELD => signed_version = Some(value),
            _ => {}
        }
    }
    let version = version.to_string();
    for (field, given, wanted) in [
        (FILE_FIELD, file, list_name),
        (VERSION_FIELD, signed_version, version.as_str()),
    ] {
        match given {
            None => return Err(refused(format!("its trusted comment gives no {field}"))),
            Some(given) if given != wanted => {
                return Err(refused(format!(
                    "its trusted comment gives the {field} {given:?}, not {wanted}"
                )));
            }
            Some(_) => {}
        }
    }
    Ok(())
}

/// Why minisign refused a signature with `error`, in the launcher's words.
fn refusal_of(error: MinisignError) -> &'static str {
    match error {
        MinisignError::InvalidEncoding => "it is not the four lines of a minisign signature",
        MinisignError::UnsupportedAlgorithm => {
            "it names a signature algorithm other than minisign's Ed25519"
        }
        MinisignError::UnexpectedAlgorithm | MinisignError::UnsupportedLegacyMode => {
            "it is a legacy signature, made over the list itself rather than its hash"
        }
        MinisignError::InvalidSignature => {
            "it does not verify over this list and its trusted comment"
        }
        MinisignError::UnexpectedKeyId => NO_TRUSTED_KEY,
        MinisignError::IoError(_) => "it could not be read",
    }
}

/// The refusal of a signature, for `detail`.
fn refused(detail: impl Into<Box<str>>) -> ReleaseError {
    ReleaseError::Signature {
        detail: detail.into(),
    }
}

#[cfg(test)]
mod tests {
    use gdl_fixtures::{TEST_KEY_ID, sign_with_test_key, test_public_key};
    use gdl_media::Sha256Digest;
    use minisign_verify::PublicKey;

    use super::{
        Keyring, MAX_LIST_LEN, NO_TRUSTED_KEY, RELEASE_KEYS, TEST_PUBLIC_KEY,
        UNTRUSTED_COMMENT_PREFIX, decoded,
    };
    use crate::{Platform, ReleaseError, Version, checksums_asset, launcher_asset, licences_asset};

    /// What every refusal of a signature says before why.
    const REFUSAL_PREFIX: &str =
        "The release's checksum list is not signed as this launcher requires: ";

    /// Why a signature that does not verify is refused.
    const NOT_VERIFIED: &str = "it does not verify over this list and its trusted comment";

    /// Why a legacy signature is refused.
    const LEGACY: &str = "it is a legacy signature, made over the list itself rather than its hash";

    /// Why text that is not a minisign signature's lines is refused.
    const MALFORMED: &str = "it is not the four lines of a minisign signature";

    /// Why a signature whose first line is no untrusted comment is refused.
    const NO_UNTRUSTED_COMMENT: &str = "its first line is not an untrusted comment";

    /// The release whose checksum list the tests sign.
    const RELEASE: &str = "1.0.0-beta.1";

    /// When the tests' signatures say they were made, as the release signing writes it.
    const TIMESTAMP: &str = "timestamp:1764003600";

    /// minisign's own published test key, from minisign-verify's tests.
    const PUBLISHED_KEY: &str = "RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3";

    /// The published key's legacy signature of `test`, made over the file itself.
    const PUBLISHED_LEGACY_SIGNATURE: &str = "untrusted comment: signature from minisign secret \
        key\n\
        RWQf6LRCGA9i59SLOFxz6NxvASXDJeRtuZykwQepbDEGt87ig1BNpWaVWuNrm73YiIiJbq71Wi+dP9eKL8OC351vwI\
        asSSbXxwA=\n\
        trusted comment: timestamp:1555779966\tfile:test\n\
        QtKMXWyYcwdpZAlPF7tE2ENJkRd1ujvKjlj1m9RtHTBnZPa5WKU5uWRs5GoP5M/VqE81QFuMKI5k/SfNQUaOAA==";

    /// The letters of base64, in the order of the values they stand for.
    const BASE64_ALPHABET: &[u8; 64] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /// How many bytes a minisign public key's line holds: its algorithm tag, its id and its
    /// Ed25519 key.
    const PUBLIC_KEY_LEN: usize = 42;

    /// Where the Ed25519 key begins in those bytes, after the two of the tag and the eight of
    /// the id.
    const KEY_START: usize = 10;

    /// The algorithm tag minisign writes in a public key.
    const PUBLIC_KEY_TAG: &[u8; 2] = b"Ed";

    /// The published key's prehashed signature of `test`, whose trusted comment names no version.
    const PUBLISHED_PREHASHED_SIGNATURE: &str = "untrusted comment: signature from minisign \
        secret key\n\
        RUQf6LRCGA9i559r3g7V1qNyJDApGip8MfqcadIgT9CuhV3EMhHoN1mGTkUidF/z7SrlQgXdy8ofjb7bNJJylDOocrC\
        o8KLzZwo=\n\
        trusted comment: timestamp:1556193335\tfile:test\n\
        y/rUw2y8/hOUYjZU71eHp/Wo1KZ40fGy2VJEDl34XMJM+TX48Ss/17u3IvIfbVR1FkZZSNCisQbuQY+bHwhEBg==";

    /// A release's checksum list, its name and its version, as the tests sign it.
    struct ReleaseList {
        version: Version,
        name: String,
        list: Vec<u8>,
    }

    impl ReleaseList {
        /// The list a release of `text` for `platform` carries: its launcher asset's and its
        /// licences' digests.
        fn of(text: &str, platform: Platform) -> ReleaseList {
            let version = version(text);
            let list = format!(
                "{}  {}\n{}  {}\n",
                Sha256Digest::of(b"the launcher"),
                launcher_asset(&version, platform),
                Sha256Digest::of(b"the licences"),
                licences_asset(&version, platform),
            );
            ReleaseList {
                name: checksums_asset(&version, platform),
                version,
                list: list.into_bytes(),
            }
        }

        /// The test key's signature of the list, as the release signing makes one.
        fn signature(&self) -> String {
            sign_with_test_key(&self.list, &comment(&self.name, &self.version.to_string()))
        }

        /// The test key's signature of the list with `comment` as its trusted comment.
        fn signed(&self, comment: &str) -> String {
            sign_with_test_key(&self.list, comment)
        }

        /// What `keyring` makes of `signature` over the list.
        fn verify(&self, keyring: &Keyring, signature: &str) -> Result<(), ReleaseError> {
            keyring.verify_list(&self.list, &self.name, &self.version, signature.as_bytes())
        }
    }

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The keyring of `keys`, which must all be keys.
    fn keyring(keys: &[&str]) -> Keyring {
        Keyring::of(keys).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The trusted comment the release signing writes for the list `name` of `version`.
    fn comment(name: &str, version: &str) -> String {
        format!("{TIMESTAMP}\tfile:{name}\tversion:{version}")
    }

    /// Why `outcome` refused a signature or a key, as the refusal says it.
    fn refusal(outcome: Result<(), ReleaseError>) -> String {
        match outcome {
            Err(error @ ReleaseError::Signature { .. }) => {
                assert_eq!(error.rule(), "signature");
                let shown = error.to_string();
                let detail = shown
                    .strip_prefix(REFUSAL_PREFIX)
                    .unwrap_or_else(|| panic!("{shown}"));
                detail.to_owned()
            }
            other => panic!("checked as {other:?}"),
        }
    }

    /// `text` with the character at `at` of its line `line` (from nought) replaced by another
    /// base64 letter, so that the line still decodes to as many bytes.
    fn with_letter_changed(text: &str, line: usize, at: usize) -> String {
        let mut lines: Vec<String> = text.lines().map(str::to_owned).collect();
        let letters: Vec<char> = lines[line].chars().collect();
        let other = if letters[at] == 'A' { 'B' } else { 'A' };
        lines[line] = letters
            .iter()
            .enumerate()
            .map(|(index, &letter)| if index == at { other } else { letter })
            .collect();
        lines.iter().map(|line| format!("{line}\n")).collect()
    }

    /// The bytes of a minisign public key's base64 line: its algorithm tag, its id, then its
    /// Ed25519 key.
    fn public_key_bytes(line: &str) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(PUBLIC_KEY_LEN);
        // The bits read and not yet made into a byte, the newest lowest, and how many they are.
        let (mut bits, mut held) = (0u32, 0u32);
        for letter in line.trim_end_matches('=').bytes() {
            let value = BASE64_ALPHABET
                .iter()
                .position(|&each| each == letter)
                .unwrap_or_else(|| panic!("{line:?} is not base64"));
            bits = (bits << 6) | u32::try_from(value).expect("a letter stands for six bits");
            held += 6;
            if held >= 8 {
                held -= 8;
                bytes.push(u8::try_from(bits >> held).expect("eight bits"));
                bits &= (1 << held) - 1;
            }
        }
        assert_eq!(bytes.len(), PUBLIC_KEY_LEN, "{line:?}");
        bytes
    }

    /// Whether any of `lines` holds the test key's Ed25519 key, under whatever id and algorithm
    /// tag: its secret seed is in the fixtures, so anyone can sign with it.
    fn holds_the_test_key(lines: &[&str]) -> bool {
        let test_key = public_key_bytes(TEST_PUBLIC_KEY);
        lines
            .iter()
            .any(|line| public_key_bytes(line)[KEY_START..] == test_key[KEY_START..])
    }

    /// `line` with its letters from `at` replaced by `letters`.
    fn with_letters(line: &str, at: usize, letters: &str) -> String {
        format!("{}{letters}{}", &line[..at], &line[at + letters.len()..])
    }

    #[test]
    fn the_production_keyring_is_empty_and_never_holds_the_test_key() {
        assert!(RELEASE_KEYS.is_empty(), "{RELEASE_KEYS:?}");
        assert!(!holds_the_test_key(RELEASE_KEYS), "{RELEASE_KEYS:?}");
        // Nor do the release keys verify a list the test key signed.
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        assert_eq!(
            refusal(release.verify(&keyring(RELEASE_KEYS), &release.signature())),
            NO_TRUSTED_KEY
        );
    }

    #[test]
    fn the_test_key_is_known_under_any_id_or_tag() {
        // The test key's line is its tag, its id and its key.
        let bytes = public_key_bytes(TEST_PUBLIC_KEY);
        assert_eq!(
            bytes[..KEY_START],
            [PUBLIC_KEY_TAG.as_slice(), TEST_KEY_ID.as_slice()].concat()
        );
        assert!(!holds_the_test_key(&[PUBLISHED_KEY]));

        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let signature = release.signature();
        // The first four letters spell the tag and the id's first byte; the next four, three
        // more bytes of the id, in a key's line and a signature's alike.
        let tagged = with_letters(TEST_PUBLIC_KEY, 0, "RURH");
        let renamed = with_letters(TEST_PUBLIC_KEY, 4, "AAAA");
        let lines: Vec<String> = signature.lines().map(str::to_owned).collect();
        let renamed_signature: String = [
            lines[0].clone(),
            with_letters(&lines[1], 4, "AAAA"),
            lines[2].clone(),
            lines[3].clone(),
        ]
        .iter()
        .map(|line| format!("{line}\n"))
        .collect();
        // Under the prehashed tag the key is no equal of the test key, yet verifies what it
        // signs; under another id it verifies the same signature naming that id, which the
        // signature does not cover.
        assert_ne!(keyring(&[tagged.as_str()]), keyring(&[TEST_PUBLIC_KEY]));
        for (line, signature) in [
            (TEST_PUBLIC_KEY, signature.as_str()),
            (tagged.as_str(), signature.as_str()),
            (renamed.as_str(), renamed_signature.as_str()),
        ] {
            release
                .verify(&keyring(&[line]), signature)
                .unwrap_or_else(|error| panic!("{line}: {error}"));
            assert!(holds_the_test_key(&[PUBLISHED_KEY, line]), "{line}");
        }
    }

    #[cfg(not(feature = "test-hooks"))]
    #[test]
    fn a_release_build_trusts_the_release_keys() {
        assert_eq!(Keyring::release(), keyring(RELEASE_KEYS));
        assert_eq!(Keyring::release().is_empty(), RELEASE_KEYS.is_empty());
    }

    #[cfg(feature = "test-hooks")]
    #[test]
    fn the_test_build_trusts_the_test_key_alone() {
        let built_in = Keyring::release();
        assert_eq!(built_in, keyring(&[TEST_PUBLIC_KEY]));
        // It verifies a list the test key signed, and refuses one another key signed.
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        release
            .verify(&built_in, &release.signature())
            .unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(
            refusal(built_in.verify_list(
                b"test",
                "test",
                &release.version,
                PUBLISHED_PREHASHED_SIGNATURE.as_bytes()
            )),
            NO_TRUSTED_KEY
        );
    }

    #[test]
    fn the_test_key_is_the_fixtures_signer() {
        let derived = test_public_key();
        assert_eq!(
            TEST_PUBLIC_KEY, derived,
            "the fixtures derive the test key's public half as {derived}"
        );
        assert_eq!(keyring(&[TEST_PUBLIC_KEY]).keys.len(), 1);
    }

    #[test]
    fn a_list_signed_by_the_test_key_verifies() {
        let test_key = keyring(&[TEST_PUBLIC_KEY]);
        // Each platform's list, for a beta, a release and a version with build metadata, whose
        // text the trusted comment gives as the list's name does.
        for text in ["1.0.0-beta", RELEASE, "1.0.0+build.1", "1.2.3"] {
            for platform in Platform::ALL {
                let release = ReleaseList::of(text, platform);
                let signature = release.signature();
                release
                    .verify(&test_key, &signature)
                    .unwrap_or_else(|error| panic!("{text} for {platform:?}: {error}"));
                // A signature whose lines end as Windows ends them reads as the same signature.
                release
                    .verify(&test_key, &signature.replace('\n', "\r\n"))
                    .unwrap_or_else(|error| panic!("{text} for {platform:?}, CRLF: {error}"));
            }
        }

        // The two fields in any order, beside other fields or none, a value empty or holding the
        // key's separator.
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let file = format!("file:{}", release.name);
        let version = format!("version:{RELEASE}");
        for comment in [
            format!("{file}\t{version}"),
            format!("{version}\t{file}"),
            format!("{version}\t{TIMESTAMP}\t{file}"),
            format!("{TIMESTAMP}\t{file}\t{version}\tsigner:the release machine"),
            format!("{TIMESTAMP}\t{file}\t{version}\tnote:"),
            format!("{TIMESTAMP}\t{file}\t{version}\tsource:https://example.test:443/list"),
        ] {
            release
                .verify(&test_key, &release.signed(&comment))
                .unwrap_or_else(|error| panic!("{comment:?}: {error}"));
        }
    }

    #[test]
    fn a_changed_list_does_not() {
        let test_key = keyring(&[TEST_PUBLIC_KEY]);
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let signature = release.signature();
        let verify = |list: &[u8], version: &Version, signature: &str| {
            test_key.verify_list(list, &release.name, version, signature.as_bytes())
        };

        // A byte of the list changed, one added or taken away, nothing, or another release's list.
        let last = release.list.len() - 1;
        let mut lists: Vec<Vec<u8>> = [0, 70, last]
            .into_iter()
            .map(|at| {
                let mut list = release.list.clone();
                list[at] ^= 1;
                list
            })
            .collect();
        lists.push([release.list.as_slice(), b"\n"].concat());
        lists.push(release.list[..last].to_vec());
        lists.push(Vec::new());
        lists.push(ReleaseList::of("1.0.0-beta.2", Platform::WindowsX64).list);
        for list in lists {
            assert_eq!(
                refusal(verify(&list, &release.version, &signature)),
                NOT_VERIFIED,
                "{:?}",
                String::from_utf8_lossy(&list)
            );
        }

        // The trusted comment changed to name another release, or dropping its version: the
        // global signature covers it, so it is refused before what it says is read.
        let newer = version("1.0.0-beta.2");
        for (from, to, claimed) in [
            ("version:1.0.0-beta.1", "version:1.0.0-beta.2", &newer),
            ("\tversion:1.0.0-beta.1", "", &release.version),
            (
                "timestamp:1764003600",
                "timestamp:1764003601",
                &release.version,
            ),
        ] {
            let edited = signature.replace(from, to);
            assert_ne!(edited, signature, "{from}");
            assert_eq!(
                refusal(verify(&release.list, claimed, &edited)),
                NOT_VERIFIED,
                "{from}"
            );
        }

        // The signature of the list, or the global signature, changed.
        for (line, at) in [(1, 20), (3, 0)] {
            let edited = with_letter_changed(&signature, line, at);
            assert_ne!(edited, signature);
            assert_eq!(
                refusal(verify(&release.list, &release.version, &edited)),
                NOT_VERIFIED,
                "line {line}"
            );
        }
    }

    #[test]
    fn the_trusted_comment_must_name_the_list_and_the_version() {
        let test_key = keyring(&[TEST_PUBLIC_KEY]);
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let name = &release.name;
        let linux = checksums_asset(&release.version, Platform::LinuxX64);
        let older = checksums_asset(&version("1.0.0-beta"), Platform::WindowsX64);
        let shouted = name.to_uppercase();
        let file = format!("file:{name}");
        let version = format!("version:{RELEASE}");
        let given_file =
            |given: &str| format!("its trusted comment gives the file {given:?}, not {name}");
        let given_version =
            |given: &str| format!("its trusted comment gives the version {given:?}, not {RELEASE}");
        let no_field =
            |field: &str| format!("its trusted comment holds {field:?}, which is no key and value");
        let bad_key = |key: &str| {
            format!("its trusted comment holds the key {key:?}, which is not lower-case letters")
        };
        let cases = [
            // Another file: the other platform's list, an older release's, or the name in
            // capitals.
            (
                format!("{TIMESTAMP}\tfile:{linux}\t{version}"),
                given_file(&linux),
            ),
            (
                format!("{TIMESTAMP}\tfile:{older}\t{version}"),
                given_file(&older),
            ),
            (
                format!("{TIMESTAMP}\tfile:{shouted}\t{version}"),
                given_file(&shouted),
            ),
            // Another version, or this one's written otherwise: build metadata, a tag's `v`, a
            // space after it.
            (
                format!("{TIMESTAMP}\t{file}\tversion:1.0.0-beta.2"),
                given_version("1.0.0-beta.2"),
            ),
            (
                format!("{TIMESTAMP}\t{file}\tversion:1.0.0-beta"),
                given_version("1.0.0-beta"),
            ),
            (
                format!("{TIMESTAMP}\t{file}\tversion:1.0.0-beta.1+build.7"),
                given_version("1.0.0-beta.1+build.7"),
            ),
            (
                format!("{TIMESTAMP}\t{file}\tversion:v1.0.0-beta.1"),
                given_version("v1.0.0-beta.1"),
            ),
            (
                format!("{TIMESTAMP}\t{file}\tversion:1.0.0-beta.1 "),
                given_version("1.0.0-beta.1 "),
            ),
            // A field missing.
            (
                format!("{TIMESTAMP}\t{version}"),
                "its trusted comment gives no file".to_owned(),
            ),
            (
                format!("{TIMESTAMP}\t{file}"),
                "its trusted comment gives no version".to_owned(),
            ),
            (
                TIMESTAMP.to_owned(),
                "its trusted comment gives no file".to_owned(),
            ),
            // A field twice, with the same value or another.
            (
                format!("{TIMESTAMP}\t{file}\t{version}\t{version}"),
                "its trusted comment gives version twice".to_owned(),
            ),
            (
                format!("{TIMESTAMP}\t{file}\tfile:{linux}\t{version}"),
                "its trusted comment gives file twice".to_owned(),
            ),
            (
                format!("{TIMESTAMP}\t{file}\t{version}\t{TIMESTAMP}"),
                "its trusted comment gives timestamp twice".to_owned(),
            ),
            // A field that is no key and value: minisign's own flag after its default comment,
            // a tab left at the end, or no comment at all.
            (
                format!("{TIMESTAMP}\t{file}\t{version}\thashed"),
                no_field("hashed"),
            ),
            (format!("{TIMESTAMP}\t{file}\t{version}\t"), no_field("")),
            (String::new(), no_field("")),
            // A key that is not lower-case letters.
            (
                format!("{TIMESTAMP}\t{file}\tVersion:{RELEASE}"),
                bad_key("Version"),
            ),
            (format!("{TIMESTAMP}\t {file}\t{version}"), bad_key(" file")),
            (
                format!("{TIMESTAMP}\t{file}\t{version}\tsigned-by:release"),
                bad_key("signed-by"),
            ),
            (format!("{TIMESTAMP}\t{file}\t{version}\t:x"), bad_key("")),
        ];
        for (comment, expected) in cases {
            assert_eq!(
                refusal(release.verify(&test_key, &release.signed(&comment))),
                expected,
                "{comment:?}"
            );
        }
    }

    #[test]
    fn a_legacy_signature_is_refused() {
        let release_version = version(RELEASE);
        let published = keyring(&[PUBLISHED_KEY]);
        assert_eq!(
            refusal(published.verify_list(
                b"test",
                "test",
                &release_version,
                PUBLISHED_LEGACY_SIGNATURE.as_bytes()
            )),
            LEGACY
        );

        // The test key's own signature marked as legacy is refused the same way, by the key it
        // names.
        let test_key = keyring(&[TEST_PUBLIC_KEY]);
        let list = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let signature = list.signature();
        // "ED" (prehashed) and the id's first letter, then "Ed" (legacy) and the same.
        let marked = signature.replacen("\nRURH", "\nRWRH", 1);
        assert_ne!(marked, signature);
        assert_eq!(refusal(list.verify(&test_key, &marked)), LEGACY);
    }

    #[test]
    fn a_key_the_keyring_lacks_is_refused() {
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let signature = release.signature();
        let test_key = keyring(&[TEST_PUBLIC_KEY]);
        let published = keyring(&[PUBLISHED_KEY]);

        // minisign's published signature is refused by the test key, and the test key's by the
        // published key.
        assert_eq!(
            refusal(test_key.verify_list(
                b"test",
                "test",
                &release.version,
                PUBLISHED_PREHASHED_SIGNATURE.as_bytes()
            )),
            NO_TRUSTED_KEY
        );
        assert_eq!(
            refusal(release.verify(&published, &signature)),
            NO_TRUSTED_KEY
        );
        // A keyring of no key verifies nothing.
        assert_eq!(
            refusal(release.verify(&Keyring::empty(), &signature)),
            NO_TRUSTED_KEY
        );
        // A signature naming another key's id than the key that made it.
        let other_id = with_letter_changed(&signature, 1, 5);
        assert_ne!(other_id, signature);
        assert_eq!(
            refusal(release.verify(&test_key, &other_id)),
            NO_TRUSTED_KEY
        );
    }

    #[test]
    fn either_of_two_keys_verifies() {
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let published = PublicKey::from_base64(PUBLISHED_KEY).expect("the published key");
        let signature =
            decoded(PUBLISHED_PREHASHED_SIGNATURE.as_bytes()).expect("the published signature");
        for keys in [
            [PUBLISHED_KEY, TEST_PUBLIC_KEY],
            [TEST_PUBLIC_KEY, PUBLISHED_KEY],
        ] {
            let both = keyring(&keys);
            release
                .verify(&both, &release.signature())
                .unwrap_or_else(|error| panic!("{keys:?}: {error}"));
            // The published signature is the published key's, whichever place it holds: it
            // verifies, and only its trusted comment, which names no version, is refused.
            let signer = both
                .signer(b"test", &signature)
                .unwrap_or_else(|error| panic!("{keys:?}: {error}"));
            assert_eq!(both.keys[signer], published, "{keys:?}");
            assert_eq!(
                refusal(both.verify_list(
                    b"test",
                    "test",
                    &release.version,
                    PUBLISHED_PREHASHED_SIGNATURE.as_bytes()
                )),
                "its trusted comment gives no version",
                "{keys:?}"
            );
        }
    }

    #[test]
    fn an_empty_keyring_holds_no_key() {
        assert!(Keyring::empty().is_empty());
        assert_eq!(keyring(&[]), Keyring::empty());
        assert!(!keyring(&[TEST_PUBLIC_KEY]).is_empty());
        assert!(!keyring(&[PUBLISHED_KEY, TEST_PUBLIC_KEY]).is_empty());
    }

    #[test]
    fn a_line_that_is_no_public_key_is_refused() {
        let lines = [
            String::new(),
            "not a key".to_owned(),
            TEST_PUBLIC_KEY[1..].to_owned(),
            format!("{TEST_PUBLIC_KEY}="),
            format!(" {TEST_PUBLIC_KEY}"),
            format!("{TEST_PUBLIC_KEY}\n"),
            // A public key file's two lines, rather than its key's line alone.
            format!("untrusted comment: minisign public key\n{TEST_PUBLIC_KEY}\n"),
            // Another algorithm than Ed25519.
            format!("A{}", &TEST_PUBLIC_KEY[1..]),
            // A signature's line.
            sign_with_test_key(b"list\n", "file:list")
                .lines()
                .nth(1)
                .expect("a signature line")
                .to_owned(),
        ];
        for line in lines {
            assert_eq!(
                refusal(Keyring::of(&[line.as_str()]).map(drop)),
                "key 1 of the keyring is no minisign public key",
                "{line:?}"
            );
            assert_eq!(
                refusal(Keyring::of(&[TEST_PUBLIC_KEY, line.as_str()]).map(drop)),
                "key 2 of the keyring is no minisign public key",
                "{line:?}"
            );
        }
    }

    #[test]
    fn a_signature_that_is_no_minisign_signature_is_refused() {
        let test_key = keyring(&[TEST_PUBLIC_KEY]);
        let release = ReleaseList::of(RELEASE, Platform::WindowsX64);
        let signature = release.signature();
        let lines: Vec<&str> = signature.lines().collect();
        let joined =
            |lines: &[&str]| -> String { lines.iter().map(|line| format!("{line}\n")).collect() };
        // A base64 line without its last group of four letters, which spells its last bytes.
        let short = |line: &str| line[..line.len() - 4].to_owned();
        let verify = |bytes: &[u8]| {
            test_key.verify_list(&release.list, &release.name, &release.version, bytes)
        };

        // A signature of the most bytes one may hold, its trusted comment padded by a field of
        // its own, and one of a byte more.
        let base = comment(&release.name, RELEASE);
        let unpadded = release.signed(&format!("{base}\tpad:")).len();
        let bound = usize::try_from(MAX_LIST_LEN).expect("the bound fits in memory");
        let padded = |more: usize| {
            release.signed(&format!(
                "{base}\tpad:{}",
                "x".repeat(bound - unpadded + more)
            ))
        };
        let largest = padded(0);
        assert_eq!(largest.len(), bound);
        verify(largest.as_bytes()).unwrap_or_else(|error| panic!("{error}"));

        let cases: [(&str, Vec<u8>, String); 13] = [
            (
                "a byte more than 64 KiB",
                padded(1).into_bytes(),
                format!("it holds more than {MAX_LIST_LEN} bytes"),
            ),
            (
                "a byte that is no UTF-8",
                [
                    UNTRUSTED_COMMENT_PREFIX.as_bytes(),
                    b"\xff",
                    &signature.as_bytes()[UNTRUSTED_COMMENT_PREFIX.len()..],
                ]
                .concat(),
                "it is not UTF-8 text".to_owned(),
            ),
            ("nothing", Vec::new(), NO_UNTRUSTED_COMMENT.to_owned()),
            (
                "no untrusted comment",
                joined(&lines[1..]).into_bytes(),
                NO_UNTRUSTED_COMMENT.to_owned(),
            ),
            (
                "an untrusted comment in capitals",
                signature.replacen("untrusted", "Untrusted", 1).into_bytes(),
                NO_UNTRUSTED_COMMENT.to_owned(),
            ),
            (
                "a blank line first",
                format!("\n{signature}").into_bytes(),
                NO_UNTRUSTED_COMMENT.to_owned(),
            ),
            (
                "three lines",
                joined(&lines[..3]).into_bytes(),
                MALFORMED.to_owned(),
            ),
            (
                "an unmarked trusted comment",
                joined(&[lines[0], lines[1], &lines[2]["trusted ".len()..], lines[3]]).into_bytes(),
                MALFORMED.to_owned(),
            ),
            (
                "a signature line that is not base64",
                joined(&[
                    lines[0],
                    &lines[1].replacen('R', "!", 1),
                    lines[2],
                    lines[3],
                ])
                .into_bytes(),
                MALFORMED.to_owned(),
            ),
            (
                "a signature line a group of letters short",
                joined(&[lines[0], &short(lines[1]), lines[2], lines[3]]).into_bytes(),
                MALFORMED.to_owned(),
            ),
            (
                "a global signature a group of letters short",
                joined(&[lines[0], lines[1], lines[2], &short(lines[3])]).into_bytes(),
                MALFORMED.to_owned(),
            ),
            (
                "the lines out of order",
                joined(&[lines[0], lines[2], lines[1], lines[3]]).into_bytes(),
                MALFORMED.to_owned(),
            ),
            (
                "another algorithm than Ed25519",
                signature.replacen("\nRURH", "\nAURH", 1).into_bytes(),
                "it names a signature algorithm other than minisign's Ed25519".to_owned(),
            ),
        ];
        for (case, bytes, expected) in cases {
            assert_eq!(refusal(verify(&bytes)), expected, "{case}");
        }
    }
}
