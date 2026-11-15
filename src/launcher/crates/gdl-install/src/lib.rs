//! The install folder: its receipt, payload policy, embedded sections, lock and probes; installing
//! the game, updating it and the launcher, recovering interrupted work, the expansions, the disc
//! icon and launching. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]

mod payload;

pub use payload::{
    BUILD_INFO, DATA_SUFFIX, LIBRARY_SUFFIX, MAX_BUILD_INFO_LEN, MAX_PAYLOAD_BYTES,
    MAX_PAYLOAD_ENTRIES, PORTABLE_FLAG, Payload, PayloadEntry, PayloadError, ROOT_FILES,
    ROOT_FOLDERS, VERSION_FILE,
};
