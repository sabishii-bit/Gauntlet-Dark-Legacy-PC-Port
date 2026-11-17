//! The shared vectors as the readers' tests read them: the N64 and Dreamcast cases that the
//! extractors' tests and the launcher's read alike, the manifests the extractors wrote for their
//! dumps, the names the N64 extractor was given those dumps under, the tracks a sheet lists as the
//! cases write them, with the LBA a GDI gives each, and a disc's data tracks as its manifest
//! records them.

use gdl_fixtures::unhex;
use serde_json::{Value, json};

use crate::{DataTrack, SheetTrack, SourceName, TrackMode};

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

/// The disc manifests the Dreamcast extractor wrote for the vectors' sets, by the names the
/// vectors give them.
const DREAMCAST_GOLDENS: [(&str, &str); 2] = [
    (
        "dreamcast-small-disc-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-small-disc-manifest.json"
        )),
    ),
    (
        "dreamcast-full-disc-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-full-disc-manifest.json"
        )),
    ),
];

/// The N64 cases, read.
pub(crate) fn n64_vectors() -> Value {
    serde_json::from_str(N64_VECTORS).expect("the N64 vectors are JSON")
}

/// The golden manifest the N64 vectors name `name`.
pub(crate) fn n64_golden(name: &Value) -> Value {
    golden(&N64_GOLDENS, name)
}

/// The golden manifest the Dreamcast vectors name `name`.
pub(crate) fn dreamcast_golden(name: &Value) -> Value {
    golden(&DREAMCAST_GOLDENS, name)
}

/// The manifest among `goldens` that the vectors name `name`, read.
fn golden(goldens: &[(&str, &str)], name: &Value) -> Value {
    let name = text(name);
    let (_, golden) = goldens
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
    let rows = tracks
        .iter()
        .map(|track| json!([track.number, track.file, spelled(track.mode), track.index]));
    Value::Array(rows.collect())
}

/// A sheet's tracks as [`listed`] gives them, each followed by the LBA the sheet gives its first
/// sector, or null where it gives none.
pub(crate) fn placed(tracks: &[SheetTrack]) -> Value {
    let rows = tracks.iter().map(|track| {
        let mode = spelled(track.mode);
        json!([track.number, track.file, mode, track.index, track.lba])
    });
    Value::Array(rows.collect())
}

/// A disc's data tracks as the Dreamcast extractor's disc manifest records them: each one's file,
/// index, first LBA and sectors, by those names.
pub(crate) fn recorded(tracks: &[DataTrack]) -> Value {
    let rows = tracks.iter().map(|track| {
        json!({
            "file": track.file,
            "index": track.index,
            "first_lba": track.first_lba,
            "sectors": track.sectors,
        })
    });
    Value::Array(rows.collect())
}

/// A track's mode as a cue sheet spells it.
fn spelled(mode: TrackMode) -> &'static str {
    match mode {
        TrackMode::Audio => "AUDIO",
        TrackMode::Mode1Raw => "MODE1/2352",
    }
}
