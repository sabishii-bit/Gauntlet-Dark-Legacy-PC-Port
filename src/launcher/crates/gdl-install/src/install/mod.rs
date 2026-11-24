//! A fresh install of the game into a folder of the player's: the `Gauntlet` and `carddemo` trees
//! of the player's GameCube disc, the runtime the launcher file carries, a copy of the launcher
//! file and the two folders the game keeps the player's saves and settings in. An install is
//! planned before anything is written: the plan reads the disc's image, judged whole when it was
//! opened, the runtime's central directory and release record, and the folder, and lists what
//! refuses an install there, so that a disc, a runtime or a folder an install would fail on is
//! refused before a file is made.

use std::collections::BTreeSet;
use std::io::{Read, Seek};
use std::path::{Path, PathBuf};

use gdl_files::{DISC_NAMES, NameRules, PAYLOAD_NAMES, RelPath};
use gdl_media::{Container, DiscEntryKind, GameCubeImage, MediaError};
#[cfg(windows)]
use gdl_media::{MAX_TPL_LEN, first_frame};
use gdl_release::Platform;

use crate::destination::{
    Blocker, DestinationError, DestinationReport, FolderChecks, Warning, judge_with,
};
use crate::folder::STATE_DIR;
use crate::host::{Unsupported, host_supported};
use crate::icon::IconError;
#[cfg(windows)]
use crate::icon::{DISC_ICON, icon_resources};
use crate::payload::{LIB_FOLDER, PayloadError, ROOT_FILES, ROOT_FOLDERS};
use crate::receipt::installed_launcher_name;
use crate::sections::Sections;

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
}

impl InstallError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the finding's that
    /// refuses the install, or the rule the inner error gives.
    pub fn rule(&self) -> &'static str {
        match self {
            InstallError::Blocked(blocker) => blocker.rule(),
            InstallError::Destination(error) => error.rule(),
            InstallError::Image(error) => error.rule(),
            InstallError::Icon(error) => error.rule(),
            InstallError::Runtime(error) => error.rule(),
        }
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
    let icon = image
        .entries()
        .iter()
        .find(|entry| entry.path.as_str().eq_ignore_ascii_case(DISC_ICON))
        .cloned();
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
    use std::fs;
    use std::io::{self, Cursor};
    use std::path::{Path, PathBuf};

    use gdl_files::{FilesError, FolderProbe, PAYLOAD_NAMES, ProbedFolder, RelPath, Volume};
    #[cfg(windows)]
    use gdl_fixtures::fake_sign;
    use gdl_fixtures::{
        RuntimeFiles, append_sections, folder_names, icon_iso, synthetic_ciso, synthetic_iso,
        tiny_elf, tiny_pe,
    };
    use gdl_media::{Container, GameCubeImage, MediaError};
    use gdl_release::Platform;

    use super::{
        CONFIG_FOLDER, DiscSummary, InstallError, InstallPlan, RuntimeSummary, SAVES_FOLDER,
        SPACE_MARGIN, install_names, plan_install, plan_with,
    };
    use crate::destination::{Blocker, DestinationError, FolderChecks};
    use crate::folder::STATE_DIR;
    use crate::host::Unsupported;
    use crate::icon::IconError;
    use crate::payload::{LIB_FOLDER, PayloadError, ROOT_FOLDERS};
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
        ];
        for (error, rule, text) in cases {
            assert_eq!(error.rule(), rule, "{error:?}");
            assert_eq!(error.to_string(), text, "{error:?}");
        }
        assert!(size_of::<InstallError>() <= MOST_ERROR_BYTES);
    }
}
