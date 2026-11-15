//! The install folder: its receipt, payload policy, embedded sections, lock and probes; installing
//! the game, updating it and the launcher, recovering interrupted work, the expansions, the disc
//! icon and launching. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]

mod compat;
mod folder;
mod payload;
mod sections;

pub use compat::{
    Access, JOURNAL_NAMES, JOURNAL_SCHEMA, MIN_LAUNCHER, Stamp, StampError, StateName, access,
    launcher_state_name, stamp_of,
};
pub use folder::{
    FolderError, InstallFolder, InstallLock, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE,
    STATE_DIR, alpha_markers, check_links,
};
pub use payload::{
    BUILD_INFO, BuildInfo, DATA_SUFFIX, DEFAULT_CONFIG, LIBRARY_SUFFIX, MAX_BUILD_INFO_LEN,
    MAX_PAYLOAD_BYTES, MAX_PAYLOAD_ENTRIES, MAX_VERSION_FILE_LEN, PORTABLE_FLAG, Payload,
    PayloadEntry, PayloadError, ROOT_FILES, ROOT_FOLDERS, VERSION_FILE,
};
pub use sections::{
    MAX_SECTION_LEN, MAX_SECTION_NAME, MAX_TABLE_LEN, RUNTIME_SECTION, Section, SectionView,
    Sections, SectionsError, WINDOW_SECTION,
};
