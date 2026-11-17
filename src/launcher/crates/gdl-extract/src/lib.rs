//! The Legends expansions' trees: extraction from the N64 and Dreamcast dumps, byte for byte as
//! the Python extractors write them, the fingerprints that identify a dump's files, and the check
//! of an extracted tree against its manifest.
#![forbid(unsafe_code)]

mod dreamcast;
mod error;
mod n64;
mod pyjson;
mod tree;
#[cfg(test)]
mod vectors;

use gdl_files::{BLOCK_LEN, PhaseId};
use gdl_media::{OverlayTable, ROMDISK_LIMITS, RomdiskLimits, US_OVERLAYS};

pub use dreamcast::{
    CheckedDisc, DISC_FOLDER, DISC_FORMAT, DISC_MANIFEST, DISC_SCOPE, DISK_ROM, DiscFileRow,
    DiscManifest, DreamcastExtraction, ExternalRow, IP_BIN, NATIVE_BLOCK_LEN, NATIVE_FOLDER,
    NATIVE_FORMAT, NATIVE_MANIFEST, NATIVE_SCOPE, NativeFileRow, NativeManifest, SIZES_ROM,
    check_dreamcast, extract_dreamcast,
};
pub use error::{ExtractError, NativeRule, TreeRule};
pub use n64::{
    CheckedRom, MAP_OBJECTS, MAP_PREFIX, MAP_WORLD_SUFFIX, MapRow, N64_FORMAT, N64_MANIFEST,
    N64_VERSION, N64Extraction, N64FileRow, N64Manifest, N64Source, ProgramRow, WAD_SUFFIX,
    check_n64, extract_n64,
};
pub use pyjson::{INDENT, JsonValue, render};

/// The phase of an N64 extraction that reads the ROM's files and decides on them, before anything
/// is written.
pub const READ_CARTRIDGE: PhaseId = PhaseId("readCartridge");

/// The phase of a Dreamcast extraction that reads the disc and decides on it, before anything is
/// written.
pub const READ_DISC: PhaseId = PhaseId("readDisc");

/// The phase of an extraction that writes the tree's files into the staging.
pub const UNPACK: PhaseId = PhaseId("unpack");

/// The last phase of an extraction, which writes the manifest the tree is checked against once
/// every file it records is in the staging.
pub const CHECK: PhaseId = PhaseId("check");

/// What an extraction is held to and how it writes: the bounds of the ROMDISK, the boot table the
/// overlays are read through, and the most bytes written between two looks at the job's cancel.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ExtractSettings {
    /// The most files a ROMDISK may list, and the most they may expand to.
    pub(crate) romdisk_limits: RomdiskLimits,
    /// Where the ROM keeps its table of executable overlays, for the ROM it is known for.
    pub(crate) overlays: OverlayTable,
    /// The most bytes a file is written in between two looks at the job's cancel.
    pub(crate) block_len: usize,
}

impl ExtractSettings {
    /// What a player's extraction is held to: the N64 extractor's limits, the US ROM's boot table,
    /// and the blocks every long file operation works in.
    pub(crate) const SHIPPED: ExtractSettings = ExtractSettings {
        romdisk_limits: ROMDISK_LIMITS,
        overlays: US_OVERLAYS,
        block_len: BLOCK_LEN,
    };
}

#[cfg(test)]
mod tests {
    use gdl_files::BLOCK_LEN;
    use gdl_media::{ROMDISK_LIMITS, US_OVERLAYS};

    use super::{CHECK, ExtractSettings, READ_CARTRIDGE, READ_DISC, UNPACK};

    #[test]
    fn a_players_extraction_is_held_to_the_extractors_bounds() {
        let shipped = ExtractSettings::SHIPPED;
        assert_eq!(shipped.romdisk_limits, ROMDISK_LIMITS);
        assert_eq!(shipped.overlays, US_OVERLAYS);
        assert_eq!(shipped.block_len, BLOCK_LEN);
        // The phases' names, as a job's progress shows them.
        let phases = [READ_CARTRIDGE, READ_DISC, UNPACK, CHECK].map(|phase| phase.0);
        assert_eq!(phases, ["readCartridge", "readDisc", "unpack", "check"]);
    }
}
