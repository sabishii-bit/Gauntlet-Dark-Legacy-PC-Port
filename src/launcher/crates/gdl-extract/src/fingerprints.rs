//! The fingerprints an installation's extraction holds a Dreamcast disc's files to, taken from a
//! trusted extraction's tree: for IP.BIN and for every file under `disc`, the SHA-256 digest of its
//! path in the tree beside the SHA-256 digest of its bytes. They hold no name, no size and no byte
//! of the disc, only digests and the identity of the disc they are for, and a file's fingerprint is
//! found by the digest of the path the extraction gives it. They are read as the fingerprint
//! writer, scripts/assets/legends_fingerprints.py, writes them.

use std::collections::BTreeMap;
use std::collections::btree_map::Entry;

use gdl_media::{MAX_ENTRIES, Sha256Digest, US_DISC};
use serde::Deserialize;

use crate::IP_BIN;

/// The format fingerprints are written in.
pub const FINGERPRINTS_FORMAT: &str = "gdl-legends-fingerprints";

/// The version of that format the launcher reads.
pub const FINGERPRINTS_VERSION: u64 = 1;

/// The most fingerprints a set may hold: one for every entry a disc's tree may hold, and one for
/// IP.BIN.
pub const MAX_FINGERPRINTS: usize = MAX_ENTRIES + 1;

/// The expansion fingerprints are taken for, whose disc's files they fingerprint.
const EXPANSION: &str = "dreamcast";

/// The fingerprints of the US disc's files, as the fingerprint writer took them from an extraction
/// of the player's own disc on the trusted machine.
const COMMITTED: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/fingerprints/dreamcast-us.json"
));

/// The digests a trusted extraction's files had, each under the digest of its tree path: the
/// files of the tree that come from the disc, IP.BIN and every file under `disc`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Fingerprints {
    /// Each file's digest, under its path's digest.
    files: BTreeMap<Sha256Digest, Sha256Digest>,
}

/// Fingerprints as the fingerprint writer writes them. Keys it does not know are passed over.
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Written {
    /// The format they are written in.
    format: String,
    /// The version of that format.
    version: u64,
    /// The expansion they are for.
    expansion: String,
    /// The product the disc's boot area names, without its padding.
    product: String,
    /// The product's version the boot area gives.
    product_version: String,
    /// The areas the boot area names, without their padding.
    area: String,
    /// The fingerprints, in the order of their path digests.
    files: Vec<WrittenRow>,
}

/// A fingerprint as the writer writes one: two digests in lowercase hexadecimal.
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct WrittenRow {
    /// The digest of the file's tree path.
    path_sha256: String,
    /// The digest of the file's bytes.
    sha256: String,
}

impl Fingerprints {
    /// The fingerprints `text` holds, read as the fingerprint writer writes them: of
    /// [`FINGERPRINTS_FORMAT`] at [`FINGERPRINTS_VERSION`], for the Dreamcast expansion and for the
    /// US disc's product, version and area, each without the padding its boot area keeps; at most
    /// [`MAX_FINGERPRINTS`] rows, each two digests of 64 lowercase hexadecimal digits; no path
    /// digest twice; and one row for IP.BIN. Refused by the first of these the text breaks, in
    /// that order, once it has been read as JSON of the writer's shape.
    pub fn parse(text: &str) -> Result<Fingerprints, FingerprintsError> {
        let written: Written =
            serde_json::from_str(text).map_err(|error| FingerprintsError::Json {
                detail: error.to_string().into(),
            })?;
        if written.format != FINGERPRINTS_FORMAT {
            return Err(FingerprintsError::Format);
        }
        if written.version != FINGERPRINTS_VERSION {
            return Err(FingerprintsError::Version);
        }
        let identity = [
            (&written.product, &US_DISC.product[..]),
            (&written.product_version, &US_DISC.version[..]),
            (&written.area, &US_DISC.area[..]),
        ];
        let for_the_disc = identity
            .iter()
            .all(|(given, field)| given.as_bytes() == field.trim_ascii());
        if written.expansion != EXPANSION || !for_the_disc {
            return Err(FingerprintsError::Identity);
        }
        if written.files.len() > MAX_FINGERPRINTS {
            return Err(FingerprintsError::TooMany);
        }
        let mut files = BTreeMap::new();
        for (row, fingerprint) in written.files.iter().enumerate() {
            let digests = Sha256Digest::from_hex(&fingerprint.path_sha256)
                .zip(Sha256Digest::from_hex(&fingerprint.sha256));
            let (key, digest) = digests.ok_or(FingerprintsError::Digest { row })?;
            hold(&mut files, key, digest)?;
        }
        if !files.contains_key(&Fingerprints::path_key(IP_BIN)) {
            return Err(FingerprintsError::NoBootArea);
        }
        Ok(Fingerprints { files })
    }

    /// The fingerprints of the US disc's files, which the launcher carries. Panics only when the
    /// file it carries them in is malformed, which a test rules out.
    pub fn committed() -> Fingerprints {
        Fingerprints::parse(COMMITTED).expect("the committed fingerprints are well formed")
    }

    /// The fingerprints of `files`, each a path of a tree and the digest of the file at it, held
    /// to the bounds [`parse`](Self::parse) holds a written set to: at most [`MAX_FINGERPRINTS`] of
    /// them, and no path twice. They need not hold IP.BIN's, which the extraction asks for when it
    /// holds a disc to them.
    pub fn from_files(
        files: impl IntoIterator<Item = (String, Sha256Digest)>,
    ) -> Result<Fingerprints, FingerprintsError> {
        let mut held = BTreeMap::new();
        for (index, (path, digest)) in files.into_iter().enumerate() {
            if index == MAX_FINGERPRINTS {
                return Err(FingerprintsError::TooMany);
            }
            hold(&mut held, Fingerprints::path_key(&path), digest)?;
        }
        Ok(Fingerprints { files: held })
    }

    /// The key a file's fingerprint is found by: the SHA-256 digest of its tree path's UTF-8
    /// bytes, its components joined by `/` in their exact letter case.
    pub fn path_key(path: &str) -> Sha256Digest {
        Sha256Digest::of(path.as_bytes())
    }

    /// The digest the file at the tree path `path` had, when the fingerprints hold one for it.
    pub fn get(&self, path: &str) -> Option<Sha256Digest> {
        self.files.get(&Fingerprints::path_key(path)).copied()
    }

    /// Every path digest the fingerprints are kept under, in the digests' order.
    pub fn keys(&self) -> impl Iterator<Item = &Sha256Digest> {
        self.files.keys()
    }

    /// How many fingerprints there are.
    pub fn len(&self) -> usize {
        self.files.len()
    }

    /// Whether there are none.
    pub fn is_empty(&self) -> bool {
        self.files.is_empty()
    }
}

/// Keeps `digest` under `key` in `files`, refused when a fingerprint is kept there already.
fn hold(
    files: &mut BTreeMap<Sha256Digest, Sha256Digest>,
    key: Sha256Digest,
    digest: Sha256Digest,
) -> Result<(), FingerprintsError> {
    match files.entry(key) {
        Entry::Vacant(slot) => {
            slot.insert(digest);
            Ok(())
        }
        Entry::Occupied(_) => Err(FingerprintsError::Duplicate {
            key: key.hex().into(),
        }),
    }
}

/// Why fingerprints were refused.
#[derive(Debug, thiserror::Error)]
pub enum FingerprintsError {
    /// The text is not JSON, or not of the shape fingerprints are written in: a field is missing
    /// or of another kind.
    #[error("the fingerprints are not written as their format writes them: {detail}")]
    Json {
        /// What the JSON reader said.
        detail: Box<str>,
    },
    /// They are written in another format.
    #[error("the fingerprints are not of the format {format}", format = FINGERPRINTS_FORMAT)]
    Format,
    /// They are written in another version of the format.
    #[error(
        "the fingerprints are not of version {version} of their format",
        version = FINGERPRINTS_VERSION
    )]
    Version,
    /// They are for another expansion, or for another product, version or area of the disc.
    #[error("the fingerprints are for another expansion or another disc")]
    Identity,
    /// A row's path digest or file digest is not 64 lowercase hexadecimal digits.
    #[error("fingerprint {row} does not hold two SHA-256 digests in lowercase hexadecimal")]
    Digest {
        /// Where the row stands among the rows, counted from nought.
        row: usize,
    },
    /// Two fingerprints are kept under one path digest.
    #[error("two fingerprints are kept under one path digest: {key}")]
    Duplicate {
        /// That digest, in lowercase hexadecimal.
        key: Box<str>,
    },
    /// There are more fingerprints than [`MAX_FINGERPRINTS`].
    #[error("there are more than {most} fingerprints", most = MAX_FINGERPRINTS)]
    TooMany,
    /// None of the fingerprints is IP.BIN's.
    #[error("no fingerprint is {name}'s", name = IP_BIN)]
    NoBootArea,
}

#[cfg(test)]
mod tests {
    use gdl_files::Cancel;
    use gdl_fixtures::{DiscSet, Edc};
    use gdl_media::Sha256Digest;
    use serde_json::{Value, json};

    use super::{
        COMMITTED, FINGERPRINTS_FORMAT, FINGERPRINTS_VERSION, Fingerprints, FingerprintsError,
        MAX_FINGERPRINTS,
    };
    use crate::vectors::{
        FULL_FINGERPRINTS, Folders, array, dreamcast_root, fingerprints_of, quiet, stage_dreamcast,
        tree_listing,
    };
    use crate::{DISC_FOLDER, ExtractSettings, IP_BIN, JsonValue, Strictness, render};

    /// The most bytes a refusal may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// How many fingerprints the US disc's tree gives: IP.BIN's and its 156 files'.
    const US_DISC_FINGERPRINTS: usize = 157;

    /// How many the vectors' full set's tree gives: IP.BIN's and its four files'.
    const FULL_SET_FINGERPRINTS: usize = 5;

    /// The digest of the tree path `IP.BIN`, as the fingerprint writer finds it.
    const IP_BIN_KEY: &str = "e60b7f339972edaa3a7a242795ff7dbf098d6c5f565b1e4c4ceed4469aa67814";

    /// The writer's fingerprints of the full set's tree, read as JSON, to be changed.
    fn written() -> Value {
        serde_json::from_str(FULL_FINGERPRINTS).unwrap()
    }

    /// What `document`, written out, is refused by.
    fn refused(document: &Value) -> FingerprintsError {
        Fingerprints::parse(&document.to_string()).unwrap_err()
    }

    #[test]
    fn the_committed_fingerprints_parse() {
        let committed = Fingerprints::committed();
        assert_eq!(committed.len(), US_DISC_FINGERPRINTS);
        assert!(committed.get(IP_BIN).is_some());
        let key = Fingerprints::path_key(IP_BIN);
        assert!(committed.keys().any(|held| *held == key));
        assert_eq!(Fingerprints::parse(COMMITTED).unwrap(), committed);
        // The file holds digests and the disc's identity alone: no path, no name and no size.
        for word in [IP_BIN, "disc/", "DISK.ROM", "\"size\""] {
            assert!(!COMMITTED.contains(word), "the fingerprints hold {word}");
        }
    }

    #[test]
    fn path_keys_are_the_sha256_of_the_tree_path() {
        assert_eq!(Fingerprints::path_key(IP_BIN).hex(), IP_BIN_KEY);
        let path = "disc/AUDIO/X.STR";
        assert_eq!(
            Fingerprints::path_key(path),
            Sha256Digest::of(path.as_bytes())
        );
        // The path is held in its exact letter case.
        assert_ne!(
            Fingerprints::path_key(path),
            Fingerprints::path_key("disc/audio/x.str")
        );
        // A file's fingerprint is found by its path.
        let digest = Sha256Digest::of(b"boot");
        let fingerprints = Fingerprints::from_files([(IP_BIN.to_owned(), digest)]).unwrap();
        assert_eq!(fingerprints.get(IP_BIN), Some(digest));
        assert_eq!(fingerprints.get("ip.bin"), None);
        let keys: Vec<String> = fingerprints.keys().map(Sha256Digest::hex).collect();
        assert_eq!(keys, [IP_BIN_KEY]);
    }

    #[test]
    fn the_python_writer_and_the_reader_agree() {
        // The tree the full set extracts to, and the fingerprints of its files that come from the
        // disc, as the writer takes them: IP.BIN's and those under `disc`, never a native file's
        // or a manifest's.
        let folders = Folders::new();
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let shipped = ExtractSettings::SHIPPED;
        let (staging, _) =
            stage_dreamcast(&sheet, &folders.parent, &Strictness::Script, &shipped, &job).unwrap();
        let listing = tree_listing(&dreamcast_root().path_under(staging.path()));
        drop(staging);
        let extracted = fingerprints_of(&listing);
        assert_eq!(extracted.len(), FULL_SET_FINGERPRINTS);
        let disc_folder = format!("{DISC_FOLDER}/");
        for (path, _, digest) in &listing {
            let fingerprinted = path == IP_BIN || path.starts_with(&disc_folder);
            let found = extracted.get(path).map(|found| found.hex());
            assert_eq!(
                found.as_deref(),
                fingerprinted.then_some(digest.as_str()),
                "{path}"
            );
        }

        // The writer's file of the same tree reads as the same fingerprints.
        let parsed = Fingerprints::parse(FULL_FINGERPRINTS).unwrap();
        assert_eq!(parsed, extracted);

        // And it is, byte for byte, what the writer writes of them: its keys in its order, the
        // disc's identity without padding, and a row for each fingerprint in the order of its
        // path digest.
        let rows = extracted
            .files
            .iter()
            .map(|(key, digest)| {
                JsonValue::Object(vec![
                    ("pathSha256", JsonValue::Str(key.hex())),
                    ("sha256", JsonValue::Str(digest.hex())),
                ])
            })
            .collect();
        let text = |value: &str| JsonValue::Str(value.to_owned());
        let document = JsonValue::Object(vec![
            ("format", text(FINGERPRINTS_FORMAT)),
            ("version", JsonValue::Uint(FINGERPRINTS_VERSION)),
            ("expansion", text("dreamcast")),
            ("product", text("T9710N")),
            ("productVersion", text("V1.003")),
            ("area", text("U")),
            ("files", JsonValue::Array(rows)),
        ]);
        assert!(render(&document) == FULL_FINGERPRINTS.as_bytes());
    }

    #[test]
    fn malformed_fingerprints_are_refused() {
        let golden = written();
        assert_eq!(array(&golden["files"]).len(), FULL_SET_FINGERPRINTS);
        // Keys the reader does not know are passed over, at the top and in a row.
        let mut annotated = golden.clone();
        annotated["comment"] = json!("taken on the trusted machine");
        annotated["files"][0]["comment"] = json!("a row");
        let taken = Fingerprints::parse(&annotated.to_string()).unwrap();
        assert_eq!(taken, Fingerprints::parse(FULL_FINGERPRINTS).unwrap());

        // What is not JSON of the writer's shape.
        for text in ["", "fingerprints", "[]", "{}"] {
            let error = Fingerprints::parse(text).unwrap_err();
            assert!(matches!(error, FingerprintsError::Json { .. }), "{text:?}");
        }
        let mut without_files = golden.clone();
        without_files.as_object_mut().unwrap().remove("files");
        assert!(matches!(
            refused(&without_files),
            FingerprintsError::Json { .. }
        ));
        let mut text_version = golden.clone();
        text_version["version"] = json!("1");
        assert!(matches!(
            refused(&text_version),
            FingerprintsError::Json { .. }
        ));

        // Another format, or another version of it.
        let mut other = golden.clone();
        other["format"] = json!("gdl-legends-hashes");
        assert!(matches!(refused(&other), FingerprintsError::Format));
        let mut other = golden.clone();
        other["version"] = json!(2);
        assert!(matches!(refused(&other), FingerprintsError::Version));

        // Another expansion, product, version or area, or a field given with its padding.
        for (key, value) in [
            ("expansion", "n64"),
            ("product", "T9710P"),
            ("product", "T9710N    "),
            ("productVersion", "V1.002"),
            ("area", "E"),
            ("area", " U      "),
        ] {
            let mut other = golden.clone();
            other[key] = json!(value);
            assert!(
                matches!(refused(&other), FingerprintsError::Identity),
                "{key} {value:?}"
            );
        }

        // A digest that is not 64 lowercase hexadecimal digits, in either place of a row.
        let first = golden["files"][0]["pathSha256"]
            .as_str()
            .unwrap()
            .to_owned();
        let third = golden["files"][2]["sha256"].as_str().unwrap().to_owned();
        for (row, key, value) in [
            (2, "sha256", third.to_uppercase()),
            (0, "pathSha256", first[..63].to_owned()),
            (4, "sha256", String::new()),
        ] {
            let mut other = golden.clone();
            other["files"][row][key] = json!(value);
            match refused(&other) {
                FingerprintsError::Digest { row: refused } => assert_eq!(refused, row, "{key}"),
                error => panic!("{row} {key}: {error:?}"),
            }
        }

        // Two fingerprints under one key.
        let mut twice = golden.clone();
        twice["files"][1]["pathSha256"] = json!(first);
        match refused(&twice) {
            FingerprintsError::Duplicate { key } => assert_eq!(&*key, first),
            error => panic!("{error:?}"),
        }

        // None for IP.BIN.
        let mut without_boot = golden.clone();
        let rows = without_boot["files"].as_array_mut().unwrap();
        rows.retain(|row| row["pathSha256"] != IP_BIN_KEY);
        assert_eq!(rows.len(), FULL_SET_FINGERPRINTS - 1);
        assert!(matches!(
            refused(&without_boot),
            FingerprintsError::NoBootArea
        ));

        // More rows than a disc's tree may give, refused before any row is looked at. The rows
        // are written into the text as text, which keeps the test's memory to the text's.
        let mut crowded = golden;
        crowded["files"] = json!([]);
        let without_rows = crowded.to_string();
        let rows = vec![r#"{"pathSha256":"","sha256":""}"#; MAX_FINGERPRINTS + 1].join(",");
        let text = without_rows.replacen(r#""files":[]"#, &format!(r#""files":[{rows}]"#), 1);
        assert_ne!(text, without_rows);
        let error = Fingerprints::parse(&text).unwrap_err();
        assert!(matches!(error, FingerprintsError::TooMany), "{error:?}");
    }

    #[test]
    fn an_empty_set_is_empty() {
        let empty = Fingerprints::from_files(Vec::new()).unwrap();
        assert!(empty.is_empty());
        assert_eq!(empty.len(), 0);
        assert_eq!(empty.keys().count(), 0);
        assert_eq!(empty.get(IP_BIN), None);
        let one = Fingerprints::from_files([(IP_BIN.to_owned(), Sha256Digest::of(b"boot"))]);
        let one = one.unwrap();
        assert!(!one.is_empty());
        assert_eq!(one.len(), 1);
    }

    #[test]
    fn fingerprints_of_files_are_held_to_the_files_bounds() {
        let digest = Sha256Digest::of(b"file");
        // A path given twice would be two fingerprints under one key.
        let twice = [
            ("disc/A.BIN".to_owned(), digest),
            ("disc/A.BIN".to_owned(), Sha256Digest::of(b"other")),
        ];
        match Fingerprints::from_files(twice).unwrap_err() {
            FingerprintsError::Duplicate { key } => {
                assert_eq!(&*key, Fingerprints::path_key("disc/A.BIN").hex());
            }
            error => panic!("{error:?}"),
        }
        // Paths that differ in letter case are two paths.
        let cases = [
            ("disc/A.BIN".to_owned(), digest),
            ("disc/a.bin".to_owned(), digest),
        ];
        assert_eq!(Fingerprints::from_files(cases).unwrap().len(), 2);
        // As many as a disc's tree may give, and no more.
        let paths = |count: usize| (0..count).map(move |at| (format!("disc/{at}"), digest));
        let most = Fingerprints::from_files(paths(MAX_FINGERPRINTS)).unwrap();
        assert_eq!(most.len(), MAX_FINGERPRINTS);
        let error = Fingerprints::from_files(paths(MAX_FINGERPRINTS + 1)).unwrap_err();
        assert!(matches!(error, FingerprintsError::TooMany), "{error:?}");
    }

    #[test]
    fn refusals_say_what_is_wrong() {
        let refusals = [
            (
                FingerprintsError::Format,
                "the fingerprints are not of the format gdl-legends-fingerprints",
            ),
            (
                FingerprintsError::Version,
                "the fingerprints are not of version 1 of their format",
            ),
            (
                FingerprintsError::Identity,
                "the fingerprints are for another expansion or another disc",
            ),
            (
                FingerprintsError::Digest { row: 3 },
                "fingerprint 3 does not hold two SHA-256 digests in lowercase hexadecimal",
            ),
            (
                FingerprintsError::Duplicate {
                    key: IP_BIN_KEY.into(),
                },
                "two fingerprints are kept under one path digest: \
                 e60b7f339972edaa3a7a242795ff7dbf098d6c5f565b1e4c4ceed4469aa67814",
            ),
            (
                FingerprintsError::TooMany,
                "there are more than 100001 fingerprints",
            ),
            (FingerprintsError::NoBootArea, "no fingerprint is IP.BIN's"),
        ];
        for (refusal, words) in refusals {
            assert_eq!(refusal.to_string(), words);
        }
        let error = Fingerprints::parse("{}").unwrap_err();
        let words = error.to_string();
        assert!(
            words.starts_with("the fingerprints are not written as their format writes them: "),
            "{words}"
        );
        // A refusal is cheap to return, whatever it carries.
        assert!(size_of::<FingerprintsError>() <= MOST_ERROR_BYTES);
    }
}
