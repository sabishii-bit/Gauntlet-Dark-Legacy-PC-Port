//! The install folder: its receipt, payload policy, embedded sections, lock and probes; installing
//! the game, updating it and the launcher, the hand-over to the launcher copy an update put in
//! place, recovering interrupted work, the expansions, the disc icon and launching; and the proxy
//! the system's settings name. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]

mod awake;
mod compat;
#[cfg(all(test, windows))]
mod default_error_mode;
mod destination;
mod faults;
mod folder;
#[cfg(test)]
mod full_disk;
mod handover;
mod hold;
mod host;
mod icon;
mod inspect;
mod install;
mod payload;
#[allow(unsafe_code)]
mod platform;
mod probe;
mod receipt;
mod recover;
mod sections;
mod system_proxy;
#[cfg(test)]
mod test_platform;
mod update;

pub use awake::{KeepAwake, keep_awake};
pub use compat::{
    Access, JOURNAL_NAMES, JOURNAL_SCHEMA, MIN_LAUNCHER, Stamp, StampError, StateName, access,
    launcher_state_name, stamp_of,
};
pub use destination::{
    Blocker, ComponentAttributes, DestinationError, DestinationReport, KnownFolder, Warning,
    judge_destination,
};
#[cfg(feature = "test-hooks")]
pub use faults::{FAULT_VARIABLE, check_fault_variable};
pub use folder::{
    FolderError, InstallFolder, InstallLock, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE,
    STATE_DIR, alpha_markers, check_links,
};
pub use handover::{
    AFTER_UPDATE_OPTION, ENDED_WAIT_LIMIT, Linger, PREVIOUS_WAIT_LIMIT, Previous,
    RETIRED_DELETE_INTERVAL, RETIRED_DELETE_WINDOW, RetiredSweep, hand_over, hand_over_needed,
    is_installed_copy, sweep_retired, wait_for_previous,
};
pub use hold::HOLD_POLL;
pub use host::{Unsupported, host_supported};
pub use icon::{
    DISC_ICON, ICON_SIZES, IconError, IconResources, icon_resources, refresh_shell_icon,
};
#[cfg(windows)]
pub use icon::{Personalised, personalise};
pub use inspect::{FolderState, INSPECTION_SCHEMA, Inspection, InstallationSummary, Relation};
pub use install::{
    CHECK_DISC, CHECK_GAME, CONFIG_FOLDER, COPY_DISC_FILES, COPY_LAUNCHER, Changes, CopiedFile,
    DISC_FILES, DISC_FILES_SCHEMA, DiscFiles, DiscSummary, FolderRecord, GET_RUNTIME,
    INSTALL_JOURNAL, INSTALL_STAGING_PREFIX, InstallError, InstallFailure, InstallNotice,
    InstallPhase, InstallPlan, Installed, PERSONALISE, PREPARE, PUBLISH, RECOVER_INSTALL,
    RecoveryError, RecoveryOutcome, RuntimeSummary, SAVES_FOLDER, SPACE_MARGIN, Unrecorded,
    install_game, install_names, plan_install, recover_install,
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
pub use recover::{RecoverError, Recovered, UpdateRecovery, recover};
pub use sections::{
    MAX_SECTION_LEN, MAX_SECTION_NAME, MAX_TABLE_LEN, RUNTIME_SECTION, Section, SectionView,
    Sections, SectionsError, WINDOW_SECTION,
};
pub use system_proxy::internet_settings;
pub use update::{
    CHECK_INSTALLED, CLEANUP, DOWNLOAD, DOWNLOAD_PREFIX, FINISH_CLEANUP, FileChanges, PlanSource,
    Probed, RECOVER_UPDATE, RETIRED_LAUNCHER_PREFIX, RetiredCopy, STAGE, SWAP, SourceSummary,
    UPDATE_BACKUP, UPDATE_JOURNAL, UPDATE_STAGING_PREFIX, UpdateError, UpdateFailure, UpdateNotice,
    UpdateOutcome, UpdatePhase, UpdatePlan, UpdateRequest, UpdateSource, Updated, VERIFY_DOWNLOAD,
    installed_release, plan_update, update,
};

/// Held while children of this test program run that must not run beside one another: a job a kill
/// may end, an install's or an update's, and a hand-over test's children.
#[cfg(test)]
static ONE_TEST_CHILD: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Waits until no other test runs such children, whatever the last that ran them did, and holds the
/// others off until what it gives is dropped.
#[cfg(test)]
fn one_test_child() -> std::sync::MutexGuard<'static, ()> {
    ONE_TEST_CHILD
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}
