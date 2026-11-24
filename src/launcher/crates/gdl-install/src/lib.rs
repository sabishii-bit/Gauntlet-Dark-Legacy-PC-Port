//! The install folder: its receipt, payload policy, embedded sections, lock and probes; installing
//! the game, updating it and the launcher, recovering interrupted work, the expansions, the disc
//! icon and launching. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]

mod awake;
mod compat;
mod destination;
mod folder;
#[cfg(test)]
mod full_disk;
mod host;
mod icon;
mod inspect;
mod install;
mod payload;
#[allow(unsafe_code)]
mod platform;
mod probe;
mod receipt;
mod sections;

pub use awake::{KeepAwake, keep_awake};
pub use compat::{
    Access, JOURNAL_NAMES, JOURNAL_SCHEMA, MIN_LAUNCHER, Stamp, StampError, StateName, access,
    launcher_state_name, stamp_of,
};
pub use destination::{
    Blocker, ComponentAttributes, DestinationError, DestinationReport, KnownFolder, Warning,
    judge_destination,
};
pub use folder::{
    FolderError, InstallFolder, InstallLock, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE,
    STATE_DIR, alpha_markers, check_links,
};
pub use host::{Unsupported, host_supported};
pub use icon::{
    DISC_ICON, ICON_SIZES, IconError, IconResources, icon_resources, refresh_shell_icon,
};
#[cfg(windows)]
pub use icon::{Personalised, personalise};
pub use inspect::{FolderState, INSPECTION_SCHEMA, Inspection, InstallationSummary, Relation};
pub use install::{
    CONFIG_FOLDER, DiscSummary, InstallError, InstallPlan, RuntimeSummary, SAVES_FOLDER,
    SPACE_MARGIN, install_names, plan_install,
};
pub use payload::{
    BUILD_INFO, BuildInfo, DATA_SUFFIX, DEFAULT_CONFIG, LIBRARY_SUFFIX, MAX_BUILD_INFO_LEN,
    MAX_PAYLOAD_BYTES, MAX_PAYLOAD_ENTRIES, MAX_VERSION_FILE_LEN, PORTABLE_FLAG, Payload,
    PayloadEntry, PayloadError, ROOT_FILES, ROOT_FOLDERS, VERSION_FILE,
};
pub use probe::{
    GAME_PROBE_LIMIT, GameStartError, GameUseError, MAX_PROBE_OUTPUT, VersionProbe,
    probe_game_version, probe_game_version_with, require_game_closed,
};
pub use receipt::{
    DiscIcon, GUNE5D, InstalledGame, InstalledLauncher, MAX_RECEIPT_LEN, RECEIPT_SCHEMA, Receipt,
    ReceiptError, installed_launcher_name,
};
pub use sections::{
    MAX_SECTION_LEN, MAX_SECTION_NAME, MAX_TABLE_LEN, RUNTIME_SECTION, Section, SectionView,
    Sections, SectionsError, WINDOW_SECTION,
};
