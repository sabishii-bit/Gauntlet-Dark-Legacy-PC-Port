//! A fresh install of the game into a folder of the player's: the `Gauntlet` and `carddemo` trees
//! of the player's GameCube disc, the runtime the launcher file carries, a copy of the launcher
//! file and the two folders the game keeps the player's saves and settings in. An install is
//! planned before anything is written: the plan reads the disc's image, judged whole when it was
//! opened, the runtime's central directory and release record, and the folder, and lists what
//! refuses an install there, so that a disc, a runtime or a folder an install would fail on is
//! refused before a file is made. The install then makes everything in a staging folder of its
//! own inside the launcher's state folder, under a journal that records how far it has come, and
//! puts each finished root in place by a rename; its receipt, written once every root is in place,
//! is the point from which the installation stands.

mod disc_files;
mod journal;
mod settings;
mod stage;

use std::collections::BTreeSet;
use std::io::{self, Read, Seek};
use std::path::{Path, PathBuf};

use gdl_files::{DISC_NAMES, FilesError, Job, NameRules, PAYLOAD_NAMES, PhaseId, RelPath};
#[cfg(windows)]
use gdl_media::DiscEntry;
use gdl_media::{Container, DiscEntryKind, GameCubeImage, MediaError};
#[cfg(windows)]
use gdl_media::{MAX_TPL_LEN, first_frame};
use gdl_release::Platform;

pub use disc_files::{CopiedFile, DISC_FILES, DISC_FILES_SCHEMA, DiscFiles};
pub use journal::{INSTALL_JOURNAL, InstallPhase};

use self::settings::InstallSettings;
use self::stage::Install;
use crate::awake::keep_awake;
use crate::destination::{
    Blocker, DestinationError, DestinationReport, FolderChecks, Warning, judge_with,
};
use crate::folder::{InstallFolder, LockError, STATE_DIR};
use crate::host::{Unsupported, host_supported};
use crate::icon::IconError;
#[cfg(windows)]
use crate::icon::{DISC_ICON, icon_resources};
use crate::payload::{LIB_FOLDER, PayloadError, ROOT_FILES, ROOT_FOLDERS};
use crate::receipt::{Receipt, installed_launcher_name};
use crate::sections::{Sections, SectionsError};

/// The phase in which an install judges its folder again before it makes anything.
pub const CHECK_DISC: PhaseId = PhaseId("checkDisc");

/// The phase in which an install makes its folder, its state folder, lock, staging and journal.
pub const PREPARE: PhaseId = PhaseId("prepare");

/// The phase in which an install reads the launcher file's sections and stages the runtime.
pub const GET_RUNTIME: PhaseId = PhaseId("getRuntime");

/// The phase in which an install stages the disc's trees.
pub const COPY_DISC_FILES: PhaseId = PhaseId("copyDiscFiles");

/// The phase in which an install sets the disc's icon into the game's Windows executable.
pub const PERSONALISE: PhaseId = PhaseId("personalise");

/// The phase in which an install stages its copy of the launcher file.
pub const COPY_LAUNCHER: PhaseId = PhaseId("copyLauncher");

/// The phase in which an install asks the staged game for its version.
pub const CHECK_GAME: PhaseId = PhaseId("checkGame");

/// The phase in which an install puts its roots in place and writes its receipt.
pub const PUBLISH: PhaseId = PhaseId("publish");

/// The prefix of a fresh install's staging folder in the state folder, which a random part
/// follows.
pub const INSTALL_STAGING_PREFIX: &str = "install-";

/// The folder an install makes at a folder's top for the game to keep the player's saves in.
pub const SAVES_FOLDER: &str = "saves";

/// The folder an install makes at a folder's top for the game to keep the player's settings in.
pub const CONFIG_FOLDER: &str = "config";

/// The room an install keeps free on its volume beyond what it writes, as the release scripts'
/// installer keeps it.
pub const SPACE_MARGIN: u64 = 64 << 20;

/// The folders at the top of the disc's tree that an install copies, spelled as the game's disc
/// spells them: the names a folder is held to when no disc is at hand to give its own spelling.
const DISC_ROOTS: [&str; 2] = ["Gauntlet", "carddemo"];

/// What a plan found of the disc it copies: the disc's `Gauntlet` and `carddemo` trees.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscSummary {
    /// The container the image keeps the disc in.
    pub container: Container,
    /// The disc's revision.
    pub revision: u8,
    /// How many folders the two trees hold, the two included.
    pub folders: usize,
    /// How many files they hold.
    pub files: usize,
    /// How many bytes those files hold together.
    pub bytes: u64,
}

/// What a plan found of the runtime it installs, as the runtime's release record names it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RuntimeSummary {
    /// The release's version, which is the launcher's.
    pub version: String,
    /// The commit the release was built from.
    pub commit: String,
    /// The platform the release was built for, which is this program's.
    pub platform: Platform,
    /// How many files the runtime holds, its release record included.
    pub files: usize,
    /// How many bytes they declare uncompressed: what installing them writes.
    pub bytes: u64,
}

/// A fresh install planned, with nothing yet written: what it copies from where, the names it
/// puts at the folder's top, the room it needs, and what refuses it or calls for a warning.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InstallPlan {
    /// The folder to install into, made absolute and folded by name as the judging made it.
    pub destination: PathBuf,
    /// The version of the launcher that planned the install, which the runtime is of.
    pub launcher_version: String,
    /// The names the install puts at the folder's top, in byte order and each once: the first
    /// names of the disc's entries, spelled as the disc spells them, and of the runtime's files,
    /// the folders for the player's saves and settings, and the launcher's copy. The launcher's
    /// state folder is made beside them and is not among them.
    pub roots: Vec<RelPath>,
    /// What the install copies of the disc.
    pub disc: DiscSummary,
    /// What it installs of the runtime.
    pub runtime: RuntimeSummary,
    /// How many bytes the launcher file holds, which the install copies whole as the launcher's
    /// copy.
    pub launcher_bytes: u64,
    /// The room the install needs on the folder's volume: the runtime's declared bytes, the disc
    /// files', the launcher file's and [`SPACE_MARGIN`].
    pub needed_bytes: u64,
    /// What refuses the install, in the order found: a system that runs the game only by emulating
    /// it, then the folder's findings.
    pub blockers: Vec<Blocker>,
    /// What the player should be warned of: the folder's warnings.
    pub warnings: Vec<Warning>,
    /// What judging the folder found, its findings among it.
    pub destination_report: DestinationReport,
}

impl InstallPlan {
    /// Whether nothing refuses the install.
    pub fn is_ready(&self) -> bool {
        self.blockers.is_empty()
    }
}

/// Why an install could not be planned or made, as technical detail for logs. What a player reads
/// is chosen from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum InstallError {
    /// A finding refuses an install into the folder.
    #[error("{0}")]
    Blocked(Blocker),
    /// The folder could not be judged.
    #[error(transparent)]
    Destination(DestinationError),
    /// The disc's image could not be read.
    #[error(transparent)]
    Image(MediaError),
    /// The disc's icon cannot be made into the icon of the game's Windows executable.
    #[error(transparent)]
    Icon(IconError),
    /// The runtime the launcher file carries is refused, or is not the one this launcher installs.
    #[error(transparent)]
    Runtime(PayloadError),
    /// The launcher file's sections do not hold what their table says, or its copy could not be
    /// made.
    #[error(transparent)]
    Launcher(SectionsError),
    /// The installation's lock was not taken.
    #[error(transparent)]
    Lock(LockError),
    /// A file operation in the folder failed: in the staging, the state folder or the publish.
    #[error(transparent)]
    Files(FilesError),
    /// The system refused an operation on `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// What the operation was on.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The record of installations would not take the folder.
    #[error("the record of installations refused the destination: {source}")]
    Record {
        /// What the record answered.
        #[source]
        source: io::Error,
    },
    /// The job was asked to stop.
    #[error("Installation cancelled; no game files were installed.")]
    Cancelled,
}

impl InstallError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the finding's that
    /// refuses the install, the rule the inner error gives, `io`, `record` or `cancelled`.
    pub fn rule(&self) -> &'static str {
        match self {
            InstallError::Blocked(blocker) => blocker.rule(),
            InstallError::Destination(error) => error.rule(),
            InstallError::Image(error) => error.rule(),
            InstallError::Icon(error) => error.rule(),
            InstallError::Runtime(error) => error.rule(),
            InstallError::Launcher(error) => error.rule(),
            InstallError::Lock(error) => error.rule(),
            InstallError::Files(error) => error.rule(),
            InstallError::Io { .. } => "io",
            InstallError::Record { .. } => "record",
            InstallError::Cancelled => "cancelled",
        }
    }
}

/// The per-user record of the folders the launcher installed into, which an install tells as it
/// goes: of a folder it begins to install into, so that a start after a crash finds it; of the
/// installation once it stands; and of a folder it leaves as it was, which the record then forgets.
pub trait FolderRecord {
    /// An install begins in `destination`. A refusal refuses the install before anything is made.
    fn pending(&mut self, destination: &Path) -> io::Result<()>;
    /// The installation in `destination` stands. A refusal is only a notice.
    fn installed(&mut self, destination: &Path) -> io::Result<()>;
    /// The install in `destination` left nothing of its own there. A refusal is passed over.
    fn forget(&mut self, destination: &Path) -> io::Result<()>;
}

/// A record that keeps nothing, for an install no list of installations follows.
#[derive(Clone, Copy, Debug, Default)]
pub struct Unrecorded;

impl FolderRecord for Unrecorded {
    fn pending(&mut self, _: &Path) -> io::Result<()> {
        Ok(())
    }

    fn installed(&mut self, _: &Path) -> io::Result<()> {
        Ok(())
    }

    fn forget(&mut self, _: &Path) -> io::Result<()> {
        Ok(())
    }
}

/// A fresh install that stands: its folder, the receipt it wrote, and what the player should be
/// told of it.
#[derive(Clone, Debug)]
pub struct Installed {
    /// The installation's folder.
    pub folder: InstallFolder,
    /// The receipt the install wrote.
    pub receipt: Receipt,
    /// What went wrong without undoing the install, in the order it happened.
    pub notices: Vec<InstallNotice>,
}

/// What went wrong in an install that stands, as technical detail for logs. What a player reads is
/// chosen from it elsewhere.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InstallNotice {
    /// The staged game did not answer when it was asked for its version: it ended with another
    /// status than 0, did not start or did not end in time.
    ProbeFailed {
        /// The game's exit code, when it started and ended with one.
        status: Option<i32>,
        /// The first line it wrote to its standard output.
        first_line: Option<String>,
        /// The end of what it wrote to its standard error.
        stderr_tail: String,
    },
    /// The record of installations would not take the installation.
    NotRecorded {
        /// What the record answered.
        detail: Box<str>,
    },
    /// Cleaning up after the receipt was written stopped part-way: the journal it leaves has the
    /// next start finish it.
    CleanupLeft {
        /// What stopped it.
        detail: Box<str>,
    },
}

/// What an install that failed left in the folder.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Changes {
    /// Nothing: the install failed before it made anything.
    None,
    /// What it made was taken back, and the folder is as it was.
    RolledBack,
    /// Some of what it made is left, with what the next start needs to finish or take it back.
    Kept,
}

/// Why an install failed, and what it left.
#[derive(Debug, thiserror::Error)]
#[error("{error}")]
pub struct InstallFailure {
    /// Why it failed.
    pub error: InstallError,
    /// What it left in the folder.
    pub changes: Changes,
}

impl InstallFailure {
    /// The failure's stable machine name, its error's.
    pub fn rule(&self) -> &'static str {
        self.error.rule()
    }
}

/// Plans a fresh install into `destination` of the disc in `image`, which was judged whole when it
/// was opened, and of the runtime the launcher file `launcher` carries, for a launcher of
/// `launcher_version`.
///
/// In order: the system, which refuses the install when it runs the game only by emulating it,
/// the first of the plan's blockers; the runtime, held to the one this launcher installs, of its
/// platform and version, its files read no further than its release record; on Windows the disc's
/// icon, read no further than [`MAX_TPL_LEN`](gdl_media::MAX_TPL_LEN) and one byte and made into
/// the game's icon, so that an icon the install could not set refuses it now; then the folder,
/// judged for the names the install puts at its top and its state folder, and for the room it
/// needs. A runtime refused, an icon that cannot be made, an image that cannot be read or a folder
/// that cannot be judged is an error; what the judging finds is listed in the plan.
///
/// Nothing is written but the entries the folder's probe makes and removes at once.
pub fn plan_install<R: Read + Seek>(
    image: &mut GameCubeImage,
    launcher: &mut Sections<R>,
    destination: &Path,
    launcher_version: &str,
) -> Result<InstallPlan, InstallError> {
    plan_with(
        image,
        launcher,
        destination,
        launcher_version,
        host_supported(),
        &FolderChecks::SHIPPED,
    )
}

/// Plans an install as [`plan_install`] does, for a system that `host` judged, asking the system
/// of the folder through `checks`.
pub(crate) fn plan_with<R: Read + Seek>(
    image: &mut GameCubeImage,
    launcher: &mut Sections<R>,
    destination: &Path,
    launcher_version: &str,
    host: Result<(), Unsupported>,
    checks: &FolderChecks<'_>,
) -> Result<InstallPlan, InstallError> {
    let (runtime, runtime_roots) = runtime_of(launcher, launcher_version)?;
    #[cfg(windows)]
    check_icon(image)?;
    let disc = disc_of(image);
    let roots = roots_of(image, runtime_roots, runtime.platform);
    let mut names = roots.clone();
    names.push(install_name(STATE_DIR));
    let needed_bytes = runtime
        .bytes
        .saturating_add(disc.bytes)
        .saturating_add(launcher.file_len())
        .saturating_add(SPACE_MARGIN);
    let report =
        judge_with(destination, &names, needed_bytes, checks).map_err(InstallError::Destination)?;
    let mut blockers: Vec<Blocker> = host.err().map(Blocker::from).into_iter().collect();
    blockers.extend(report.blockers.iter().cloned());
    Ok(InstallPlan {
        destination: report.path.clone(),
        launcher_version: launcher_version.to_owned(),
        roots,
        disc,
        runtime,
        launcher_bytes: launcher.file_len(),
        needed_bytes,
        blockers,
        warnings: report.warnings.clone(),
        destination_report: report,
    })
}

/// Installs the game as `plan` planned it, from the disc in `image` and the launcher file
/// `launcher` the plan was made of, telling `record` of the folder as the install goes, its
/// progress and cancel taken from `job`. The system is kept out of idle sleep while it runs.
///
/// In phases, each told before it begins: [`CHECK_DISC`], the folder judged again, its first
/// finding refusing the install as [`InstallError::Blocked`]; [`PREPARE`], the record told, the
/// folder and the missing folders above it made, then the launcher's state folder, which no other
/// launcher may have made meanwhile, the lock taken, the folder judged again under it, the staging
/// made and the journal written; [`GET_RUNTIME`], every section of the launcher file read against
/// its table and the runtime staged, each file against its release record; [`COPY_DISC_FILES`],
/// the disc's trees staged and listed with their digests; on Windows [`PERSONALISE`], the disc's
/// icon set into the staged executable; [`COPY_LAUNCHER`], the launcher file copied whole beside
/// the folders for saves and settings; [`CHECK_GAME`], the staged game asked for its version,
/// whose failure is only a notice; and [`PUBLISH`], every root put in place by a rename that never
/// replaces anything, then the receipt written. The progress counts the bytes of every part
/// together, the sections read and the runtime, the disc and the launcher's copy written, and ends
/// at its total. The cancel is looked at as each phase but the publish begins, and before every
/// block and every entry within one.
///
/// The receipt in place is the point from which the installation stands: what fails after it, in
/// cleaning up, is a notice for the next start to finish. A failure before it is the install's,
/// with what it left: nothing when it failed before it made anything.
pub fn install_game<R: Read + Seek>(
    plan: &InstallPlan,
    image: &mut GameCubeImage,
    launcher: &mut Sections<R>,
    record: &mut dyn FolderRecord,
    job: &Job<'_>,
) -> Result<Installed, InstallFailure> {
    install_game_with(
        plan,
        image,
        launcher,
        record,
        job,
        &InstallSettings::SHIPPED,
    )
}

/// Installs the game as [`install_game`] does, asking the world beyond the folder through
/// `settings`.
pub(crate) fn install_game_with<R: Read + Seek>(
    plan: &InstallPlan,
    image: &mut GameCubeImage,
    launcher: &mut Sections<R>,
    record: &mut dyn FolderRecord,
    job: &Job<'_>,
    settings: &InstallSettings<'_>,
) -> Result<Installed, InstallFailure> {
    let _awake = keep_awake();
    let mut install = Install::new(plan, *job, settings);
    match install.run(image, launcher, record) {
        Ok(installed) => Ok(installed),
        Err(error) => Err(InstallFailure {
            error,
            changes: install.left(record),
        }),
    }
}

/// The names an install for `platform` may make at a folder's top, whatever disc and runtime it
/// installs, for judging a folder when neither is at hand: the disc's two folders as the game's
/// disc spells them, the folders for the player's saves and settings, the launcher's state folder,
/// for a platform a release is built for the launcher's copy and the game's executable, then the
/// files and folders a runtime may have at its top, Linux's libraries only for Linux. The
/// libraries a Windows runtime keeps at its top are left out, since only the runtime names them.
pub fn install_names(platform: Option<Platform>) -> Vec<RelPath> {
    let mut names = DISC_ROOTS.to_vec();
    names.extend([SAVES_FOLDER, CONFIG_FOLDER, STATE_DIR]);
    if let Some(platform) = platform {
        names.extend([installed_launcher_name(platform), platform.executable()]);
    }
    names.extend(ROOT_FILES);
    names.extend(
        ROOT_FOLDERS
            .into_iter()
            .filter(|&folder| folder != LIB_FOLDER || platform == Some(Platform::LinuxX64)),
    );
    names.into_iter().map(install_name).collect()
}

/// The runtime `launcher` carries, held to the one a launcher of `launcher_version` installs, as a
/// plan records it, with the first names of its files. The runtime is let go of before anything
/// else is read.
fn runtime_of<R: Read + Seek>(
    launcher: &mut Sections<R>,
    launcher_version: &str,
) -> Result<(RuntimeSummary, Vec<RelPath>), InstallError> {
    let payload = launcher.runtime().map_err(InstallError::Runtime)?;
    payload
        .for_launcher(launcher_version)
        .map_err(InstallError::Runtime)?;
    let info = payload.build_info();
    let summary = RuntimeSummary {
        version: info.version.clone(),
        commit: info.commit.clone(),
        platform: info.platform,
        files: payload.entries().len(),
        bytes: payload.declared_size(),
    };
    let roots = payload
        .entries()
        .iter()
        .map(|entry| first_of(&entry.name, &PAYLOAD_NAMES))
        .collect();
    Ok((summary, roots))
}

/// Makes the disc's icon into the icon the install sets into the game's Windows executable, and
/// refuses the install when it cannot be: the icon is read no further than one byte past the most
/// a TPL may hold, as the install reads the copy it makes. A judged disc always holds its icon; one
/// without it would be refused as an icon of no bytes.
#[cfg(windows)]
fn check_icon(image: &mut GameCubeImage) -> Result<(), InstallError> {
    let icon = icon_entry(image.entries()).cloned();
    let tpl = match icon {
        Some(entry) => image
            .read_entry(&entry, MAX_TPL_LEN)
            .map_err(InstallError::Image)?,
        None => Vec::new(),
    };
    let frame = first_frame(&tpl).map_err(|error| InstallError::Icon(IconError::Tpl(error)))?;
    icon_resources(&frame).map_err(InstallError::Icon)?;
    Ok(())
}

/// The disc's icon among `entries`, the disc's own spelling of [`DISC_ICON`] in any ASCII letter
/// case, as the disc's names are compared.
#[cfg(windows)]
fn icon_entry(entries: &[DiscEntry]) -> Option<&DiscEntry> {
    entries
        .iter()
        .find(|entry| entry.path.as_str().eq_ignore_ascii_case(DISC_ICON))
}

/// What a plan records of the disc in `image`.
fn disc_of(image: &GameCubeImage) -> DiscSummary {
    let folders = image
        .entries()
        .iter()
        .filter(|entry| entry.kind == DiscEntryKind::Folder)
        .count();
    DiscSummary {
        container: image.container(),
        revision: image.revision(),
        folders,
        files: image.entries().len() - folders,
        bytes: image.files_len(),
    }
}

/// The names an install of the disc in `image` and of a runtime whose files' first names are
/// `runtime` puts at a folder's top for `platform`, in byte order and each once.
fn roots_of(image: &GameCubeImage, runtime: Vec<RelPath>, platform: Platform) -> Vec<RelPath> {
    let disc = image
        .entries()
        .iter()
        .map(|entry| first_of(&entry.path, &DISC_NAMES));
    let own = [
        SAVES_FOLDER,
        CONFIG_FOLDER,
        installed_launcher_name(platform),
    ]
    .map(install_name);
    let roots: BTreeSet<RelPath> = disc.chain(runtime).chain(own).collect();
    roots.into_iter().collect()
}

/// The first name of `name`, which was read under `rules`: one of its components, and so a name
/// those rules take alone.
fn first_of(name: &RelPath, rules: &NameRules) -> RelPath {
    RelPath::parse(name.first(), rules).expect("a checked name's first component keeps its rules")
}

/// `name`, one of the names an install makes of its own at a folder's top.
fn install_name(name: &str) -> RelPath {
    RelPath::parse(name, &PAYLOAD_NAMES).expect("an install's names keep the payload's rules")
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::collections::BTreeMap;
    use std::fs;
    use std::io::{self, Cursor, Read, Seek};
    use std::path::{Path, PathBuf};
    use std::process::ExitStatus;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};

    use gdl_files::{
        Cancel, FilesError, FolderProbe, Job, NoProgress, PAYLOAD_NAMES, PhaseId, ProbedFolder,
        Progress, RelPath, Volume,
    };
    #[cfg(windows)]
    use gdl_fixtures::fake_sign;
    use gdl_fixtures::{
        FstEntry, ICON_DATA_AT, RuntimeFiles, SYNTHETIC_ISO_LEN, append_sections, folder_names,
        fst, icon_iso, iso_of, sha256_hex, snapshot, snapshot_outside, synthetic_ciso,
        synthetic_iso, tiny_elf, tiny_pe, tpl,
    };
    use gdl_media::{Container, GameCubeImage, MediaError};
    use gdl_release::{Platform, Version};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::settings::{FaultPoint, FaultPoints, InstallSettings, NoFaults, ProbeFn};
    use super::{
        CHECK_DISC, CHECK_GAME, CONFIG_FOLDER, COPY_DISC_FILES, COPY_LAUNCHER, Changes, DISC_FILES,
        DiscSummary, FolderRecord, GET_RUNTIME, INSTALL_JOURNAL, InstallError, InstallFailure,
        InstallNotice, InstallPlan, Installed, PREPARE, PUBLISH, RuntimeSummary, SAVES_FOLDER,
        SPACE_MARGIN, Unrecorded, install_game_with, install_names, plan_install, plan_with,
    };
    use crate::destination::{Blocker, DestinationError, FolderChecks};
    use crate::folder::{
        InstallFolder, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE, STATE_DIR,
    };
    use crate::host::Unsupported;
    use crate::icon::IconError;
    use crate::inspect::FolderState;
    use crate::payload::{LIB_FOLDER, PayloadError, ROOT_FOLDERS};
    use crate::probe::{GameStartError, VersionProbe, probe_game_version_with};
    use crate::receipt::installed_launcher_name;
    use crate::sections::{RUNTIME_SECTION, Sections, SectionsError, WINDOW_SECTION};

    /// The version the tests' launcher and runtimes are of.
    const VERSION: &str = "1.0.0-beta";

    /// The commit the tests' runtimes say they were built from.
    const COMMIT: &str = "test-commit";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The bytes the icon disc's files hold: `WAR.WAD`'s 32, `TOWER.WAD`'s 5 and its icon's 192.
    const ICON_DISC_BYTES: u64 = 32 + 5 + 192;

    /// The machine Windows on ARM64 names itself by, which runs x64 programs only by emulating
    /// them.
    const ARM64_MACHINE: u16 = 0xAA64;

    /// What a probe finds in a folder of the tests' own, which takes anything: Windows' file
    /// systems ignore letter case, and the Linux tests' do not.
    const WRITABLE: FolderProbe = FolderProbe {
        writable: true,
        ignores_case: cfg!(windows),
    };

    /// The names an install of the icon disc and the standard runtime puts at a folder's top on
    /// Windows and on Linux, in byte order.
    const WINDOWS_ROOTS: [&str; 11] = [
        "Gauntlet",
        "GauntletDarkLegacy-Launcher.exe",
        "VERSION",
        "build-info.json",
        "carddemo",
        "config",
        "data",
        "gauntlet.exe",
        "portable.flag",
        "saves",
        "shaders",
    ];
    const LINUX_ROOTS: [&str; 11] = [
        "Gauntlet",
        "GauntletDarkLegacy-Launcher",
        "VERSION",
        "build-info.json",
        "carddemo",
        "config",
        "data",
        "gauntlet",
        "portable.flag",
        "saves",
        "shaders",
    ];

    /// The platform this test program was built for, whose runtime it installs.
    fn this_platform() -> Platform {
        Platform::of_this_build().expect("a release is built for the system the tests run on")
    }

    /// The platform a release is built for that the tests do not run on.
    fn other_platform() -> Platform {
        Platform::ALL
            .into_iter()
            .find(|&platform| platform != this_platform())
            .expect("a release is built for two platforms")
    }

    /// The runtime of the installer's own tests, for `platform`, of the launcher's version.
    fn standard_runtime(platform: Platform) -> RuntimeFiles {
        RuntimeFiles::standard(platform.as_str(), VERSION)
    }

    /// A launcher file of this system's kind carrying `sections`, signed on Windows, as a release
    /// is, so that the file runs on past its table and trailer.
    fn launcher_file(sections: &[(&str, &[u8])]) -> Vec<u8> {
        let mut file = if cfg!(windows) { tiny_pe() } else { tiny_elf() };
        append_sections(&mut file, sections, 0);
        #[cfg(windows)]
        fake_sign(&mut file);
        file
    }

    /// The sections of the launcher file `file`, found and their table checked.
    fn launcher_of(file: Vec<u8>) -> Sections<Cursor<Vec<u8>>> {
        Sections::find(Cursor::new(file))
            .expect("the tests' launcher files keep the sections' rules")
            .expect("the tests' launcher files carry sections")
    }

    /// A launcher file carrying `runtime` as its runtime zip, with the sections found.
    fn launcher_carrying(runtime: &RuntimeFiles) -> Sections<Cursor<Vec<u8>>> {
        launcher_of(launcher_file(&[(RUNTIME_SECTION, &runtime.zip())]))
    }

    /// The image `bytes`, written into `dir` and opened as the player's image is.
    fn opened(dir: &Path, bytes: &[u8]) -> GameCubeImage {
        let path = dir.join("game.iso");
        fs::write(&path, bytes).unwrap();
        GameCubeImage::open(&path).unwrap()
    }

    /// How many bytes `runtime`'s files declare, its release record included.
    fn declared(runtime: &RuntimeFiles) -> u64 {
        let files: usize = runtime.files.iter().map(|(_, data)| data.len()).sum();
        (files + runtime.build_info_bytes().len()) as u64
    }

    /// `names`, each as text.
    fn texts(names: &[RelPath]) -> Vec<&str> {
        names.iter().map(RelPath::as_str).collect()
    }

    /// Plans the install of the disc `disc` and of `runtime` into `destination`, on a system that
    /// `host` judged, with a folder probe that finds the folder `probe` and a volume of
    /// `available` bytes that runs programs.
    fn plan_standing_in(
        disc: &[u8],
        runtime: &RuntimeFiles,
        destination: &Path,
        host: Result<(), Unsupported>,
        probe: FolderProbe,
        available: u64,
    ) -> Result<InstallPlan, InstallError> {
        let source = tempfile::tempdir().unwrap();
        let mut image = opened(source.path(), disc);
        let mut launcher = launcher_carrying(runtime);
        let probe =
            move |_: &Path, _: ProbedFolder| -> Result<FolderProbe, FilesError> { Ok(probe) };
        let volume = move |_: &Path| -> io::Result<Volume> {
            Ok(Volume {
                available,
                noexec: false,
            })
        };
        let checks = FolderChecks {
            probe: &probe,
            volume: &volume,
            ..FolderChecks::SHIPPED
        };
        plan_with(
            &mut image,
            &mut launcher,
            destination,
            VERSION,
            host,
            &checks,
        )
    }

    #[test]
    fn a_plan_reads_the_disc_and_the_runtime_and_judges_the_folder() {
        let source = tempfile::tempdir().unwrap();
        let top = tempfile::tempdir().unwrap();
        let mut image = opened(source.path(), &icon_iso());
        let platform = this_platform();
        let runtime = standard_runtime(platform);
        let file = launcher_file(&[(RUNTIME_SECTION, &runtime.zip())]);
        let file_len = file.len() as u64;
        let mut launcher = launcher_of(file);
        let destination = top.path().join("Games").join("Gauntlet Dark Legacy");

        let plan = plan_install(&mut image, &mut launcher, &destination, VERSION).unwrap();
        assert!(plan.is_ready(), "{plan:?}");
        assert!(plan.blockers.is_empty(), "{plan:?}");
        assert_eq!(plan.destination, destination);
        assert_eq!(plan.launcher_version, VERSION);
        let expected = match platform {
            Platform::WindowsX64 => WINDOWS_ROOTS,
            Platform::LinuxX64 => LINUX_ROOTS,
        };
        assert_eq!(texts(&plan.roots), expected);
        assert_eq!(
            plan.disc,
            DiscSummary {
                container: Container::GameCubeIso,
                revision: 1,
                folders: 5,
                files: 3,
                bytes: ICON_DISC_BYTES,
            }
        );
        assert_eq!(
            plan.runtime,
            RuntimeSummary {
                version: VERSION.to_owned(),
                commit: COMMIT.to_owned(),
                platform,
                files: runtime.files.len() + 1,
                bytes: declared(&runtime),
            }
        );
        // The launcher's copy is the whole file, its signature and all where it has one.
        assert_eq!(plan.launcher_bytes, file_len);
        assert_eq!(
            plan.needed_bytes,
            declared(&runtime) + ICON_DISC_BYTES + file_len + SPACE_MARGIN
        );

        // The folder was judged for the plan's names, the state folder's among them, and the
        // plan carries what the judging found.
        let report = &plan.destination_report;
        assert_eq!(report.path, destination);
        assert_eq!(report.exists, Some(false));
        assert_eq!(report.nearest_existing.as_deref(), Some(top.path()));
        assert_eq!(report.probe, Some(WRITABLE));
        assert!(report.available_bytes.is_some_and(|bytes| bytes > 0));
        assert_eq!(plan.warnings, report.warnings);
        assert_eq!(plan.blockers, report.blockers);
        // Nothing was made, in the folder or in the image's.
        assert!(folder_names(top.path()).unwrap().is_empty());
        assert_eq!(folder_names(source.path()).unwrap(), ["game.iso"]);
    }

    #[test]
    fn a_ciso_plans_as_its_iso() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let runtime = standard_runtime(this_platform());
        let plan_of = |disc: &[u8]| {
            plan_standing_in(disc, &runtime, &destination, Ok(()), WRITABLE, u64::MAX).unwrap()
        };
        let iso = plan_of(&icon_iso());
        let ciso = plan_of(&synthetic_ciso(&icon_iso()));
        assert_eq!(iso.disc.container, Container::GameCubeIso);
        assert_eq!(ciso.disc.container, Container::GameCubeCiso);
        assert_eq!(
            ciso,
            InstallPlan {
                disc: DiscSummary {
                    container: Container::GameCubeCiso,
                    ..iso.disc.clone()
                },
                ..iso
            }
        );
    }

    #[test]
    fn the_host_comes_first_and_the_folders_findings_follow() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let runtime = standard_runtime(this_platform());
        let unsupported = Unsupported {
            native_machine: ARM64_MACHINE,
        };
        let not_writable = FolderProbe {
            writable: false,
            ignores_case: false,
        };
        let plan = plan_standing_in(
            &icon_iso(),
            &runtime,
            &destination,
            Err(unsupported),
            not_writable,
            0,
        )
        .unwrap();
        assert!(!plan.is_ready());
        let folders = vec![
            Blocker::NotWritable {
                path: top.path().to_path_buf(),
            },
            Blocker::Space {
                needed: plan.needed_bytes,
                available: 0,
            },
        ];
        assert_eq!(plan.destination_report.blockers, folders);
        let mut expected = vec![Blocker::Unsupported {
            native_machine: ARM64_MACHINE,
        }];
        expected.extend(folders);
        assert_eq!(plan.blockers, expected);

        // A system that runs the game as it is built refuses nothing of its own.
        let plan =
            plan_standing_in(&icon_iso(), &runtime, &destination, Ok(()), WRITABLE, 0).unwrap();
        assert_eq!(plan.blockers, plan.destination_report.blockers);
        assert!(
            matches!(plan.blockers[..], [Blocker::Space { .. }]),
            "{plan:?}"
        );
    }

    #[test]
    fn the_room_needed_counts_the_runtime_the_disc_and_the_launcher_file() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let runtime = standard_runtime(this_platform());
        let needed = plan_standing_in(&icon_iso(), &runtime, &destination, Ok(()), WRITABLE, 0)
            .unwrap()
            .needed_bytes;
        // Exactly the room needed is room enough, and a byte less is not.
        let plan = plan_standing_in(
            &icon_iso(),
            &runtime,
            &destination,
            Ok(()),
            WRITABLE,
            needed,
        )
        .unwrap();
        assert!(plan.is_ready(), "{plan:?}");
        assert_eq!(plan.destination_report.available_bytes, Some(needed));
        let plan = plan_standing_in(
            &icon_iso(),
            &runtime,
            &destination,
            Ok(()),
            WRITABLE,
            needed - 1,
        )
        .unwrap();
        assert_eq!(
            plan.blockers,
            [Blocker::Space {
                needed,
                available: needed - 1,
            }]
        );
    }

    #[test]
    fn a_runtime_of_another_platform_is_refused_before_anything_is_made() {
        let source = tempfile::tempdir().unwrap();
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let other = other_platform();
        let mut image = opened(source.path(), &icon_iso());
        let mut launcher = launcher_carrying(&standard_runtime(other));
        let refused = plan_install(&mut image, &mut launcher, &destination, VERSION).unwrap_err();
        assert!(
            matches!(
                refused,
                InstallError::Runtime(PayloadError::OtherPlatform { platform }) if platform == other
            ),
            "{refused:?}"
        );
        assert_eq!(refused.rule(), "otherPlatform");
        assert_eq!(
            refused.to_string(),
            format!(
                "This installer was built for a different operating system: the runtime is for {}",
                other.as_str()
            )
        );
        assert!(folder_names(top.path()).unwrap().is_empty());
    }

    #[test]
    fn a_runtime_the_launcher_does_not_install_is_refused() {
        let source = tempfile::tempdir().unwrap();
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let mut image = opened(source.path(), &icon_iso());
        let newer = RuntimeFiles::standard(this_platform().as_str(), "1.0.1");
        let cases: [(Vec<u8>, &str); 3] = [
            // A runtime of another version.
            (
                launcher_file(&[(RUNTIME_SECTION, &newer.zip())]),
                "otherVersion",
            ),
            // A launcher file that carries no runtime.
            (
                launcher_file(&[(WINDOW_SECTION, b"the window program")]),
                "missingSection",
            ),
            // A runtime section that holds no zip.
            (launcher_file(&[(RUNTIME_SECTION, b"not a zip")]), "archive"),
        ];
        for (file, rule) in cases {
            let mut launcher = launcher_of(file);
            let refused =
                plan_install(&mut image, &mut launcher, &destination, VERSION).unwrap_err();
            assert!(matches!(refused, InstallError::Runtime(_)), "{refused:?}");
            assert_eq!(refused.rule(), rule, "{refused:?}");
            assert!(folder_names(top.path()).unwrap().is_empty(), "{rule}");
        }
    }

    #[cfg(windows)]
    #[test]
    fn a_damaged_icon_is_refused_while_planning() {
        use gdl_fixtures::{iso_with_icon, tpl};
        use gdl_media::{MAX_TPL_LEN, TplRule};

        let source = tempfile::tempdir().unwrap();
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let runtime = standard_runtime(this_platform());
        let too_long = vec![0; usize::try_from(MAX_TPL_LEN).unwrap() + 1];
        let cases = [
            // The installer tests' disc, whose icon is four bytes of text.
            (synthetic_iso(), TplRule::Size),
            // An icon of an encoding the installer does not decode.
            (iso_with_icon(&tpl(9, 8, 8)), TplRule::Format),
            // An icon longer than a TPL may be.
            (iso_with_icon(&too_long), TplRule::Size),
        ];
        for (disc, expected) in cases {
            let mut image = opened(source.path(), &disc);
            let mut launcher = launcher_carrying(&runtime);
            let refused =
                plan_install(&mut image, &mut launcher, &destination, VERSION).unwrap_err();
            assert!(
                matches!(
                    refused,
                    InstallError::Icon(IconError::Tpl(MediaError::Tpl { rule, .. }))
                        if rule == expected
                ),
                "{refused:?}"
            );
            assert_eq!(refused.rule(), expected.as_str());
            drop(image);
            assert!(folder_names(top.path()).unwrap().is_empty());
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_linux_plan_does_not_read_the_icon() {
        // The installer tests' disc, whose icon is four bytes of text, which Windows refuses
        // while planning.
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let runtime = standard_runtime(this_platform());
        let plan = plan_standing_in(
            &synthetic_iso(),
            &runtime,
            &destination,
            Ok(()),
            WRITABLE,
            u64::MAX,
        )
        .unwrap();
        assert!(plan.is_ready(), "{plan:?}");
        assert_eq!(plan.disc.bytes, 32 + 5 + 4);
        assert_eq!(texts(&plan.roots), LINUX_ROOTS);
    }

    #[test]
    fn a_plan_names_the_roots_the_folder_holds_already() {
        let source = tempfile::tempdir().unwrap();
        let top = tempfile::tempdir().unwrap();
        let destination = top.path();
        for folder in ["Gauntlet", SAVES_FOLDER, CONFIG_FOLDER] {
            fs::create_dir(destination.join(folder)).unwrap();
        }
        fs::write(destination.join("DATA"), b"the player's own").unwrap();
        let before = folder_names(destination).unwrap();
        let mut image = opened(source.path(), &icon_iso());
        let mut launcher = launcher_carrying(&standard_runtime(this_platform()));
        let plan = plan_install(&mut image, &mut launcher, destination, VERSION).unwrap();
        assert!(!plan.is_ready());
        // Each named as the install spells it, in byte order. Names are compared in upper case on
        // every system, so on Linux the game's executable, `gauntlet`, is taken by the disc's
        // folder too.
        let mut taken = vec!["Gauntlet", "config", "data", "saves"];
        if this_platform() == Platform::LinuxX64 {
            taken.insert(3, "gauntlet");
        }
        assert_eq!(
            plan.blockers,
            [Blocker::Collision {
                names: taken.into_iter().map(str::to_owned).collect(),
            }]
        );
        assert_eq!(folder_names(destination).unwrap(), before);
        assert_eq!(
            fs::read(destination.join("DATA")).unwrap(),
            b"the player's own"
        );
    }

    #[test]
    fn a_folder_that_cannot_be_judged_is_refused() {
        let source = tempfile::tempdir().unwrap();
        let mut image = opened(source.path(), &icon_iso());
        let mut launcher = launcher_carrying(&standard_runtime(this_platform()));
        // An empty path names no folder, and cannot be made absolute.
        let refused = plan_install(&mut image, &mut launcher, Path::new(""), VERSION).unwrap_err();
        assert!(
            matches!(
                refused,
                InstallError::Destination(DestinationError::Io { .. })
            ),
            "{refused:?}"
        );
        assert_eq!(refused.rule(), "io");
    }

    #[test]
    fn an_invalid_image_is_refused_before_anything_is_made() {
        // The disc is judged whole as its image is opened, so a plan is never made of one that is
        // refused, and nothing is made for it.
        let source = tempfile::tempdir().unwrap();
        for (bytes, rule) in [
            (Vec::new(), "outsideImage"),
            (b"not a game".to_vec(), "outsideImage"),
        ] {
            let path = source.path().join("game.iso");
            fs::write(&path, &bytes).unwrap();
            let refused = GameCubeImage::open(&path).unwrap_err();
            assert_eq!(refused.rule(), rule, "{refused:?}");
        }
    }

    #[test]
    fn a_plans_names_are_among_those_a_folder_is_checked_against() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let platform = this_platform();
        let plan = plan_standing_in(
            &icon_iso(),
            &standard_runtime(platform),
            &destination,
            Ok(()),
            WRITABLE,
            u64::MAX,
        )
        .unwrap();
        let checked = install_names(Some(platform));
        for root in &plan.roots {
            assert!(checked.contains(root), "{root:?} of {checked:?}");
        }
        let state = RelPath::parse(STATE_DIR, &PAYLOAD_NAMES).unwrap();
        assert!(checked.contains(&state));
        assert!(!plan.roots.contains(&state));
    }

    #[test]
    fn a_folder_is_checked_against_every_name_an_install_makes() {
        let common_files = ["VERSION", "build-info.json", "portable.flag"];
        let common_folders = ["shaders", "data", "licenses"];
        let mut windows = vec!["Gauntlet", "carddemo", "saves", "config", STATE_DIR];
        windows.extend(["GauntletDarkLegacy-Launcher.exe", "gauntlet.exe"]);
        windows.extend(common_files);
        windows.extend(common_folders);
        let mut linux = vec!["Gauntlet", "carddemo", "saves", "config", STATE_DIR];
        linux.extend(["GauntletDarkLegacy-Launcher", "gauntlet"]);
        linux.extend(common_files);
        linux.extend(common_folders);
        linux.push("lib");
        let mut unknown = vec!["Gauntlet", "carddemo", "saves", "config", STATE_DIR];
        unknown.extend(common_files);
        unknown.extend(common_folders);
        for (platform, expected) in [
            (Some(Platform::WindowsX64), windows),
            (Some(Platform::LinuxX64), linux),
            (None, unknown),
        ] {
            assert_eq!(texts(&install_names(platform)), expected, "{platform:?}");
        }
        assert!(ROOT_FOLDERS.contains(&LIB_FOLDER));
        // The room an install keeps free, as the release scripts' installer keeps it.
        assert_eq!(SPACE_MARGIN, 64 << 20);
    }

    #[test]
    fn install_errors_name_their_rules_and_stay_small() {
        let path = PathBuf::from("game");
        let cases = [
            (
                InstallError::Blocked(Blocker::Space {
                    needed: 2,
                    available: 1,
                }),
                "space",
                "Not enough free disk space for the installation".to_owned(),
            ),
            (
                InstallError::Destination(DestinationError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                }),
                "io",
                format!("{}: refused", path.display()),
            ),
            (
                InstallError::Image(MediaError::Cancelled),
                "cancelled",
                "cancelled".to_owned(),
            ),
            (
                InstallError::Icon(IconError::Decoded),
                "decoded",
                "Invalid decoded icon".to_owned(),
            ),
            (
                InstallError::Runtime(PayloadError::Sections(SectionsError::NoSections)),
                "noSections",
                SectionsError::NoSections.to_string(),
            ),
            (
                InstallError::Launcher(SectionsError::TableHash),
                "tableHash",
                SectionsError::TableHash.to_string(),
            ),
            (
                InstallError::Lock(LockError::Busy),
                "busy",
                "Another job is using this installation".to_owned(),
            ),
            (
                InstallError::Files(FilesError::Exists { path: path.clone() }),
                "exists",
                format!(
                    "Destination changed during installation: {}",
                    path.display()
                ),
            ),
            (
                InstallError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                },
                "io",
                format!("{}: refused", path.display()),
            ),
            (
                InstallError::Record {
                    source: io::Error::other("refused"),
                },
                "record",
                "the record of installations refused the destination: refused".to_owned(),
            ),
            (
                InstallError::Cancelled,
                "cancelled",
                "Installation cancelled; no game files were installed.".to_owned(),
            ),
        ];
        for (error, rule, text) in cases {
            assert_eq!(error.rule(), rule, "{error:?}");
            assert_eq!(error.to_string(), text, "{error:?}");
            let failure = InstallFailure {
                error,
                changes: Changes::Kept,
            };
            assert_eq!(failure.rule(), rule);
            assert_eq!(failure.to_string(), text);
        }
        assert!(size_of::<InstallError>() <= MOST_ERROR_BYTES);
        assert!(size_of::<InstallFailure>() <= MOST_ERROR_BYTES);
        assert!(size_of::<DestinationError>() <= MOST_ERROR_BYTES);
        assert!(size_of::<IconError>() <= MOST_ERROR_BYTES);
    }

    /// The list of what an install of the icon disc copied, as the install writes it on every
    /// system.
    const ICON_DISC_FILES: &str = r#"{
  "schema": 1,
  "minLauncher": "1.0.0-beta",
  "folders": [
    "Gauntlet",
    "Gauntlet/PDATA",
    "Gauntlet/WDATA",
    "Gauntlet/EMPTY",
    "carddemo"
  ],
  "files": [
    {
      "path": "Gauntlet/PDATA/WAR.WAD",
      "size": 32,
      "sha256": "c35e4f503d52e9d3477d1c0366e628ab3aaa4202ee48b06fb52a271fc7f1cc51"
    },
    {
      "path": "Gauntlet/WDATA/TOWER.WAD",
      "size": 5,
      "sha256": "ad2015263f339bff96818238148b790bd02be561472f4ea7db44498caa0b6558"
    },
    {
      "path": "carddemo/icon.tpl",
      "size": 192,
      "sha256": "b8d54eaff3a13e8a1a22f4823b9c54a0471c3cfb3396ca1580642449b3af0103"
    }
  ]
}
"#;

    /// The SHA-256 digest of no bytes.
    const EMPTY_SHA256: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    /// What the stand-in personaliser appends to the executable it is given.
    #[cfg(windows)]
    const ICON_MARK: &[u8] = b" with the disc's icon";

    /// The longest a probe of a program that cannot load may take before the install goes on.
    const PROBE_FAILS_WITHIN: Duration = Duration::from_secs(5);

    /// The disc and the launcher file an install test installs from: the disc written as the
    /// player's image is, and a launcher file of this system's kind carrying the standard runtime.
    struct Inputs {
        /// The folder the image is written into.
        source: TempDir,
        /// The runtime the launcher file carries.
        runtime: RuntimeFiles,
        /// The launcher file.
        launcher: Vec<u8>,
    }

    impl Inputs {
        fn of(disc: &[u8]) -> Inputs {
            let source = tempfile::tempdir().unwrap();
            fs::write(source.path().join("game.iso"), disc).unwrap();
            let runtime = standard_runtime(this_platform());
            let launcher = launcher_file(&[(RUNTIME_SECTION, &runtime.zip())]);
            Inputs {
                source,
                runtime,
                launcher,
            }
        }

        fn image(&self) -> GameCubeImage {
            GameCubeImage::open(&self.source.path().join("game.iso")).unwrap()
        }

        fn sections(&self) -> Sections<Cursor<Vec<u8>>> {
            launcher_of(self.launcher.clone())
        }

        /// The install of the inputs into `destination`, planned.
        fn plan(&self, destination: &Path) -> InstallPlan {
            plan_install(
                &mut self.image(),
                &mut self.sections(),
                destination,
                VERSION,
            )
            .unwrap()
        }

        /// Installs the inputs as `plan` planned, telling `record`, with `job`'s cancel and
        /// progress and the world beyond the folder as `settings` gives it.
        fn install(
            &self,
            plan: &InstallPlan,
            record: &mut dyn FolderRecord,
            job: &Job<'_>,
            settings: &InstallSettings<'_>,
        ) -> Result<Installed, InstallFailure> {
            install_game_with(
                plan,
                &mut self.image(),
                &mut self.sections(),
                record,
                job,
                settings,
            )
        }

        /// Plans and installs the inputs into `destination` with the stand-ins, nothing recorded
        /// and no progress followed.
        fn install_into(&self, destination: &Path) -> Result<Installed, InstallFailure> {
            let plan = self.plan(destination);
            let cancel = Cancel::default();
            let job = Job {
                cancel: &cancel,
                progress: &NoProgress,
            };
            self.install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&NoFaults, &answering),
            )
        }
    }

    /// The world beyond the folder as the install tests give it: `faults`, the probe `probe`, on
    /// Windows a personaliser that marks the executable rather than edit it, and a shell nobody
    /// tells.
    fn standing_in<'a>(faults: &'a dyn FaultPoints, probe: &'a ProbeFn) -> InstallSettings<'a> {
        InstallSettings {
            faults,
            probe,
            #[cfg(windows)]
            personalise: &marking,
            notify_shell: &unheard,
        }
    }

    /// A shell that is told nothing it keeps.
    fn unheard(_: &Path) {}

    /// The status a program that ended with exit code `code` has.
    #[cfg(unix)]
    fn exit_status(code: u8) -> ExitStatus {
        use std::os::unix::process::ExitStatusExt;
        ExitStatus::from_raw(i32::from(code) << 8)
    }

    /// The status a program that ended with exit code `code` has.
    #[cfg(windows)]
    fn exit_status(code: u8) -> ExitStatus {
        use std::os::windows::process::ExitStatusExt;
        ExitStatus::from_raw(u32::from(code))
    }

    /// A probe that finds the game answering with the launcher's version.
    fn answering(_: &Path, _: &Cancel) -> Result<VersionProbe, GameStartError> {
        Ok(VersionProbe {
            status: exit_status(0),
            first_line: Some(VERSION.to_owned()),
            stderr_tail: String::new(),
        })
    }

    /// A probe that finds the game ending with exit code 3, saying why on its standard error.
    fn failing(_: &Path, _: &Cancel) -> Result<VersionProbe, GameStartError> {
        Ok(VersionProbe {
            status: exit_status(3),
            first_line: Some("no".to_owned()),
            stderr_tail: "the game is broken".to_owned(),
        })
    }

    /// A personaliser that appends [`ICON_MARK`] to the executable rather than set an icon into it,
    /// as no executable the tests stage is a program the system edits.
    #[cfg(windows)]
    fn marking(
        executable: &Path,
        tpl: &[u8],
    ) -> Result<crate::icon::Personalised, crate::icon::IconError> {
        let mut bytes = fs::read(executable).unwrap();
        bytes.extend_from_slice(ICON_MARK);
        fs::write(executable, &bytes).unwrap();
        Ok(crate::icon::Personalised {
            executable_sha256: gdl_media::Sha256Digest::of(&bytes),
            icon_sha256: gdl_media::Sha256Digest::of(tpl),
        })
    }

    /// A record of installations that keeps what it is told, and refuses a pending folder when
    /// asked to; a stand-in for another launcher may act as the pending folder is told.
    #[derive(Default)]
    struct Heard {
        /// What it was told, by the call's name, in order.
        calls: Vec<(&'static str, PathBuf)>,
        /// Whether it refuses a pending folder.
        refuse_pending: bool,
        /// What happens as it is told of a pending folder.
        on_pending: Option<fn(&Path)>,
    }

    impl FolderRecord for Heard {
        fn pending(&mut self, destination: &Path) -> io::Result<()> {
            self.calls.push(("pending", destination.to_path_buf()));
            if let Some(act) = self.on_pending {
                act(destination);
            }
            if self.refuse_pending {
                return Err(io::Error::other("the record is full"));
            }
            Ok(())
        }

        fn installed(&mut self, destination: &Path) -> io::Result<()> {
            self.calls.push(("installed", destination.to_path_buf()));
            Ok(())
        }

        fn forget(&mut self, destination: &Path) -> io::Result<()> {
            self.calls.push(("forget", destination.to_path_buf()));
            Ok(())
        }
    }

    /// What an install told its progress.
    #[derive(Clone, Debug, PartialEq, Eq)]
    enum Said {
        /// A phase began.
        Phase(&'static str),
        /// This much of the total was done, of what file.
        Advance(u64, u64, Option<String>),
    }

    /// A progress that keeps what it is told, and asks the job to stop once an advance names the
    /// file it waits for.
    #[derive(Default)]
    struct Told {
        said: Mutex<Vec<Said>>,
        cancel_at: Option<(Cancel, String)>,
    }

    impl Told {
        fn said(&self) -> Vec<Said> {
            self.said.lock().unwrap().clone()
        }

        fn phases(&self) -> Vec<&'static str> {
            self.said()
                .into_iter()
                .filter_map(|said| match said {
                    Said::Phase(phase) => Some(phase),
                    Said::Advance(..) => None,
                })
                .collect()
        }

        fn advances(&self) -> Vec<(u64, u64, Option<String>)> {
            self.said()
                .into_iter()
                .filter_map(|said| match said {
                    Said::Advance(done, total, item) => Some((done, total, item)),
                    Said::Phase(_) => None,
                })
                .collect()
        }
    }

    impl Progress for Told {
        fn phase(&self, phase: PhaseId) {
            self.said.lock().unwrap().push(Said::Phase(phase.0));
        }

        fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
            let item = item.map(|item| item.as_str().to_owned());
            if let Some((cancel, at)) = &self.cancel_at
                && item.as_deref() == Some(at.as_str())
            {
                cancel.cancel();
            }
            self.said
                .lock()
                .unwrap()
                .push(Said::Advance(done, total, item));
        }
    }

    /// Every entry of the tree at `root`, by its path under it with `/` between its names: a folder
    /// as `None` and a file as the digest of its bytes. A link is refused.
    fn tree(root: &Path) -> BTreeMap<String, Option<String>> {
        let mut entries = BTreeMap::new();
        let mut folders = vec![(root.to_path_buf(), String::new())];
        while let Some((folder, above)) = folders.pop() {
            for entry in fs::read_dir(&folder).unwrap() {
                let entry = entry.unwrap();
                let path = format!("{above}{}", entry.file_name().into_string().unwrap());
                let kind = fs::symlink_metadata(entry.path()).unwrap().file_type();
                if kind.is_dir() {
                    folders.push((entry.path(), format!("{path}/")));
                    entries.insert(path, None);
                } else {
                    assert!(kind.is_file(), "{path} is no file");
                    entries.insert(path, Some(sha256_hex(&fs::read(entry.path()).unwrap())));
                }
            }
        }
        entries
    }

    /// The names at the top of `folder` that the launcher's state or its probes take.
    fn launcher_names(folder: &Path) -> Vec<String> {
        folder_names(folder)
            .unwrap()
            .into_iter()
            .filter(|name| name.starts_with(STATE_DIR))
            .collect()
    }

    /// The icon disc with an empty file, `Gauntlet/PDATA/NONE.WAD`, right after `WAR.WAD`.
    fn disc_with_an_empty_file() -> Vec<u8> {
        const WAR_AT: usize = 0x2000;
        const TOWER_AT: usize = 0x2800;
        let icon = tpl(5, 8, 8);
        let war = [b'A'; 16];
        let table = fst(&[
            FstEntry::folder("Gauntlet", 0, 7),
            FstEntry::folder("PDATA", 1, 5),
            FstEntry::file("WAR.WAD", WAR_AT as u32, war.len() as u32),
            FstEntry::file("NONE.WAD", WAR_AT as u32, 0),
            FstEntry::folder("WDATA", 1, 7),
            FstEntry::file("TOWER.WAD", TOWER_AT as u32, 5),
            FstEntry::folder("carddemo", 0, 9),
            FstEntry::file("icon.tpl", ICON_DATA_AT as u32, icon.len() as u32),
        ]);
        iso_of(
            &table,
            &[
                (WAR_AT, &war[..]),
                (TOWER_AT, b"tower"),
                (ICON_DATA_AT, &icon),
            ],
            SYNTHETIC_ISO_LEN,
        )
    }

    /// Installs the launcher file at `file` on disk, with the icon disc, into `destination`.
    fn install_from_file(inputs: &Inputs, file: &Path, destination: &Path) -> Installed {
        let mut launcher = Sections::of_path(file).unwrap().unwrap();
        let mut image = inputs.image();
        let plan = plan_install(&mut image, &mut launcher, destination, VERSION).unwrap();
        install_with(&plan, &mut image, &mut launcher).unwrap()
    }

    /// Installs as `plan` planned from `image` and `launcher`, with the stand-ins, nothing
    /// recorded and no progress followed.
    fn install_with<R: Read + Seek>(
        plan: &InstallPlan,
        image: &mut GameCubeImage,
        launcher: &mut Sections<R>,
    ) -> Result<Installed, InstallFailure> {
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        install_game_with(
            plan,
            image,
            launcher,
            &mut Unrecorded,
            &job,
            &standing_in(&NoFaults, &answering),
        )
    }

    #[test]
    fn an_install_keeps_what_the_folder_held_and_copies_the_disc_as_it_is() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        fs::create_dir(&destination).unwrap();
        fs::write(destination.join("setup.exe"), b"the player's setup").unwrap();
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let cancel = Cancel::default();
        let told = Told::default();
        let job = Job {
            cancel: &cancel,
            progress: &told,
        };
        let installed = inputs
            .install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap();
        assert!(installed.notices.is_empty(), "{:?}", installed.notices);
        assert_eq!(installed.folder.root(), destination);
        assert_eq!(
            fs::read(destination.join("setup.exe")).unwrap(),
            b"the player's setup"
        );
        let mut war = vec![b'A'; 16];
        war.extend([0; 16]);
        assert_eq!(
            fs::read(destination.join("Gauntlet/PDATA/WAR.WAD")).unwrap(),
            war
        );
        for folder in ["Gauntlet/EMPTY", "carddemo", CONFIG_FOLDER, SAVES_FOLDER] {
            let metadata = fs::symlink_metadata(destination.join(folder)).unwrap();
            assert!(metadata.is_dir(), "{folder}");
        }
        assert_eq!(installed.receipt.disc_revision, 1);
        assert_eq!(installed.receipt.version, VERSION);
        let advances = told.advances();
        let (done, total, item) = advances.last().unwrap().clone();
        assert_eq!((done, item), (total, None));
        // The lock's file stays, free for the next job.
        let state = destination.join(STATE_DIR);
        assert!(
            fs::symlink_metadata(state.join(LOCK_FILE))
                .unwrap()
                .is_file()
        );
        drop(InstallFolder::at(&destination).unwrap().lock().unwrap());
    }

    #[test]
    fn a_ciso_installs_the_same_tree() {
        let top = tempfile::tempdir().unwrap();
        let from_iso = top.path().join("iso");
        let from_ciso = top.path().join("ciso");
        Inputs::of(&icon_iso()).install_into(&from_iso).unwrap();
        Inputs::of(&synthetic_ciso(&icon_iso()))
            .install_into(&from_ciso)
            .unwrap();
        assert_eq!(
            fs::read(from_ciso.join("Gauntlet/WDATA/TOWER.WAD")).unwrap(),
            b"tower"
        );
        assert_eq!(tree(&from_ciso), tree(&from_iso));
    }

    #[test]
    fn an_existing_root_is_a_collision_and_stays_as_it_was() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path();
        for folder in ["Gauntlet", SAVES_FOLDER, CONFIG_FOLDER] {
            fs::create_dir(destination.join(folder)).unwrap();
        }
        fs::write(destination.join("DATA"), b"the player's own").unwrap();
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(destination);
        assert!(!plan.is_ready());
        let before = snapshot_outside(destination, STATE_DIR).unwrap();
        let mut record = Heard::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let failure = inputs
            .install(
                &plan,
                &mut record,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap_err();
        // Names are compared in upper case on every system, so on Linux the game's executable,
        // `gauntlet`, is taken by the disc's folder too.
        let mut taken = vec!["Gauntlet", "config", "data", "saves"];
        if this_platform() == Platform::LinuxX64 {
            taken.insert(3, "gauntlet");
        }
        assert!(
            matches!(
                &failure.error,
                InstallError::Blocked(Blocker::Collision { names }) if *names == taken
            ),
            "{failure:?}"
        );
        assert_eq!(failure.changes, Changes::None);
        assert!(record.calls.is_empty(), "{:?}", record.calls);
        assert_eq!(snapshot_outside(destination, STATE_DIR).unwrap(), before);
        assert!(launcher_names(destination).is_empty());
        assert_eq!(
            fs::read(destination.join("DATA")).unwrap(),
            b"the player's own"
        );
    }

    #[test]
    fn another_launchers_state_folder_is_a_collision_left_as_it_was() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path();
        let state = destination.join(STATE_DIR);
        fs::create_dir(&state).unwrap();
        fs::write(state.join(LOCK_FILE), b"other process").unwrap();
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(destination);
        let before = snapshot(destination).unwrap();
        let mut record = Heard::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let failure = inputs
            .install(
                &plan,
                &mut record,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap_err();
        assert!(
            matches!(
                &failure.error,
                InstallError::Blocked(Blocker::Collision { names }) if *names == [STATE_DIR]
            ),
            "{failure:?}"
        );
        assert_eq!(failure.changes, Changes::None);
        assert!(record.calls.is_empty(), "{:?}", record.calls);
        // Nothing was made, opened or probed there.
        assert_eq!(snapshot(destination).unwrap(), before);
        assert_eq!(fs::read(state.join(LOCK_FILE)).unwrap(), b"other process");
    }

    #[test]
    fn a_state_folder_made_meanwhile_is_a_collision() {
        /// Another launcher, which makes its state folder as this install is recorded.
        fn another_launcher(destination: &Path) {
            let state = destination.join(STATE_DIR);
            fs::create_dir(&state).unwrap();
            fs::write(state.join(LOCK_FILE), b"other process").unwrap();
        }

        let top = tempfile::tempdir().unwrap();
        let destination = top.path();
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(destination);
        assert!(plan.is_ready(), "{plan:?}");
        let mut record = Heard {
            on_pending: Some(another_launcher),
            ..Heard::default()
        };
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let failure = inputs
            .install(
                &plan,
                &mut record,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap_err();
        assert!(
            matches!(
                &failure.error,
                InstallError::Blocked(Blocker::Collision { names }) if *names == [STATE_DIR]
            ),
            "{failure:?}"
        );
        // The install made nothing, and the record forgets the folder again.
        assert_eq!(failure.changes, Changes::None);
        let destination = destination.to_path_buf();
        assert_eq!(
            record.calls,
            [
                ("pending", destination.clone()),
                ("forget", destination.clone())
            ]
        );
        assert_eq!(folder_names(&destination).unwrap(), [STATE_DIR]);
        let state = destination.join(STATE_DIR);
        assert_eq!(folder_names(&state).unwrap(), [LOCK_FILE]);
        assert_eq!(fs::read(state.join(LOCK_FILE)).unwrap(), b"other process");
    }

    #[test]
    fn the_destination_is_judged_again_when_the_job_starts() {
        let inputs = Inputs::of(&icon_iso());
        // A root taken in a folder the plan found missing, and a state folder made in one the
        // plan found empty.
        for (made, taken) in [(SAVES_FOLDER, SAVES_FOLDER), (STATE_DIR, STATE_DIR)] {
            let top = tempfile::tempdir().unwrap();
            let destination = top.path().join("game");
            if made == STATE_DIR {
                fs::create_dir(&destination).unwrap();
            }
            let plan = inputs.plan(&destination);
            assert!(plan.is_ready(), "{plan:?}");
            fs::create_dir_all(destination.join(made)).unwrap();
            let mut record = Heard::default();
            let cancel = Cancel::default();
            let job = Job {
                cancel: &cancel,
                progress: &NoProgress,
            };
            let failure = inputs
                .install(
                    &plan,
                    &mut record,
                    &job,
                    &standing_in(&NoFaults, &answering),
                )
                .unwrap_err();
            assert!(
                matches!(
                    &failure.error,
                    InstallError::Blocked(Blocker::Collision { names }) if *names == [taken]
                ),
                "{failure:?}"
            );
            assert_eq!(failure.changes, Changes::None);
            assert!(record.calls.is_empty(), "{:?}", record.calls);
            assert_eq!(folder_names(&destination).unwrap(), [made]);
        }
    }

    #[test]
    fn the_record_hears_pending_then_installed() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let mut record = Heard::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        inputs
            .install(
                &plan,
                &mut record,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap();
        assert_eq!(
            record.calls,
            [
                ("pending", destination.clone()),
                ("installed", destination.clone())
            ]
        );
    }

    #[test]
    fn the_shell_is_told_of_the_installed_executable_once_it_stands() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let receipt = destination.join(STATE_DIR).join(RECEIPT_FILE);
        let heard = RefCell::new(Vec::new());
        let notify = |executable: &Path| {
            heard
                .borrow_mut()
                .push((executable.to_path_buf(), receipt.is_file()));
        };
        let settings = InstallSettings {
            notify_shell: &notify,
            ..standing_in(&NoFaults, &answering)
        };
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        inputs
            .install(&plan, &mut Unrecorded, &job, &settings)
            .unwrap();
        let executable = destination.join(this_platform().executable());
        assert_eq!(heard.into_inner(), [(executable, true)]);
    }

    #[test]
    fn a_refused_pending_record_makes_nothing() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let mut record = Heard {
            refuse_pending: true,
            ..Heard::default()
        };
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let failure = inputs
            .install(
                &plan,
                &mut record,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap_err();
        assert!(
            matches!(failure.error, InstallError::Record { .. }),
            "{failure:?}"
        );
        assert_eq!(failure.rule(), "record");
        assert_eq!(failure.changes, Changes::None);
        assert_eq!(record.calls, [("pending", destination.clone())]);
        assert!(folder_names(top.path()).unwrap().is_empty());
    }

    #[test]
    fn the_launcher_copy_holds_the_launcher_files_bytes() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let file = inputs.source.path().join("launcher");
        fs::write(&file, &inputs.launcher).unwrap();
        let installed = install_from_file(&inputs, &file, &destination);
        let name = installed_launcher_name(this_platform());
        assert_eq!(fs::read(destination.join(name)).unwrap(), inputs.launcher);
        assert_eq!(fs::read(&file).unwrap(), inputs.launcher);
        assert_eq!(installed.receipt.launcher.name, name);
        assert_eq!(
            installed.receipt.launcher.sha256,
            sha256_hex(&inputs.launcher)
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = fs::metadata(destination.join(name)).unwrap().permissions();
            assert_eq!(mode.mode() & 0o777, 0o755);
        }
    }

    #[cfg(windows)]
    #[test]
    fn a_zone_identifier_does_not_follow_the_launcher_copy() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let file = inputs.source.path().join("launcher.exe");
        fs::write(&file, &inputs.launcher).unwrap();
        let stream = |path: &Path| PathBuf::from(format!("{}:Zone.Identifier", path.display()));
        fs::write(stream(&file), b"[ZoneTransfer]\r\nZoneId=3\r\n").unwrap();
        assert!(fs::metadata(stream(&file)).is_ok());
        install_from_file(&inputs, &file, &destination);
        let copy = destination.join(installed_launcher_name(Platform::WindowsX64));
        assert_eq!(fs::read(&copy).unwrap(), inputs.launcher);
        let refused = fs::metadata(stream(&copy)).unwrap_err();
        assert_eq!(refused.kind(), io::ErrorKind::NotFound, "{refused}");
    }

    #[test]
    fn the_receipt_is_what_the_install_wrote() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let installed = inputs.install_into(&destination).unwrap();
        let platform = this_platform();
        let build_info = sha256_hex(&inputs.runtime.build_info_bytes());
        #[cfg(windows)]
        let icon = {
            let shipped = b"not a real executable";
            let mut marked = shipped.to_vec();
            marked.extend_from_slice(ICON_MARK);
            format!(
                "  \"installedDigests\": {{\n    \"gauntlet.exe\": \"{}\"\n  }},\n  \
                 \"discIcon\": {{\n    \"path\": \"carddemo/icon.tpl\",\n    \"sha256\": \
                 \"{}\",\n    \"releaseExecutableSha256\": \"{}\"\n  }},\n",
                sha256_hex(&marked),
                sha256_hex(&tpl(5, 8, 8)),
                sha256_hex(shipped)
            )
        };
        #[cfg(not(windows))]
        let icon = "  \"installedDigests\": {},\n".to_owned();
        let expected = format!(
            "{{\n  \"schema\": 1,\n  \"minLauncher\": \"1.0.0-beta\",\n  \"version\": \
             \"1.0.0-beta\",\n  \"commit\": \"test-commit\",\n  \"platform\": \"{}\",\n  \
             \"disc\": \"GUNE5D\",\n  \"discRevision\": 1,\n  \"buildInfoSha256\": \
             \"{build_info}\",\n{icon}  \"launcher\": {{\n    \"name\": \"{}\",\n    \
             \"sha256\": \"{}\"\n  }}\n}}\n",
            platform.as_str(),
            installed_launcher_name(platform),
            sha256_hex(&inputs.launcher)
        );
        let receipt = destination.join(STATE_DIR).join(RECEIPT_FILE);
        assert_eq!(fs::read_to_string(&receipt).unwrap(), expected);
        // The receipt the job gives back is the one it wrote, and reads back as the installation.
        let read: Value = serde_json::from_str(&expected).unwrap();
        assert_eq!(serde_json::to_value(&installed.receipt).unwrap(), read);
        let version = Version::parse(VERSION).unwrap();
        let game = installed.folder.installed(&version).unwrap();
        assert_eq!(game.receipt.as_ref(), Some(&installed.receipt));
    }

    #[test]
    fn disc_files_list_the_disc_in_its_order() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        Inputs::of(&icon_iso()).install_into(&destination).unwrap();
        let listed = destination.join(STATE_DIR).join(DISC_FILES);
        assert_eq!(fs::read_to_string(listed).unwrap(), ICON_DISC_FILES);
    }

    #[test]
    fn the_journal_is_gone_and_the_state_folder_holds_what_an_installation_keeps() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        Inputs::of(&icon_iso()).install_into(&destination).unwrap();
        let state = destination.join(STATE_DIR);
        assert_eq!(
            folder_names(&state).unwrap(),
            [DISC_FILES, JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE]
        );
        assert!(folder_names(&state.join(JOURNAL_DIR)).unwrap().is_empty());
        let version = Version::parse(VERSION).unwrap();
        let inspection = InstallFolder::inspect(&destination, &version);
        assert_eq!(inspection.state, FolderState::Installed, "{inspection:?}");
        assert!(inspection.journals.is_empty());
    }

    #[test]
    fn phases_come_in_order_and_progress_reaches_the_total() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let cancel = Cancel::default();
        let told = Told::default();
        let job = Job {
            cancel: &cancel,
            progress: &told,
        };
        inputs
            .install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap();
        let mut phases = vec![CHECK_DISC, PREPARE, GET_RUNTIME, COPY_DISC_FILES];
        #[cfg(windows)]
        phases.push(super::PERSONALISE);
        phases.extend([COPY_LAUNCHER, CHECK_GAME, PUBLISH]);
        let expected: Vec<&str> = phases.iter().map(|phase| phase.0).collect();
        assert_eq!(told.phases(), expected);

        // One total over the sections read, the runtime, the disc and the launcher's copy written,
        // never passed, which the launcher's copy reaches and the end tells again.
        let sections: u64 = inputs
            .sections()
            .sections()
            .iter()
            .map(|section| section.size)
            .sum();
        let total = sections + plan.runtime.bytes + plan.disc.bytes + plan.launcher_bytes;
        let advances = told.advances();
        assert!(
            advances
                .iter()
                .all(|&(done, of, _)| of == total && done <= total),
            "{advances:?}"
        );
        assert!(
            advances.windows(2).all(|pair| pair[0].0 <= pair[1].0),
            "{advances:?}"
        );
        assert_eq!(advances.last(), Some(&(total, total, None)));
        let before_publish = told
            .said()
            .into_iter()
            .take_while(|said| *said != Said::Phase(PUBLISH.0));
        assert!(
            before_publish
                .filter_map(|said| match said {
                    Said::Advance(done, ..) => Some(done),
                    Said::Phase(_) => None,
                })
                .any(|done| done == total),
            "{advances:?}"
        );
        // The disc's files are named as they are copied, in the file table's order.
        let named: Vec<String> = advances
            .iter()
            .filter_map(|(_, _, item)| item.clone())
            .filter(|item| item.starts_with("Gauntlet/") || item.starts_with("carddemo/"))
            .collect();
        let mut once = named.clone();
        once.dedup();
        assert_eq!(
            once,
            [
                "Gauntlet/PDATA/WAR.WAD",
                "Gauntlet/WDATA/TOWER.WAD",
                "carddemo/icon.tpl"
            ]
        );
    }

    #[test]
    fn a_probe_failure_is_a_notice_and_the_install_completes() {
        let inputs = Inputs::of(&icon_iso());
        // A game that answers with another status than 0, and the runtime's executable, which is
        // no program, probed as the launcher probes it.
        let probes: [(&ProbeFn, InstallNotice); 2] = [
            (
                &failing,
                InstallNotice::ProbeFailed {
                    status: Some(3),
                    first_line: Some("no".to_owned()),
                    stderr_tail: "the game is broken".to_owned(),
                },
            ),
            (
                &probe_game_version_with,
                InstallNotice::ProbeFailed {
                    status: None,
                    first_line: None,
                    stderr_tail: String::new(),
                },
            ),
        ];
        for (probe, notice) in probes {
            let top = tempfile::tempdir().unwrap();
            let destination = top.path().join("game");
            let plan = inputs.plan(&destination);
            let cancel = Cancel::default();
            let job = Job {
                cancel: &cancel,
                progress: &NoProgress,
            };
            let started = Instant::now();
            let installed = inputs
                .install(&plan, &mut Unrecorded, &job, &standing_in(&NoFaults, probe))
                .unwrap();
            assert!(started.elapsed() < PROBE_FAILS_WITHIN);
            assert_eq!(installed.notices, [notice]);
            let receipt = destination.join(STATE_DIR).join(RECEIPT_FILE);
            assert!(fs::symlink_metadata(receipt).unwrap().is_file());
        }
    }

    /// What [`Watch`] finds as a point is passed: the point, what the journal holds then, and the
    /// names in the state folder.
    type Passed = (FaultPoint, Option<Value>, Vec<String>);

    /// Fault points that look at the install's folder as each is passed.
    struct Watch {
        state: PathBuf,
        seen: RefCell<Vec<Passed>>,
    }

    impl FaultPoints for Watch {
        fn pass(&self, point: FaultPoint) -> io::Result<()> {
            let journal = self.state.join(JOURNAL_DIR).join(INSTALL_JOURNAL);
            let journal = fs::read(journal)
                .ok()
                .map(|bytes| serde_json::from_slice(&bytes).unwrap());
            let names = folder_names(&self.state).unwrap();
            self.seen.borrow_mut().push((point, journal, names));
            Ok(())
        }
    }

    #[test]
    fn each_step_is_passed_once_it_is_made_and_recorded() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let watch = Watch {
            state: destination.join(STATE_DIR),
            seen: RefCell::new(Vec::new()),
        };
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        inputs
            .install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&watch, &answering),
            )
            .unwrap();
        let seen = watch.seen.into_inner();
        let roots: Vec<&str> = plan.roots.iter().map(RelPath::as_str).collect();
        let staged = if cfg!(windows) { 5 } else { 4 };
        let mut points = vec![FaultPoint::Prepare; 3];
        points.extend(vec![FaultPoint::Stage; staged]);
        points.extend(vec![FaultPoint::Publish; roots.len() + 3]);
        points.extend([FaultPoint::Cleanup; 2]);
        assert_eq!(
            seen.iter().map(|(point, ..)| *point).collect::<Vec<_>>(),
            points
        );

        // The state folder made, then the lock, then the journal naming the staging.
        assert_eq!(seen[0].1, None);
        assert!(seen[0].2.is_empty(), "{:?}", seen[0].2);
        assert_eq!(seen[1].2, [LOCK_FILE]);
        let (_, journal, names) = &seen[2];
        let journal = journal.as_ref().unwrap();
        assert_eq!(journal["phase"], "staging");
        assert_eq!(journal["madeFolders"], 1);
        let staging = journal["staging"].as_str().unwrap();
        assert!(
            staging.starts_with(super::INSTALL_STAGING_PREFIX),
            "{staging}"
        );
        assert_eq!(*names, [staging, JOURNAL_DIR, LOCK_FILE]);
        // Staging under that journal, the disc's list written once the disc is copied.
        for (at, (_, journal, names)) in seen[3..3 + staged].iter().enumerate() {
            assert_eq!(journal.as_ref().unwrap()["phase"], "staging");
            assert_eq!(names.contains(&DISC_FILES.to_owned()), at >= 1, "{names:?}");
        }
        // The publish: every root and the receipt in the journal before anything moves, then each
        // root recorded once it has moved, in byte order, and the receipt written last.
        let publish = &seen[3 + staged..3 + staged + roots.len() + 3];
        for (moved, (_, journal, names)) in publish.iter().enumerate() {
            let journal = journal.as_ref().unwrap();
            assert_eq!(journal["phase"], "publishing");
            assert_eq!(journal["roots"], json!(roots));
            let receipt = &journal["receipt"];
            assert_eq!(receipt["launcher"]["sha256"], sha256_hex(&inputs.launcher));
            let recorded = moved.saturating_sub(1).min(roots.len());
            assert_eq!(journal["published"], json!(roots[..recorded]), "{moved}");
            if (1..=roots.len()).contains(&moved) {
                assert!(destination.join(roots[moved - 1]).exists(), "{moved}");
            }
            let receipted = moved == roots.len() + 2;
            assert_eq!(
                names.contains(&RECEIPT_FILE.to_owned()),
                receipted,
                "{moved}"
            );
        }
        // Clean-up: committed with the staging there, then the staging gone and the journal left.
        let cleanup = &seen[seen.len() - 2..];
        assert_eq!(cleanup[0].1.as_ref().unwrap()["phase"], "committed");
        assert!(cleanup[0].2.contains(&staging.to_owned()));
        assert_eq!(cleanup[1].1.as_ref().unwrap()["phase"], "committed");
        assert!(!cleanup[1].2.contains(&staging.to_owned()));
    }

    /// Fault points that remove the staging the journal names as the clean-up begins, as a
    /// clean-up a crash cut short after the staging went leaves the folder.
    struct StagingGone {
        state: PathBuf,
    }

    impl FaultPoints for StagingGone {
        fn pass(&self, point: FaultPoint) -> io::Result<()> {
            if point == FaultPoint::Cleanup {
                let journal = self.state.join(JOURNAL_DIR).join(INSTALL_JOURNAL);
                let journal: Value = serde_json::from_slice(&fs::read(journal)?)?;
                let staging = self.state.join(journal["staging"].as_str().unwrap());
                if staging.exists() {
                    fs::remove_dir_all(staging)?;
                }
            }
            Ok(())
        }
    }

    #[test]
    fn a_staging_already_gone_counts_as_removed() {
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let gone = StagingGone {
            state: destination.join(STATE_DIR),
        };
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let installed = inputs
            .install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&gone, &answering),
            )
            .unwrap();
        assert!(installed.notices.is_empty(), "{:?}", installed.notices);
        let state = destination.join(STATE_DIR);
        assert_eq!(
            folder_names(&state).unwrap(),
            [DISC_FILES, JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE]
        );
        assert!(folder_names(&state.join(JOURNAL_DIR)).unwrap().is_empty());
    }

    #[test]
    fn an_empty_disc_file_is_made_told_and_a_cancel_is_heeded_before_it() {
        const EMPTY: &str = "Gauntlet/PDATA/NONE.WAD";
        const BEFORE_IT: &str = "Gauntlet/PDATA/WAR.WAD";
        let inputs = Inputs::of(&disc_with_an_empty_file());

        // Made as an empty file, listed with the digest of no bytes, and told once, adding nothing
        // to what was done.
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let plan = inputs.plan(&destination);
        let cancel = Cancel::default();
        let told = Told::default();
        let job = Job {
            cancel: &cancel,
            progress: &told,
        };
        inputs
            .install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap();
        assert_eq!(fs::metadata(destination.join(EMPTY)).unwrap().len(), 0);
        let listed: Value = serde_json::from_slice(
            &fs::read(destination.join(STATE_DIR).join(DISC_FILES)).unwrap(),
        )
        .unwrap();
        assert_eq!(
            listed["files"][1],
            json!({"path": EMPTY, "size": 0, "sha256": EMPTY_SHA256})
        );
        let advances = told.advances();
        let at = advances
            .iter()
            .position(|(_, _, item)| item.as_deref() == Some(EMPTY))
            .unwrap();
        assert_eq!(advances[at - 1].2.as_deref(), Some(BEFORE_IT));
        assert_eq!(advances[at].0, advances[at - 1].0);
        let empty_told = advances
            .iter()
            .filter(|(_, _, item)| item.as_deref() == Some(EMPTY))
            .count();
        assert_eq!(empty_told, 1);

        // A cancel asked as the file before it is copied stops the install before it.
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let plan = inputs.plan(&destination);
        let cancel = Cancel::default();
        let told = Told {
            cancel_at: Some((cancel.clone(), BEFORE_IT.to_owned())),
            ..Told::default()
        };
        let job = Job {
            cancel: &cancel,
            progress: &told,
        };
        let failure = inputs
            .install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap_err();
        assert!(
            matches!(failure.error, InstallError::Cancelled),
            "{failure:?}"
        );
        // What the install made is left for the next start, which finds its journal.
        assert_eq!(failure.changes, Changes::Kept);
        let said = told.said();
        assert_eq!(
            said.last(),
            Some(&Said::Advance(
                advances[at - 1].0,
                advances[at - 1].1,
                Some(BEFORE_IT.to_owned())
            ))
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_linux_install_does_not_read_the_icon() {
        // The installer tests' disc, whose icon is four bytes of text, which no Linux install
        // decodes.
        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        Inputs::of(&synthetic_iso())
            .install_into(&destination)
            .unwrap();
        assert_eq!(
            fs::read(destination.join("carddemo/icon.tpl")).unwrap(),
            b"icon"
        );
        let listed: Value = serde_json::from_slice(
            &fs::read(destination.join(STATE_DIR).join(DISC_FILES)).unwrap(),
        )
        .unwrap();
        assert_eq!(
            listed["files"][2],
            json!({
                "path": "carddemo/icon.tpl",
                "size": 4,
                "sha256": "c2d4b446a44ce54fab8e01150e24dd24f3d850c7c14dcfe31f6321341dd86874",
            })
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_linux_install_keeps_the_shipped_executable() {
        use std::os::unix::fs::PermissionsExt;

        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let installed = inputs.install_into(&destination).unwrap();
        assert_eq!(
            fs::read(destination.join("gauntlet")).unwrap(),
            b"not a real executable"
        );
        assert_eq!(installed.receipt.disc_icon, None);
        assert!(installed.receipt.installed_digests.is_empty());
        let mode = |name: &str| {
            fs::metadata(destination.join(name))
                .unwrap()
                .permissions()
                .mode()
                & 0o777
        };
        assert_eq!(mode("gauntlet"), 0o755);
        for name in [
            "VERSION",
            "build-info.json",
            "data/config.json",
            "shaders/test.spv",
        ] {
            assert_eq!(mode(name), 0o644, "{name}");
        }
    }

    #[cfg(windows)]
    #[test]
    fn the_system_is_kept_awake_while_installing() {
        use std::sync::atomic::{AtomicBool, Ordering};

        use windows_sys::Win32::System::Power::{ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

        use crate::platform;

        /// A progress that reads the thread's requests at the first advance of the disc's copy.
        #[derive(Default)]
        struct Awake {
            copying: AtomicBool,
            state: Mutex<Option<u32>>,
        }

        impl Progress for Awake {
            fn phase(&self, phase: PhaseId) {
                self.copying
                    .store(phase == COPY_DISC_FILES, Ordering::SeqCst);
            }

            fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {
                let mut state = self.state.lock().unwrap();
                if self.copying.load(Ordering::SeqCst) && state.is_none() {
                    *state = Some(platform::execution_state());
                }
            }
        }

        let top = tempfile::tempdir().unwrap();
        let destination = top.path().join("game");
        let inputs = Inputs::of(&icon_iso());
        let plan = inputs.plan(&destination);
        let before = platform::execution_state();
        let awake = Awake::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &awake,
        };
        inputs
            .install(
                &plan,
                &mut Unrecorded,
                &job,
                &standing_in(&NoFaults, &answering),
            )
            .unwrap();
        assert_eq!(
            *awake.state.lock().unwrap(),
            Some(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
        );
        assert_eq!(platform::execution_state(), before);
    }
}
