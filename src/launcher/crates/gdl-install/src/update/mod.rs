//! Updating an installation to a newer release: the game's runtime and the launcher copy at the
//! installation's top together, from a release the feed found or from the launcher file that runs.
//! An update is planned first, without the lock and without changing anything: what it would take
//! the new release from, which runtime names it would move, and the room it would need.

mod names;

use std::collections::BTreeSet;
use std::fs::File;
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::RelPath;
use gdl_release::{Platform, Release, Version};

pub use names::FileChanges;

use self::names::judge_names;
use crate::compat::Access;
use crate::folder::{FolderError, InstallFolder};
use crate::inspect::FolderState;
use crate::install::SPACE_MARGIN;
use crate::payload::PayloadError;
use crate::receipt::ReceiptError;
use crate::sections::Sections;

/// What an update would do, read without a lock and without changing anything.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UpdatePlan {
    /// The installation's folder, absolute and folded by name.
    pub root: PathBuf,
    /// The installed release's version.
    pub from: Version,
    /// The version the source would bring. One that is not newer is planned too, since another
    /// launcher may have updated the folder after the source was chosen.
    pub to: Version,
    /// Where the new release would come from.
    pub source: SourceSummary,
    /// Which runtime names the update would move; `None` for a release, whose runtime is inside
    /// the launcher file it downloads.
    pub changes: Option<FileChanges>,
    /// The room the update would need on the volume of the launcher's state: the files it stages,
    /// the launcher copy and [`SPACE_MARGIN`]; `None` for a release, which is measured once
    /// downloaded.
    pub needed_bytes: Option<u64>,
    /// Whether a job of this launcher's was left unfinished in the folder, which the update would
    /// finish or take back first.
    pub recovery_pending: bool,
    /// Whether an earlier update's cleanup is still to be finished.
    pub cleanup_pending: bool,
}

/// Where an update would take the new release from, as a plan tells it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SourceSummary {
    /// The launcher file that runs, which carries the release and is copied whole as the new
    /// launcher copy.
    Bundled {
        /// How many bytes the launcher file holds.
        file_len: u64,
    },
    /// A release of the feed's, whose launcher file is downloaded.
    Release {
        /// The release's tag.
        tag: String,
        /// How many bytes its launcher file holds.
        download_bytes: u64,
    },
}

/// Where an update is planned to take the new release from.
pub enum PlanSource<'a> {
    /// The launcher file that runs, through the handle it opened as it started.
    Bundled(&'a mut Sections<File>),
    /// A release the feed found.
    Release(&'a Release),
}

/// Plans the update of the installation at `root` by the launcher of version `running` from
/// `source`, reading the folder without its lock and changing nothing.
///
/// The installation is read through its receipt as [`InstallFolder::installed`] reads it, and a
/// refusal is [`UpdateError::Receipt`]. A receipt or a journal that a later launcher stamped is
/// [`UpdateError::NewerLauncher`], and a launcher older than the installed release
/// [`UpdateError::OlderLauncher`]: neither may change the installation. A job left unfinished, as
/// [`InstallFolder::inspect`] finds one, is planned as pending.
///
/// From the launcher file that runs, its runtime is read no further than its release record and
/// held to the one this launcher installs, of its platform and version, and its names are judged
/// against the installed release's as an update moves them; on Windows an installation whose
/// receipt records no disc icon has its executable staged whatever its bytes, so that the update
/// sets the icon. From a release, nothing of the runtime is known before its download.
pub fn plan_update(
    root: &Path,
    running: &Version,
    source: PlanSource<'_>,
) -> Result<UpdatePlan, UpdateError> {
    let folder = InstallFolder::at(root).map_err(folder_error)?;
    let installed = folder.installed(running).map_err(UpdateError::Receipt)?;
    let (Access::Writable, Some(receipt)) = (installed.access, installed.receipt.as_ref()) else {
        return Err(UpdateError::NewerLauncher);
    };
    let from = installed.build_info.version();
    if *running < from {
        return Err(UpdateError::OlderLauncher {
            running: running.to_string().into(),
            installed: from.to_string().into(),
        });
    }
    let recovery_pending = match InstallFolder::inspect(folder.root(), running).state {
        FolderState::Pending => true,
        FolderState::NewerLauncher => return Err(UpdateError::NewerLauncher),
        _ => false,
    };
    let (to, source, changes, needed_bytes) = match source {
        PlanSource::Bundled(launcher) => {
            let file_len = launcher.file_len();
            let payload = launcher.runtime().map_err(UpdateError::Runtime)?;
            payload
                .for_launcher(&running.to_string())
                .map_err(UpdateError::Runtime)?;
            let stage_executable = installed.build_info.platform == Platform::WindowsX64
                && receipt.disc_icon.is_none();
            let changes = judge_names(
                folder.root(),
                &installed.build_info,
                payload.build_info(),
                stage_executable,
            )?;
            let staged: BTreeSet<&RelPath> =
                changes.replaced.iter().chain(&changes.added).collect();
            let staged_bytes: u64 = payload
                .entries()
                .iter()
                .filter(|entry| staged.contains(&entry.name))
                .map(|entry| entry.size)
                .sum();
            let needed = staged_bytes
                .saturating_add(file_len)
                .saturating_add(SPACE_MARGIN);
            (
                payload.build_info().version(),
                SourceSummary::Bundled { file_len },
                Some(changes),
                Some(needed),
            )
        }
        PlanSource::Release(release) => (
            release.version.clone(),
            SourceSummary::Release {
                tag: release.tag.clone(),
                download_bytes: release.launcher.size,
            },
            None,
            None,
        ),
    };
    Ok(UpdatePlan {
        root: folder.root().to_path_buf(),
        from,
        to,
        source,
        changes,
        needed_bytes,
        recovery_pending,
        cleanup_pending: false,
    })
}

/// The update's refusal of a folder that cannot hold an installation: a link on its path is
/// refused as one, and nothing there, or something other than a folder, is the system's answer.
fn folder_error(error: FolderError) -> UpdateError {
    match error {
        FolderError::Links { path } => UpdateError::Links { path },
        FolderError::Absent { path } => UpdateError::Io {
            path,
            source: io::Error::from(io::ErrorKind::NotFound),
        },
        FolderError::NotDirectory { path } => UpdateError::Io {
            path,
            source: io::Error::from(io::ErrorKind::NotADirectory),
        },
        FolderError::Io { path, source } => UpdateError::Io { path, source },
    }
}

/// Why an update could not be planned, as technical detail for logs. What a player reads is chosen
/// from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum UpdateError {
    /// The installation cannot be read through its receipt.
    #[error(transparent)]
    Receipt(ReceiptError),
    /// A later launcher's receipt or journal stands in the installation.
    #[error("A later launcher's state stands in this installation; this launcher changes nothing")]
    NewerLauncher,
    /// The launcher file that runs is older than the installed release.
    #[error("This launcher file ({running}) is older than the installation ({installed})")]
    OlderLauncher {
        /// The running launcher's version.
        running: Box<str>,
        /// The installed release's version.
        installed: Box<str>,
    },
    /// The new runtime is refused, or is not the one this launcher brings.
    #[error(transparent)]
    Runtime(PayloadError),
    /// The installed release record names a file that no runtime may hold.
    #[error("The installed release record names a file no runtime holds: {name}")]
    InstalledNames {
        /// The name.
        name: Box<str>,
    },
    /// A new name equals an installed one in ASCII letter case alone.
    #[error("Update changes a managed path's letter case: {old} -> {new}")]
    CaseChange {
        /// The installed name.
        old: Box<str>,
        /// The new name.
        new: Box<str>,
    },
    /// Something the launcher did not put there is at a name the new release adds, or at a folder
    /// above it.
    #[error("Update would overwrite an unowned file: {name}")]
    Unowned {
        /// The new name.
        name: Box<str>,
    },
    /// `path` is a link: the installation's folder, a folder above it, or a runtime name or a
    /// folder on the way to one.
    #[error("Choose a directory without links or junctions: {}", .path.display())]
    Links {
        /// The link.
        path: PathBuf,
    },
    /// The system refused to look at `path`, or nothing is there to hold an installation.
    #[error("{}: {source}", .path.display())]
    Io {
        /// What was looked at.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
}

impl UpdateError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the receipt's or the
    /// runtime's rule, `newerLauncher`, `olderLauncher`, `installedNames`, `caseChange`,
    /// `unowned`, `links` or `io`.
    pub fn rule(&self) -> &'static str {
        match self {
            UpdateError::Receipt(error) => error.rule(),
            UpdateError::NewerLauncher => "newerLauncher",
            UpdateError::OlderLauncher { .. } => "olderLauncher",
            UpdateError::Runtime(error) => error.rule(),
            UpdateError::InstalledNames { .. } => "installedNames",
            UpdateError::CaseChange { .. } => "caseChange",
            UpdateError::Unowned { .. } => "unowned",
            UpdateError::Links { .. } => "links",
            UpdateError::Io { .. } => "io",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::fs::{self, File};
    use std::io;
    use std::path::{Path, PathBuf};

    use gdl_files::{PAYLOAD_NAMES, RelPath, retry_locked};
    #[cfg(windows)]
    use gdl_fixtures::fake_sign;
    use gdl_fixtures::{
        InstallFixture, RuntimeFiles, append_sections, pretty_json, sha256_hex, snapshot, tiny_elf,
        tiny_pe,
    };
    use gdl_media::Sha256Digest;
    use gdl_release::{Asset, Release, Version, checksums_asset, launcher_asset};
    use serde_json::json;
    use tempfile::TempDir;

    use super::{FileChanges, PlanSource, SourceSummary, UpdateError, UpdatePlan, plan_update};
    use crate::folder::{JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE, STATE_DIR};
    use crate::install::{INSTALL_JOURNAL, SPACE_MARGIN};
    use crate::payload::{BUILD_INFO, PayloadError};
    use crate::receipt::ReceiptError;
    use crate::sections::{RUNTIME_SECTION, Sections, SectionsError};
    use crate::test_platform::this_platform;

    /// The version the tests' installations are of.
    const INSTALLED: &str = "1.0.0-beta";

    /// The version of the release the tests update them to.
    const NEXT: &str = "1.0.0-beta.1";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// An installation of the standard runtime of `version` for the platform this test program was
    /// built for, as the launcher leaves one, in a folder of the test's own.
    fn installation_of(version: &str) -> (TempDir, InstallFixture) {
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(this_platform().as_str(), version);
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        (dir, fixture)
    }

    /// A launcher file of this system's kind carrying `runtime`, written into `dir` outside the
    /// installation, signed on Windows as a release is, with its sections found as the launcher
    /// finds its own.
    fn bundled(dir: &Path, runtime: &RuntimeFiles) -> Sections<File> {
        let mut file = if cfg!(windows) { tiny_pe() } else { tiny_elf() };
        append_sections(&mut file, &[(RUNTIME_SECTION, &runtime.zip())], 0);
        #[cfg(windows)]
        fake_sign(&mut file);
        let path = dir.join("launcher");
        fs::write(&path, file).unwrap();
        Sections::of_path(&path).unwrap().unwrap()
    }

    /// `texts`, each as a name.
    fn names(texts: &[&str]) -> Vec<RelPath> {
        texts
            .iter()
            .map(|text| RelPath::parse(text, &PAYLOAD_NAMES).unwrap())
            .collect()
    }

    /// How many bytes the files `texts` of `runtime` hold, its release record among them.
    fn bytes_of(runtime: &RuntimeFiles, texts: &[&str]) -> u64 {
        let record = runtime.build_info_bytes();
        texts
            .iter()
            .map(|text| match *text {
                BUILD_INFO => record.len(),
                _ => {
                    let (_, data) = runtime.files.iter().find(|(name, _)| name == text).unwrap();
                    data.len()
                }
            })
            .sum::<usize>() as u64
    }

    #[test]
    fn a_plan_names_what_changes() {
        let (dir, fixture) = installation_of(INSTALLED);
        let next = fixture.runtime.next(NEXT);
        let mut launcher = bundled(dir.path(), &next);
        let file_len = launcher.file_len();
        let before = snapshot(dir.path()).unwrap();

        let plan = plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        let publish = [
            "VERSION",
            "build-info.json",
            "data/text/en.json",
            "shaders/next.spv",
        ];
        assert_eq!(
            plan,
            UpdatePlan {
                root: fixture.root.clone(),
                from: version(INSTALLED),
                to: version(NEXT),
                source: SourceSummary::Bundled { file_len },
                changes: Some(FileChanges {
                    replaced: names(&["VERSION", "build-info.json", "data/text/en.json"]),
                    added: names(&["shaders/next.spv"]),
                    removed: names(&["shaders/test.spv"]),
                    unchanged: 3,
                }),
                needed_bytes: Some(bytes_of(&next, &publish) + file_len + SPACE_MARGIN),
                recovery_pending: false,
                cleanup_pending: false,
            }
        );
        // Nothing was made, changed or removed, in the installation or beside it.
        assert_eq!(snapshot(dir.path()).unwrap(), before);
        assert!(!fixture.root.join(STATE_DIR).join(LOCK_FILE).exists());
    }

    #[test]
    fn a_source_no_newer_than_the_installation_is_still_planned() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime);
        let plan = plan_update(
            &fixture.root,
            &version(INSTALLED),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        assert_eq!(plan.from, plan.to);
        let changes = plan.changes.unwrap();
        assert_eq!(changes.replaced, names(&[BUILD_INFO]));
        assert_eq!(changes.unchanged, fixture.runtime.files.len());
    }

    #[test]
    fn a_release_plans_its_download() {
        let (_dir, fixture) = installation_of(INSTALLED);
        let to = version(NEXT);
        let platform = this_platform();
        let asset = |name: String, size: u64| Asset {
            url: format!("https://github.com/example/releases/download/v{to}/{name}"),
            name,
            size,
            sha256: Sha256Digest::of(b"launcher"),
        };
        let release = Release {
            version: to.clone(),
            tag: format!("v{to}"),
            platform,
            launcher: asset(launcher_asset(&to, platform), 7_340_032),
            checksums: asset(checksums_asset(&to, platform), 300),
            signature: None,
            notes: None,
        };
        let plan = plan_update(
            &fixture.root,
            &version(INSTALLED),
            PlanSource::Release(&release),
        )
        .unwrap();
        assert_eq!(
            plan,
            UpdatePlan {
                root: fixture.root.clone(),
                from: version(INSTALLED),
                to,
                source: SourceSummary::Release {
                    tag: release.tag.clone(),
                    download_bytes: 7_340_032,
                },
                changes: None,
                needed_bytes: None,
                recovery_pending: false,
                cleanup_pending: false,
            }
        );
    }

    #[test]
    fn an_older_launcher_plans_nothing() {
        let (dir, fixture) = installation_of(NEXT);
        let mut launcher = bundled(
            dir.path(),
            &RuntimeFiles::standard(this_platform().as_str(), INSTALLED),
        );
        let before = snapshot(dir.path()).unwrap();
        match plan_update(
            &fixture.root,
            &version(INSTALLED),
            PlanSource::Bundled(&mut launcher),
        ) {
            Err(UpdateError::OlderLauncher { running, installed }) => {
                assert_eq!((&*running, &*installed), (INSTALLED, NEXT));
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn a_later_launchers_installation_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let running = version(NEXT);

        // A receipt a later launcher stamped.
        let mut receipt = fixture.receipt();
        receipt["minLauncher"] = json!("9.0.0");
        fixture.write_receipt(&receipt).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let plan = plan_update(&fixture.root, &running, PlanSource::Bundled(&mut launcher));
        assert!(matches!(plan, Err(UpdateError::NewerLauncher)), "{plan:?}");
        assert_eq!(snapshot(dir.path()).unwrap(), before);

        // A journal a later launcher stamped, beside a receipt this one may change.
        fixture.write_receipt(&fixture.receipt()).unwrap();
        let journals = fixture.root.join(STATE_DIR).join(JOURNAL_DIR);
        fs::create_dir(&journals).unwrap();
        let journal = json!({"schema": 2, "minLauncher": INSTALLED});
        fs::write(journals.join(INSTALL_JOURNAL), journal.to_string()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let plan = plan_update(&fixture.root, &running, PlanSource::Bundled(&mut launcher));
        assert!(matches!(plan, Err(UpdateError::NewerLauncher)), "{plan:?}");
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn a_job_left_unfinished_is_planned_as_pending() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let journals = fixture.root.join(STATE_DIR).join(JOURNAL_DIR);
        fs::create_dir(&journals).unwrap();
        let journal = json!({"schema": 1, "minLauncher": INSTALLED});
        fs::write(journals.join(INSTALL_JOURNAL), journal.to_string()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let plan = plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        assert!(plan.recovery_pending);
        assert!(!plan.cleanup_pending);
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn a_runtime_that_is_not_this_launchers_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        match plan_update(
            &fixture.root,
            &version("1.0.0-beta.2"),
            PlanSource::Bundled(&mut launcher),
        ) {
            Err(UpdateError::Runtime(PayloadError::OtherVersion { found, expected })) => {
                assert_eq!((&*found, &*expected), (NEXT, "1.0.0-beta.2"));
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn an_installed_record_that_claims_a_players_file_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let save = fixture.root.join("saves").join("hero.json");
        fs::write(&save, b"user-owned").unwrap();
        // The record and the receipt agree with each other, but the record claims the save.
        let mut claiming = fixture.runtime.build_info();
        claiming["files"]["saves/hero.json"] = json!(sha256_hex(b"user-owned"));
        let record = pretty_json(&claiming);
        fs::write(fixture.root.join(BUILD_INFO), &record).unwrap();
        let mut receipt = fixture.receipt();
        receipt["buildInfoSha256"] = json!(sha256_hex(&record));
        fixture.write_receipt(&receipt).unwrap();
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let before = snapshot(dir.path()).unwrap();
        match plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        ) {
            Err(UpdateError::Receipt(ReceiptError::Disagrees { field })) => {
                assert_eq!(field, BUILD_INFO);
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(snapshot(dir.path()).unwrap(), before);
        assert_eq!(fs::read(&save).unwrap(), b"user-owned");
    }

    #[test]
    fn a_folder_that_holds_no_installation_plans_nothing() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let running = version(NEXT);
        let missing = dir.path().join("missing");
        match plan_update(&missing, &running, PlanSource::Bundled(&mut launcher)) {
            Err(UpdateError::Io { path, source }) => {
                assert_eq!(path, missing);
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("{other:?}"),
        }
        let receipt = fixture.root.join(STATE_DIR).join(RECEIPT_FILE);
        retry_locked(|| fs::remove_file(&receipt)).unwrap();
        match plan_update(&fixture.root, &running, PlanSource::Bundled(&mut launcher)) {
            Err(UpdateError::Receipt(ReceiptError::Unreadable { .. })) => {}
            other => panic!("{other:?}"),
        }
    }

    #[cfg(windows)]
    #[test]
    fn a_windows_receipt_without_its_icon_stages_the_executable() {
        let (dir, fixture) = installation_of(INSTALLED);
        let mut receipt = fixture.receipt();
        receipt.as_object_mut().unwrap().remove("discIcon");
        fixture.write_receipt(&receipt).unwrap();
        let mut launcher = bundled(dir.path(), &fixture.runtime.next(NEXT));
        let plan = plan_update(
            &fixture.root,
            &version(NEXT),
            PlanSource::Bundled(&mut launcher),
        )
        .unwrap();
        let changes = plan.changes.unwrap();
        assert_eq!(
            changes.replaced,
            names(&[
                "VERSION",
                "build-info.json",
                "data/text/en.json",
                "gauntlet.exe"
            ])
        );
        assert_eq!(changes.unchanged, 2);
    }

    #[test]
    fn update_errors_name_their_rules_and_stay_small() {
        let path = PathBuf::from("install");
        let cases = [
            (
                UpdateError::Receipt(ReceiptError::Disagrees { field: "version" }),
                "disagrees",
                "Installation receipts disagree; choose a complete installed game (version)"
                    .to_owned(),
            ),
            (
                UpdateError::NewerLauncher,
                "newerLauncher",
                "A later launcher's state stands in this installation; this launcher changes \
                 nothing"
                    .to_owned(),
            ),
            (
                UpdateError::OlderLauncher {
                    running: INSTALLED.into(),
                    installed: NEXT.into(),
                },
                "olderLauncher",
                format!("This launcher file ({INSTALLED}) is older than the installation ({NEXT})"),
            ),
            (
                UpdateError::Runtime(PayloadError::Sections(SectionsError::NoSections)),
                "noSections",
                SectionsError::NoSections.to_string(),
            ),
            (
                UpdateError::InstalledNames {
                    name: "saves/hero.json".into(),
                },
                "installedNames",
                "The installed release record names a file no runtime holds: saves/hero.json"
                    .to_owned(),
            ),
            (
                UpdateError::CaseChange {
                    old: "data/text/en.json".into(),
                    new: "data/Text/en.json".into(),
                },
                "caseChange",
                "Update changes a managed path's letter case: data/text/en.json -> \
                 data/Text/en.json"
                    .to_owned(),
            ),
            (
                UpdateError::Unowned {
                    name: "shaders/new.spv".into(),
                },
                "unowned",
                "Update would overwrite an unowned file: shaders/new.spv".to_owned(),
            ),
            (
                UpdateError::Links { path: path.clone() },
                "links",
                format!(
                    "Choose a directory without links or junctions: {}",
                    path.display()
                ),
            ),
            (
                UpdateError::Io {
                    path: path.clone(),
                    source: io::Error::other("refused"),
                },
                "io",
                format!("{}: refused", path.display()),
            ),
        ];
        for (error, rule, text) in cases {
            assert_eq!(error.rule(), rule, "{error:?}");
            assert_eq!(error.to_string(), text, "{error:?}");
        }
        assert!(size_of::<UpdateError>() <= MOST_ERROR_BYTES);
    }
}
