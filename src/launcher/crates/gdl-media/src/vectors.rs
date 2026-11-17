//! The shared vectors as the readers' tests read them: the N64 cases that the extractor's tests and
//! the launcher's read alike, the manifests the extractor wrote for their dumps, and the names it
//! was given those dumps under.

use gdl_fixtures::unhex;
use serde_json::Value;

use crate::SourceName;

/// The N64 cases.
const N64_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../../tests/scripts/data/legends/n64-vectors.json"
));

/// The manifests the N64 extractor wrote for the vectors' dumps, by the names the vectors give
/// them.
const N64_GOLDENS: [(&str, &str); 4] = [
    (
        "n64-default-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-default-manifest.json"
        )),
    ),
    (
        "n64-mixed-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-mixed-manifest.json"
        )),
    ),
    (
        "n64-byte-swapped-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-byte-swapped-manifest.json"
        )),
    ),
    (
        "n64-little-endian-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/n64-little-endian-manifest.json"
        )),
    ),
];

/// The N64 cases, read.
pub(crate) fn n64_vectors() -> Value {
    serde_json::from_str(N64_VECTORS).expect("the N64 vectors are JSON")
}

/// The golden manifest the N64 vectors name `name`.
pub(crate) fn n64_golden(name: &Value) -> Value {
    let name = text(name);
    let (_, golden) = N64_GOLDENS
        .iter()
        .find(|(golden, _)| *golden == name)
        .unwrap_or_else(|| panic!("no golden named {name}"));
    serde_json::from_str(golden).expect("a golden manifest is JSON")
}

/// The image the N64 vectors freeze under `name`.
pub(crate) fn frozen(vectors: &Value, name: &str) -> Vec<u8> {
    unhex(&vectors["images"][name])
}

/// A string of the vectors.
pub(crate) fn text(value: &Value) -> &str {
    value.as_str().expect("a string")
}

/// A number of the vectors.
pub(crate) fn number(value: &Value) -> usize {
    usize::try_from(value.as_u64().expect("a number")).unwrap()
}

/// A number of the vectors that fits a word.
pub(crate) fn word(value: &Value) -> u32 {
    u32::try_from(value.as_u64().expect("a number")).unwrap()
}

/// The name of a dump's file called `text`.
pub(crate) fn named(text: &str) -> SourceName {
    SourceName::from_units(text.encode_utf16().collect())
}
