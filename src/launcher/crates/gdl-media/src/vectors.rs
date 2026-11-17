//! The shared vectors as the readers' tests read them: the N64 and Dreamcast cases that the
//! extractors' tests and the launcher's read alike, the manifests the N64 extractor wrote for its
//! dumps, the names it was given those dumps under, and the tracks a sheet lists as the cases
//! write them.

use gdl_fixtures::unhex;
use serde_json::{Value, json};

use crate::{SheetTrack, SourceName, TrackMode};

/// The N64 cases.
const N64_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../../tests/scripts/data/legends/n64-vectors.json"
));

/// The Dreamcast cases.
const DREAMCAST_VECTORS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../../tests/scripts/data/legends/dreamcast-vectors.json"
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

/// The Dreamcast cases, read.
pub(crate) fn dreamcast_vectors() -> Value {
    serde_json::from_str(DREAMCAST_VECTORS).expect("the Dreamcast vectors are JSON")
}

/// The payload the Dreamcast vectors' EDC cases name `name`: the bytes a sector's user data
/// begins with, noughts after them.
pub(crate) fn edc_payload(vectors: &Value, name: &str) -> Vec<u8> {
    unhex(&vectors["edc"]["payloads"][name])
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

/// A sheet's tracks as the Dreamcast vectors list them: each one's number, file, mode as a cue
/// sheet spells it, and index.
pub(crate) fn listed(tracks: &[SheetTrack]) -> Value {
    let rows = tracks.iter().map(|track| {
        let mode = match track.mode {
            TrackMode::Audio => "AUDIO",
            TrackMode::Mode1Raw => "MODE1/2352",
        };
        json!([track.number, track.file, mode, track.index])
    });
    Value::Array(rows.collect())
}
