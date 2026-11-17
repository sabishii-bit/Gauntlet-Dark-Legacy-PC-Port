//! The shared vectors as the readers' tests read them: the N64 and Dreamcast cases that the
//! extractors' tests and the launcher's read alike, the manifests the extractors wrote for their
//! dumps, the names the N64 extractor was given those dumps under, the tracks a sheet lists as the
//! cases write them, with the LBA a GDI gives each, a disc's data tracks and files as its manifest
//! records them, and the files a packed file table lists as the cases and the native manifests
//! write them. Beside them, where the trusted machine keeps the player's own dumps.

use std::env;
use std::path::{Path, PathBuf};

use gdl_fixtures::unhex;
use serde_json::{Value, json};

use crate::{DataTrack, DiscFile, PackedFile, PackedPlace, SheetTrack, SourceName, TrackMode};

/// The repository's root, from this crate's folder.
const REPOSITORY_FROM_CRATE: &str = "../../../..";

/// The variable that names the player's own N64 ROM for the tests that need it.
const ROM_VARIABLE: &str = "GDL_LEGENDS_N64_ROM";

/// Where the player's N64 ROM lies in the repository's assets, when the variable names no other
/// place.
const ROM_IN_ASSETS: &str = "assets/n64/Gauntlet Legends (USA).z64";

/// The variable that names the player's own Dreamcast disc for the tests that need it: the cue
/// sheet of its dump.
const DISC_VARIABLE: &str = "GDL_LEGENDS_DREAMCAST_DISC";

/// Where the player's disc lies in the repository's assets, when the variable names no other
/// place.
const DISC_IN_ASSETS: &str = "assets/dreamcast/Gauntlet Legends (USA)/Gauntlet Legends (USA).cue";

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

/// The manifests the Dreamcast extractor wrote for the vectors' sets and for the native files of
/// the table its unpack test reads, by the names the vectors give them.
const DREAMCAST_GOLDENS: [(&str, &str); 4] = [
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
    (
        "dreamcast-full-native-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-full-native-manifest.json"
        )),
    ),
    (
        "dreamcast-unpack-native-manifest.json",
        include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../../tests/scripts/data/legends/dreamcast-unpack-native-manifest.json"
        )),
    ),
];

/// The offset the Dreamcast vectors list a file at that SIZES.ROM keeps outside DISK.ROM, as the
/// table gives it.
const EXTERNAL_OFFSET: i64 = -1;

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

/// A disc's files as the Dreamcast extractor's disc manifest records them, but for their
/// digests: each one's path, LBA and size, by those names.
pub(crate) fn located(files: &[DiscFile]) -> Value {
    let rows = files.iter().map(|file| {
        json!({
            "path": file.path.as_str(),
            "lba": file.lba,
            "size": file.size,
        })
    });
    Value::Array(rows.collect())
}

/// The files a disc manifest records, as [`located`] gives a disc's: without their digests.
pub(crate) fn manifest_files(manifest: &Value) -> Value {
    let rows = manifest["files"]
        .as_array()
        .expect("a manifest lists files");
    let rows = rows.iter().map(|row| {
        json!({
            "path": row["path"],
            "lba": row["lba"],
            "size": row["size"],
        })
    });
    Value::Array(rows.collect())
}

/// The files SIZES.ROM lists as the Dreamcast vectors' table cases list them: each one's path, its
/// offset in DISK.ROM, or -1 for one the disc keeps outside it, and its size.
pub(crate) fn packed_rows(files: &[PackedFile]) -> Value {
    let rows = files.iter().map(|file| {
        let offset = match file.place {
            PackedPlace::Disk { offset } => json!(offset),
            PackedPlace::External => json!(EXTERNAL_OFFSET),
        };
        json!([file.path.as_str(), offset, file.size])
    });
    Value::Array(rows.collect())
}

/// The files SIZES.ROM lists as the Dreamcast extractor's native manifest records them, but for
/// their digests: those DISK.ROM holds, each one's path, offset and size, and those the disc keeps
/// outside it, each one's path and the size its row declares, by those names.
pub(crate) fn restored(files: &[PackedFile]) -> Value {
    let (mut held, mut external) = (Vec::new(), Vec::new());
    for file in files {
        let path = file.path.as_str();
        match file.place {
            PackedPlace::Disk { offset } => {
                held.push(json!({"path": path, "offset": offset, "size": file.size}));
            }
            PackedPlace::External => {
                external.push(json!({"path": path, "declared_size": file.size}));
            }
        }
    }
    json!({"files": held, "external": external})
}

/// The files a native manifest records, as [`restored`] gives a table's: without their digests.
pub(crate) fn manifest_restored(manifest: &Value) -> Value {
    let rows = manifest["files"]
        .as_array()
        .expect("a native manifest lists files");
    let rows: Vec<Value> = rows
        .iter()
        .map(|row| json!({"path": row["path"], "offset": row["offset"], "size": row["size"]}))
        .collect();
    json!({"files": rows, "external": manifest["external"]})
}

/// The player's own N64 ROM, for the tests that run on the trusted machine, as [`players_dump`]
/// finds it.
pub(crate) fn players_rom() -> PathBuf {
    players_dump(ROM_VARIABLE, ROM_IN_ASSETS, "N64 ROM")
}

/// The cue sheet of the player's own Dreamcast disc, for the tests that run on the trusted
/// machine, as [`players_dump`] finds it.
pub(crate) fn players_disc() -> PathBuf {
    players_dump(DISC_VARIABLE, DISC_IN_ASSETS, "Dreamcast disc")
}

/// The player's own dump, `what` it is: the file the variable `variable` names, else the one at
/// `in_assets` in the repository. Fails, naming the variable to set, when there is none.
fn players_dump(variable: &str, in_assets: &str, what: &str) -> PathBuf {
    let path = env::var_os(variable).map_or_else(
        || {
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join(REPOSITORY_FROM_CRATE)
                .join(in_assets)
        },
        PathBuf::from,
    );
    assert!(
        path.is_file(),
        "no {what} at {}: set {variable} to the player's dump",
        path.display()
    );
    path
}

/// A track's mode as a cue sheet spells it.
fn spelled(mode: TrackMode) -> &'static str {
    match mode {
        TrackMode::Audio => "AUDIO",
        TrackMode::Mode1Raw => "MODE1/2352",
    }
}
