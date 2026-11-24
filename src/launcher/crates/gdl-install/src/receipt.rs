//! An installation's receipt, `.gdl-launcher/receipt.json`: what the launcher installed in the
//! folder, the release, the disc the game's data came from and the launcher copy it left at the
//! top, with the digests that pin every file it wrote; and the reading of an installation through
//! it. A receipt that a later launcher wrote is read for its stamp alone: the installation stays
//! playable, and this launcher changes nothing in it.

use std::collections::BTreeMap;
use std::fmt;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{FilesError, is_absent, no_links, read_bounded, write_json_atomic};
use gdl_media::Sha256Digest;
use gdl_release::{Platform, Version};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use crate::compat::{Access, Stamp, access, stamp_of};
use crate::folder::{InstallFolder, RECEIPT_FILE, check_links};
use crate::payload::{
    BUILD_INFO, BuildInfo, MAX_BUILD_INFO_LEN, MAX_VERSION_FILE_LEN, VERSION_FILE,
    version_file_holds,
};

/// The schema of the receipts this launcher reads and writes.
pub const RECEIPT_SCHEMA: u64 = 1;

/// The most bytes a receipt may hold.
pub const MAX_RECEIPT_LEN: u64 = 64 << 10;

/// The disc an installation's game data comes from: the GameCube release, by its product code.
pub const GUNE5D: &str = "GUNE5D";

/// The name of the launcher copy that an installation for `platform` keeps at its top.
pub fn installed_launcher_name(platform: Platform) -> &'static str {
    match platform {
        Platform::WindowsX64 => "GauntletDarkLegacy-Launcher.exe",
        Platform::LinuxX64 => "GauntletDarkLegacy-Launcher",
    }
}

/// What an installation's receipt records. The keys a receipt holds beyond its own, at its top and
/// inside its objects, are kept, so that what a later launcher adds survives this one's rewrite.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Receipt {
    /// The receipt's schema: [`RECEIPT_SCHEMA`] in one this launcher writes.
    pub schema: u64,
    /// The lowest launcher version allowed to change the receipt: [`MIN_LAUNCHER`] in one this
    /// launcher writes.
    ///
    /// [`MIN_LAUNCHER`]: crate::MIN_LAUNCHER
    pub min_launcher: String,
    /// The installed release's version, as its `build-info.json` records it.
    pub version: String,
    /// The commit the release was built from.
    pub commit: String,
    /// The platform the release was built for, as its `build-info.json` names it.
    pub platform: String,
    /// The disc the game's data was copied from: [`GUNE5D`].
    pub disc: String,
    /// That disc's revision, as its header gives it.
    pub disc_revision: u8,
    /// The SHA-256 digest of the installed `build-info.json`, which keeps the release's record as
    /// the release shipped it, in lower-case hexadecimal.
    pub build_info_sha256: String,
    /// The SHA-256 digest of each installed file whose bytes are not the ones `build-info.json`
    /// lists, by its name there: the Windows executable, once the disc's icon is set into it.
    pub installed_digests: BTreeMap<String, String>,
    /// Where the icon set into the Windows executable came from. An installation for Linux has
    /// none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub disc_icon: Option<DiscIcon>,
    /// The launcher copy at the installation's top.
    pub launcher: InstalledLauncher,
    /// The keys the receipt holds beyond these, kept as they are.
    #[serde(flatten)]
    pub other: Map<String, Value>,
}

/// Where the icon set into the Windows executable came from, and what the executable was before.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DiscIcon {
    /// The icon's file, by its name under the installation's top: `carddemo/icon.tpl`.
    pub path: String,
    /// The icon file's SHA-256 digest, in lower-case hexadecimal.
    pub sha256: String,
    /// The executable's SHA-256 digest as the release shipped it, before the icon was set into
    /// it, in lower-case hexadecimal.
    pub release_executable_sha256: String,
    /// The keys the record holds beyond these, kept as they are.
    #[serde(flatten)]
    pub other: Map<String, Value>,
}

/// The launcher copy an installation keeps at its top.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct InstalledLauncher {
    /// Its name: [`installed_launcher_name`] for the installation's platform.
    pub name: String,
    /// Its SHA-256 digest, in lower-case hexadecimal.
    pub sha256: String,
    /// The keys the record holds beyond these, kept as they are.
    #[serde(flatten)]
    pub other: Map<String, Value>,
}

impl Receipt {
    /// Writes the receipt as `folder`'s own, whole, in place of the one there: through a pending
    /// file that one rename puts in its place, so that a reader finds the old receipt or the new
    /// one and never part of either. The caller holds the installation's lock, and the state
    /// folder exists.
    ///
    /// A receipt whose JSON would take more than [`MAX_RECEIPT_LEN`] bytes, which no launcher
    /// would read again, is refused as [`FilesError::TooLarge`], and the one there is kept. Keys
    /// kept from a receipt read can make it so, since they are written back indented.
    pub fn write(&self, folder: &InstallFolder) -> Result<(), FilesError> {
        write_json_atomic(&receipt_path(folder), self, MAX_RECEIPT_LEN)
    }

    /// The platform the receipt names, once each of its fields holds what a receipt's may: a
    /// platform a release is built for, SHA-256 digests in lower-case hexadecimal, the disc
    /// [`GUNE5D`] and the launcher copy an installation for that platform keeps. Otherwise what is
    /// wrong.
    fn checked_platform(&self) -> Result<Platform, String> {
        let platform = Platform::parse(&self.platform).ok_or_else(|| {
            format!(
                "its platform {:?} is not one a release is built for",
                self.platform
            )
        })?;
        if let Some((key, _)) = self
            .digests()
            .into_iter()
            .find(|(_, digest)| Sha256Digest::from_hex(digest).is_none())
        {
            return Err(format!(
                "its {key} is not a SHA-256 digest in lower-case hexadecimal"
            ));
        }
        if self.disc != GUNE5D {
            return Err(format!("its disc {:?} is not {GUNE5D}", self.disc));
        }
        let launcher = installed_launcher_name(platform);
        if self.launcher.name != launcher {
            return Err(format!(
                "its launcher copy {:?} is not {launcher}, the one an installation for {} keeps",
                self.launcher.name,
                platform.as_str()
            ));
        }
        Ok(platform)
    }

    /// Every digest the receipt holds, by the key that holds it.
    fn digests(&self) -> Vec<(String, &str)> {
        let mut digests = vec![(
            "buildInfoSha256".to_owned(),
            self.build_info_sha256.as_str(),
        )];
        digests.extend(
            self.installed_digests
                .iter()
                .map(|(name, digest)| (format!("installedDigests[{name:?}]"), digest.as_str())),
        );
        if let Some(icon) = &self.disc_icon {
            digests.push(("discIcon.sha256".to_owned(), icon.sha256.as_str()));
            digests.push((
                "discIcon.releaseExecutableSha256".to_owned(),
                icon.release_executable_sha256.as_str(),
            ));
        }
        digests.push(("launcher.sha256".to_owned(), self.launcher.sha256.as_str()));
        digests
    }

    /// The installation's `build-info.json` under `root`, once it is the one whose digest the
    /// receipt records, of the receipt's version, commit and `platform`, and lists every file the
    /// receipt gives an installed digest for. Otherwise the field the receipt disagrees in.
    fn agreeing_build_info(
        &self,
        root: &Path,
        platform: Platform,
    ) -> Result<BuildInfo, ReceiptError> {
        let (bytes, build_info) = read_build_info(root)?;
        let digest = Sha256Digest::of(&bytes);
        let field = if Sha256Digest::from_hex(&self.build_info_sha256) != Some(digest) {
            Some("buildInfoSha256")
        } else if self.version != build_info.version {
            Some("version")
        } else if self.commit != build_info.commit {
            Some("commit")
        } else if platform != build_info.platform {
            Some("platform")
        } else if self
            .installed_digests
            .keys()
            .any(|name| !build_info.files.contains_key(name))
        {
            Some("installedDigests")
        } else {
            None
        };
        match field {
            Some(field) => Err(ReceiptError::Disagrees { field }),
            None => Ok(build_info),
        }
    }
}

/// An installation, as its receipt and the files the receipt pins show it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InstalledGame {
    /// The receipt; `None` when a later launcher wrote it, since this one does not interpret it.
    pub receipt: Option<Receipt>,
    /// The installed release's record, `build-info.json`, as the release shipped it.
    pub build_info: BuildInfo,
    /// The receipt's stamp.
    pub stamp: Stamp,
    /// What this launcher may do with the installation: play it always, and change it only when
    /// this is [`Access::Writable`].
    pub access: Access,
}

impl InstallFolder {
    /// Reads the installation through its receipt, and refuses it unless the receipt and the files
    /// it pins agree: the folder has no link on its path; the receipt is a JSON object of at most
    /// [`MAX_RECEIPT_LEN`] bytes, with a stamp, whose fields hold values of their kinds;
    /// `build-info.json` is the one whose digest it records, of its version, commit and platform,
    /// and lists every file it gives an installed digest for; the installation is for the platform
    /// this program was built for; `VERSION` holds the version; and the executable is a file,
    /// reached through no link.
    ///
    /// A receipt of a later schema than [`RECEIPT_SCHEMA`], or one that asks for a later launcher
    /// than `running`, is not interpreted. The installation is then read from its
    /// `build-info.json` alone, held to the same platform, `VERSION` and executable, and given back
    /// without its receipt as [`Access::ReadOnly`], so that it stays playable while this launcher
    /// changes nothing in it.
    ///
    /// Nothing is written, and no lock is taken.
    pub fn installed(&self, running: &Version) -> Result<InstalledGame, ReceiptError> {
        let root = self.root();
        check_links(root).map_err(|error| links_error(error, root))?;
        let path = receipt_path(self);
        let bytes = read_bounded(&path, MAX_RECEIPT_LEN).map_err(|error| {
            read_error(error, &path, |detail| ReceiptError::Unreadable {
                detail: detail.into(),
            })
        })?;
        let object: Map<String, Value> =
            serde_json::from_slice(&bytes).map_err(|error| unreadable(&path, error))?;
        let stamp = stamp_of(&object).map_err(|error| unreadable(&path, error))?;
        let access = access(&stamp, RECEIPT_SCHEMA, running);
        let (receipt, build_info) = match access {
            Access::Writable => {
                let receipt: Receipt = serde_json::from_value(Value::Object(object))
                    .map_err(|error| unreadable(&path, error))?;
                let platform = receipt
                    .checked_platform()
                    .map_err(|detail| unreadable(&path, detail))?;
                let build_info = receipt.agreeing_build_info(root, platform)?;
                (Some(receipt), build_info)
            }
            Access::ReadOnly => {
                let (_, build_info) = read_build_info(root)?;
                (None, build_info)
            }
        };
        check_playable(root, &build_info)?;
        Ok(InstalledGame {
            receipt,
            build_info,
            stamp,
            access,
        })
    }
}

/// Where `folder` keeps its receipt.
fn receipt_path(folder: &InstallFolder) -> PathBuf {
    folder.state_dir().join(RECEIPT_FILE)
}

/// The installation's `build-info.json` under `root`, as its bytes and as read. A file that is
/// not there, or is no release record, disagrees with the receipt.
fn read_build_info(root: &Path) -> Result<(Vec<u8>, BuildInfo), ReceiptError> {
    let path = root.join(BUILD_INFO);
    let bytes = read_bounded(&path, MAX_BUILD_INFO_LEN)
        .map_err(|error| read_error(error, &path, |_| disagrees(BUILD_INFO)))?;
    let build_info = BuildInfo::parse(&bytes).map_err(|_| disagrees(BUILD_INFO))?;
    Ok((bytes, build_info))
}

/// Refuses an installation under `root` that is not for the platform this program was built for,
/// whose `VERSION` does not hold the version `build_info` records, or whose executable is not a
/// file reached through no link.
fn check_playable(root: &Path, build_info: &BuildInfo) -> Result<(), ReceiptError> {
    if Platform::of_this_build() != Some(build_info.platform) {
        return Err(ReceiptError::OtherPlatform {
            platform: build_info.platform,
        });
    }
    check_version_file(root, &build_info.version)?;
    check_executable(root, &build_info.executable)
}

/// Refuses an installation under `root` whose `VERSION` is not a file of at most
/// [`MAX_VERSION_FILE_LEN`] bytes that holds `version` as [`version_file_holds`] says, the rule a
/// runtime's own `VERSION` was held to.
fn check_version_file(root: &Path, version: &str) -> Result<(), ReceiptError> {
    let path = root.join(VERSION_FILE);
    let bytes = read_bounded(&path, MAX_VERSION_FILE_LEN)
        .map_err(|error| read_error(error, &path, |_| disagrees(VERSION_FILE)))?;
    if version_file_holds(&bytes, version) {
        Ok(())
    } else {
        Err(disagrees(VERSION_FILE))
    }
}

/// Refuses an installation under `root` whose executable, `executable`, is reached through a link
/// or is not there as a file.
fn check_executable(root: &Path, executable: &str) -> Result<(), ReceiptError> {
    let path = root.join(executable);
    no_links(&path).map_err(|error| links_error(error, &path))?;
    match fs::symlink_metadata(&path) {
        Ok(metadata) if metadata.is_file() => Ok(()),
        Ok(_) => Err(ReceiptError::GameMissing { executable: path }),
        Err(error) if is_absent(&error) => Err(ReceiptError::GameMissing { executable: path }),
        Err(source) => Err(ReceiptError::Io { path, source }),
    }
}

/// The refusal of an installation for what a link check of `path` reported: a link and the
/// system's refusal keep their kinds, and anything else would count as the latter, on `path`.
fn links_error(error: FilesError, path: &Path) -> ReceiptError {
    match error {
        FilesError::Link { path } => ReceiptError::Links { path },
        FilesError::Io { path, source } => ReceiptError::Io { path, source },
        other => ReceiptError::Io {
            path: path.to_path_buf(),
            source: io::Error::other(other),
        },
    }
}

/// The refusal of an installation for a failed read of `file`, a file it keeps. Nothing there,
/// something there that is no regular file, and a file that holds more than its kind may are the
/// installation's fault, which `unfit` reports, given what was found; a link on the path is
/// refused as one, and anything else is the system's refusal.
fn read_error(
    error: FilesError,
    file: &Path,
    unfit: impl FnOnce(String) -> ReceiptError,
) -> ReceiptError {
    match error {
        FilesError::Io { path, source } if holds_no_file(file) => {
            unfit(format!("{}: {source}", path.display()))
        }
        too_large @ FilesError::TooLarge { .. } => unfit(too_large.to_string()),
        other => links_error(other, file),
    }
}

/// Whether nothing that could be read as a file is at `path`: nothing at all, a file in place of a
/// folder above it, or an entry that is no regular file, such as a folder. Systems differ in how
/// they refuse to open a folder as a file, so the entry is looked at rather than the refusal. A
/// refusal to look shows nothing, and is not taken for absence.
fn holds_no_file(path: &Path) -> bool {
    match fs::symlink_metadata(path) {
        Ok(metadata) => !metadata.is_file(),
        Err(error) => is_absent(&error),
    }
}

/// The refusal of the receipt at `path`, for the reason `detail` gives.
fn unreadable(path: &Path, detail: impl fmt::Display) -> ReceiptError {
    ReceiptError::Unreadable {
        detail: format!("{}: {detail}", path.display()).into(),
    }
}

/// The refusal of a receipt and files that disagree in `field`.
fn disagrees(field: &'static str) -> ReceiptError {
    ReceiptError::Disagrees { field }
}

/// What [`ReceiptError::Disagrees`] says for `field`: that the installed `VERSION` disagrees with
/// the version the receipt pins, or that the receipt and `build-info.json` disagree.
fn disagreement(field: &str) -> String {
    if field == VERSION_FILE {
        "The installed VERSION disagrees with its receipt".to_owned()
    } else {
        format!("Installation receipts disagree; choose a complete installed game ({field})")
    }
}

/// Why an installation cannot be read through its receipt, as technical detail for logs. What a
/// player reads is chosen from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum ReceiptError {
    /// `path` is a link: the installation's folder or one above it, the state folder, the receipt
    /// or a file the receipt pins.
    #[error("Choose a directory without links or junctions: {}", .path.display())]
    Links {
        /// The link.
        path: PathBuf,
    },
    /// The receipt is not there, is no JSON object of at most [`MAX_RECEIPT_LEN`] bytes, has no
    /// stamp, or holds a field that is not of its kind.
    #[error("Unreadable installation receipt: {detail}")]
    Unreadable {
        /// The receipt's path and what is wrong with it.
        detail: Box<str>,
    },
    /// The receipt and the installed files disagree.
    #[error("{}", disagreement(.field))]
    Disagrees {
        /// What disagrees: the receipt's key whose value the files do not match,
        /// `build-info.json` when no release record is there to match it, or `VERSION`.
        field: &'static str,
    },
    /// The installation is for another platform than the one this program was built for.
    #[error("This installation belongs to a different platform: {}", .platform.as_str())]
    OtherPlatform {
        /// The installation's platform.
        platform: Platform,
    },
    /// The game's executable is not there as a file.
    #[error("The installed executable is missing: {}", .executable.display())]
    GameMissing {
        /// Where the executable belongs.
        executable: PathBuf,
    },
    /// The system refused to look at or read `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The path looked at or read.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};

    use gdl_files::{ALPHA_NAMES, FilesError, PAYLOAD_NAMES, check_component, retry_locked};
    use gdl_fixtures::{
        InstallFixture, RuntimeFiles, backdate, folder_names, link_folder, pretty_json, sha256_hex,
        snapshot, symlink_file_where_allowed,
    };
    use gdl_media::Sha256Digest;
    use gdl_release::{Platform, Version};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::{
        GUNE5D, InstalledGame, MAX_RECEIPT_LEN, RECEIPT_SCHEMA, Receipt, ReceiptError,
        installed_launcher_name, links_error, read_error, receipt_path,
    };
    use crate::compat::{Access, MIN_LAUNCHER, Stamp, stamp_of};
    use crate::folder::{InstallFolder, RECEIPT_FILE, STATE_DIR};
    use crate::payload::{
        BUILD_INFO, BuildInfo, MAX_BUILD_INFO_LEN, MAX_VERSION_FILE_LEN, VERSION_FILE,
    };
    use crate::test_platform::{other_platform, this_platform};

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The memory card icon's file, which a Windows receipt names.
    const DISC_ICON: &str = "carddemo/icon.tpl";

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The version the tests run the launcher as: the first launcher's.
    fn running() -> Version {
        version(MIN_LAUNCHER)
    }

    /// A digest of the right form that is no file's: every byte nought.
    fn unmatched_digest() -> String {
        Sha256Digest(Default::default()).hex()
    }

    /// An installation of the standard runtime of `version` for the platform this test program
    /// was built for, as the launcher leaves one, in a folder of the test's own.
    fn installation_of(version: &str) -> (TempDir, InstallFixture, InstallFolder) {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("install");
        let runtime = RuntimeFiles::standard(this_platform().as_str(), version);
        let fixture = InstallFixture::write(&root, runtime).unwrap();
        let folder = InstallFolder::at(&root).unwrap();
        (dir, fixture, folder)
    }

    /// An installation of the standard runtime of the first launcher's version, for the platform
    /// this test program was built for.
    fn installation() -> (TempDir, InstallFixture, InstallFolder) {
        installation_of(MIN_LAUNCHER)
    }

    /// An installation whose records, `build-info.json` and the receipt, are the ones the launcher
    /// leaves for the standard runtime of `platform`, over the files of this platform's. A Linux
    /// installation cannot be laid out where names ignore letter case, as on Windows, since its
    /// executable `gauntlet` and the disc's `Gauntlet` are one name there; and nothing of an
    /// installation but its records is read before its platform is judged.
    fn recorded_for(platform: Platform) -> (TempDir, InstallFixture, InstallFolder) {
        let (dir, mut fixture, folder) = installation();
        fixture.runtime = RuntimeFiles::standard(platform.as_str(), MIN_LAUNCHER);
        let build_info = folder.root().join(BUILD_INFO);
        fs::write(build_info, fixture.runtime.build_info_bytes()).unwrap();
        fixture.write_receipt(&fixture.receipt()).unwrap();
        (dir, fixture, folder)
    }

    /// Writes the fixture's own receipt, each of `changes` giving one of its keys a new value, in
    /// place of the installed one.
    fn write_changed_receipt(fixture: &InstallFixture, changes: &[(&str, Value)]) {
        let mut receipt = fixture.receipt();
        for (key, value) in changes {
            receipt[*key] = value.clone();
        }
        fixture.write_receipt(&receipt).unwrap();
    }

    /// The fixture's own launcher record, naming the launcher copy of `platform`.
    fn launcher_of(fixture: &InstallFixture, platform: Platform) -> Value {
        let mut launcher = fixture.receipt()["launcher"].clone();
        launcher["name"] = json!(installed_launcher_name(platform));
        launcher
    }

    /// Reads the installation in `folder` as the first launcher does.
    fn read(folder: &InstallFolder) -> Result<InstalledGame, ReceiptError> {
        folder.installed(&running())
    }

    /// The installation's executable.
    fn executable_of(folder: &InstallFolder) -> PathBuf {
        folder.root().join(this_platform().executable())
    }

    /// Removes the file at `path`, waiting out a scanner that still looks at a new one, as one may
    /// on Windows.
    fn remove_file(path: &Path) {
        retry_locked(|| fs::remove_file(path))
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
    }

    /// Moves `from` to `to`, waiting out a scanner as [`remove_file`] does.
    fn rename(from: &Path, to: &Path) {
        retry_locked(|| fs::rename(from, to))
            .unwrap_or_else(|error| panic!("{}: {error}", from.display()));
    }

    /// Checks that `result` refused the link at `link`.
    #[track_caller]
    fn assert_links(result: Result<InstalledGame, ReceiptError>, link: &Path) {
        match result {
            Err(ReceiptError::Links { path }) => assert_eq!(path, link),
            other => panic!("{other:?}"),
        }
    }

    /// Checks that `result` refused a receipt and files that disagree in `field`, for the case
    /// `what` names.
    #[track_caller]
    fn assert_disagrees(result: Result<InstalledGame, ReceiptError>, field: &str, what: &str) {
        match result {
            Err(ReceiptError::Disagrees { field: found }) => assert_eq!(found, field, "{what}"),
            other => panic!("{what}: {other:?}"),
        }
    }

    /// Checks that `result` refused the receipt as unreadable, for the case `what` names, and
    /// gives back what was wrong with it.
    #[track_caller]
    fn assert_unreadable(result: Result<InstalledGame, ReceiptError>, what: &str) -> Box<str> {
        match result {
            Err(ReceiptError::Unreadable { detail }) => detail,
            other => panic!("{what}: {other:?}"),
        }
    }

    /// Checks that `result` found no executable at `executable`.
    #[track_caller]
    fn assert_game_missing(result: Result<InstalledGame, ReceiptError>, executable: &Path) {
        match result {
            Err(ReceiptError::GameMissing { executable: found }) => assert_eq!(found, executable),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn reads_an_installation_through_its_receipt() {
        let (_dir, fixture, folder) = installation();
        let game = read(&folder).unwrap();
        assert_eq!(game.access, Access::Writable);
        assert_eq!(
            game.stamp,
            Stamp {
                schema: RECEIPT_SCHEMA,
                min_launcher: running()
            }
        );
        let shipped = BuildInfo::parse(&fixture.runtime.build_info_bytes()).unwrap();
        assert_eq!(game.build_info, shipped);
        let receipt = game
            .receipt
            .expect("the first launcher's own receipt is read");
        assert_eq!(
            receipt,
            serde_json::from_value::<Receipt>(fixture.receipt()).unwrap()
        );
        assert_eq!(receipt.version, MIN_LAUNCHER);
        assert_eq!(receipt.commit, "test-commit");
        assert_eq!(receipt.platform, this_platform().as_str());
        assert_eq!(receipt.disc, GUNE5D);
        assert_eq!(receipt.disc_revision, 1);
        assert_eq!(
            receipt.launcher.name,
            installed_launcher_name(this_platform())
        );
        assert!(receipt.other.is_empty());
        // The Windows executable carries the disc's icon, so the receipt pins it by a digest of
        // its own.
        let personalised: Vec<&String> = receipt.installed_digests.keys().collect();
        match this_platform() {
            Platform::WindowsX64 => {
                assert_eq!(personalised, ["gauntlet.exe"]);
                assert_eq!(receipt.disc_icon.as_ref().unwrap().path, DISC_ICON);
            }
            Platform::LinuxX64 => {
                assert!(personalised.is_empty());
                assert_eq!(receipt.disc_icon, None);
            }
        }

        // Later launchers, and one built with build metadata, read it the same way.
        for running in ["1.0.0-beta+build.7", "1.0.0-beta.1", "1.0.0", "2.0.0"] {
            let game = folder.installed(&version(running)).unwrap();
            assert_eq!(game.access, Access::Writable, "{running}");
            assert!(game.receipt.is_some(), "{running}");
        }
    }

    #[test]
    fn a_disagreeing_version_is_invalid() {
        let (_dir, fixture, folder) = installation();
        let other = other_platform();
        for (what, changes, field) in [
            (
                "an older version",
                vec![("version", json!("0.1.0-alpha.2"))],
                "version",
            ),
            (
                "a later version",
                vec![("version", json!("1.0.1"))],
                "version",
            ),
            (
                "the version with build metadata",
                vec![("version", json!("1.0.0-beta+build.1"))],
                "version",
            ),
            (
                "another commit",
                vec![("commit", json!("another-commit"))],
                "commit",
            ),
            (
                "the other platform, with its launcher copy",
                vec![
                    ("platform", json!(other.as_str())),
                    ("launcher", launcher_of(&fixture, other)),
                ],
                "platform",
            ),
        ] {
            write_changed_receipt(&fixture, &changes);
            // Dated back first, so that even bytes written again as they were would show.
            backdate(folder.root()).unwrap();
            let before = snapshot(folder.root()).unwrap();
            assert_disagrees(read(&folder), field, what);
            assert_eq!(snapshot(folder.root()).unwrap(), before, "{what}");
        }
        assert_eq!(
            ReceiptError::Disagrees { field: "version" }.to_string(),
            "Installation receipts disagree; choose a complete installed game (version)"
        );
        // With its own receipt back, the installation reads again.
        fixture.write_receipt(&fixture.receipt()).unwrap();
        read(&folder).unwrap();
    }

    #[test]
    fn another_platform_is_refused() {
        let other = other_platform();
        let (_dir, fixture, folder) = recorded_for(other);
        let refused = |result: Result<InstalledGame, ReceiptError>| match result {
            Err(ReceiptError::OtherPlatform { platform }) => assert_eq!(platform, other),
            unexpected => panic!("{unexpected:?}"),
        };
        // Its receipt names that platform's launcher copy, which is judged by the receipt's own
        // platform, so the installation is refused for its platform alone.
        assert_eq!(
            fixture.receipt()["launcher"]["name"],
            installed_launcher_name(other)
        );
        refused(read(&folder));
        // So it is when a later launcher wrote the receipt.
        write_changed_receipt(&fixture, &[("schema", json!(2))]);
        refused(read(&folder));

        // A receipt for that platform that names this platform's launcher copy is not one an
        // installation could have.
        write_changed_receipt(
            &fixture,
            &[("launcher", launcher_of(&fixture, this_platform()))],
        );
        let detail = assert_unreadable(read(&folder), "this platform's launcher copy");
        assert!(
            detail.contains(installed_launcher_name(this_platform())),
            "{detail}"
        );
        assert_eq!(
            ReceiptError::OtherPlatform { platform: other }.to_string(),
            format!(
                "This installation belongs to a different platform: {}",
                other.as_str()
            )
        );
    }

    #[test]
    fn nothing_changes_while_reading() {
        let (_dir, fixture, folder) = installation();
        // Dated back first, so that even bytes written again as they were would show.
        backdate(folder.root()).unwrap();
        let before = snapshot(folder.root()).unwrap();
        read(&folder).unwrap();
        assert_eq!(snapshot(folder.root()).unwrap(), before);

        // Nor while reading a later launcher's receipt, or refusing one.
        let mut disagreeing = fixture.receipt();
        disagreeing["commit"] = json!("another-commit");
        for (what, receipt) in [
            (
                "a later launcher's",
                json!({"schema": 2, "minLauncher": MIN_LAUNCHER}),
            ),
            ("one that disagrees", disagreeing),
            ("one that cannot be read", json!([])),
        ] {
            fixture.write_receipt(&receipt).unwrap();
            backdate(folder.root()).unwrap();
            let before = snapshot(folder.root()).unwrap();
            let _ = read(&folder);
            assert_eq!(snapshot(folder.root()).unwrap(), before, "{what}");
        }
        // No lock file, and nothing else, was made beside the receipt.
        assert_eq!(folder_names(&folder.state_dir()).unwrap(), [RECEIPT_FILE]);
    }

    #[test]
    fn a_missing_executable_is_game_missing() {
        let (_dir, _fixture, folder) = installation();
        let executable = executable_of(&folder);
        remove_file(&executable);
        assert_game_missing(read(&folder), &executable);
        // A folder in its place is no executable either.
        fs::create_dir(&executable).unwrap();
        assert_game_missing(read(&folder), &executable);
        assert_eq!(
            ReceiptError::GameMissing {
                executable: executable.clone()
            }
            .to_string(),
            format!(
                "The installed executable is missing: {}",
                executable.display()
            )
        );
    }

    #[test]
    fn a_linked_executable_is_refused() {
        // A link to a folder in the executable's place: on Windows a junction.
        let (dir, _fixture, folder) = installation();
        let executable = executable_of(&folder);
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        remove_file(&executable);
        link_folder(&executable, &elsewhere).unwrap();
        assert_links(read(&folder), &executable);
        assert!(folder_names(&elsewhere).unwrap().is_empty());

        // A link to the executable itself, where this account may make one.
        let (dir, _fixture, folder) = installation();
        let executable = executable_of(&folder);
        let moved = dir.path().join("moved");
        rename(&executable, &moved);
        if !symlink_file_where_allowed(&executable, &moved) {
            eprintln!("skipped the link to a file: this account may not make one");
            return;
        }
        assert_links(read(&folder), &executable);
    }

    #[test]
    fn a_changed_version_file_is_invalid() {
        let (dir, _fixture, folder) = installation();
        let version_file = folder.root().join(VERSION_FILE);
        let longest = usize::try_from(MAX_VERSION_FILE_LEN).unwrap();
        let padded = |len: usize| {
            let spaces = " ".repeat(len - MIN_LAUNCHER.len());
            format!("{MIN_LAUNCHER}{spaces}").into_bytes()
        };
        // The version, with nothing but ASCII whitespace about it, in at most 128 bytes.
        for (what, bytes) in [
            ("as installed", format!("{MIN_LAUNCHER}\n").into_bytes()),
            ("alone", MIN_LAUNCHER.as_bytes().to_vec()),
            ("with CRLF", format!("{MIN_LAUNCHER}\r\n").into_bytes()),
            (
                "between spaces and tabs",
                format!(" \t{MIN_LAUNCHER} \t\n").into_bytes(),
            ),
            ("of the longest length", padded(longest)),
        ] {
            fs::write(&version_file, &bytes).unwrap();
            read(&folder).unwrap_or_else(|error| panic!("{what}: {error}"));
        }
        for (what, bytes) in [
            ("another version", b"0.1.0-alpha.3\n".to_vec()),
            (
                "the version with build metadata",
                format!("{MIN_LAUNCHER}+x\n").into_bytes(),
            ),
            ("a tag", format!("v{MIN_LAUNCHER}\n").into_bytes()),
            ("nothing", Vec::new()),
            ("a space inside", b"1.0.0- beta\n".to_vec()),
            (
                "not UTF-8",
                [MIN_LAUNCHER.as_bytes(), &b"\xff"[..]].concat(),
            ),
            ("a byte too long", padded(longest + 1)),
            (
                "whitespace that is not ASCII",
                format!("\u{a0}{MIN_LAUNCHER}\n").into_bytes(),
            ),
        ] {
            fs::write(&version_file, &bytes).unwrap();
            assert_disagrees(read(&folder), VERSION_FILE, what);
        }
        // None at all, or a folder in its place.
        remove_file(&version_file);
        assert_disagrees(read(&folder), VERSION_FILE, "no VERSION");
        fs::create_dir(&version_file).unwrap();
        assert_disagrees(read(&folder), VERSION_FILE, "a folder");
        // A link in its place is refused as one.
        fs::remove_dir(&version_file).unwrap();
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        link_folder(&version_file, &elsewhere).unwrap();
        assert_links(read(&folder), &version_file);
        assert_eq!(
            ReceiptError::Disagrees {
                field: VERSION_FILE
            }
            .to_string(),
            "The installed VERSION disagrees with its receipt"
        );
    }

    #[test]
    fn a_tampered_build_info_is_invalid() {
        let (_dir, fixture, folder) = installation();
        let build_info = folder.root().join(BUILD_INFO);
        let shipped = fixture.runtime.build_info_bytes();
        let mut keyed = fixture.runtime.clone();
        keyed.extra.insert("tampered".to_owned(), json!(true));
        let mut later = fixture.runtime.clone();
        later.version = "1.0.1".to_owned();
        // A record that still reads as one, but with a key added, of another version or only laid
        // out otherwise, is not the one the receipt pins.
        for (what, bytes) in [
            ("a key added", keyed.build_info_bytes()),
            ("another version", later.build_info_bytes()),
            (
                "written compact",
                serde_json::to_vec(&fixture.runtime.build_info()).unwrap(),
            ),
            (
                "without its line end",
                shipped[..shipped.len() - 1].to_vec(),
            ),
        ] {
            fs::write(&build_info, &bytes).unwrap();
            assert_disagrees(read(&folder), "buildInfoSha256", what);
        }
        // Nor is a file that is no release record, or one too large to be one, or none at all.
        for (what, bytes) in [
            ("not JSON", b"not JSON\n".to_vec()),
            ("an empty object", b"{}\n".to_vec()),
            (
                "a byte too large",
                vec![b' '; usize::try_from(MAX_BUILD_INFO_LEN + 1).unwrap()],
            ),
        ] {
            fs::write(&build_info, &bytes).unwrap();
            assert_disagrees(read(&folder), BUILD_INFO, what);
        }
        remove_file(&build_info);
        assert_disagrees(read(&folder), BUILD_INFO, "no record");

        // With the shipped record back, the installation reads again, until the receipt pins
        // another.
        fs::write(&build_info, &shipped).unwrap();
        read(&folder).unwrap();
        write_changed_receipt(&fixture, &[("buildInfoSha256", json!(unmatched_digest()))]);
        assert_disagrees(read(&folder), "buildInfoSha256", "another digest");

        // An installed digest for a file the record does not list: the record itself, a name no
        // runtime holds, another letter case of one it lists, or another platform's executable.
        for name in [
            BUILD_INFO,
            "saves/hero.json",
            "Data/config.json",
            other_platform().executable(),
        ] {
            let digests = json!({name: unmatched_digest()});
            write_changed_receipt(&fixture, &[("installedDigests", digests)]);
            assert_disagrees(read(&folder), "installedDigests", name);
        }
        // One for a file it lists is the receipt's to give.
        let digests = json!({"data/config.json": unmatched_digest()});
        write_changed_receipt(&fixture, &[("installedDigests", digests)]);
        read(&folder).unwrap();
    }

    #[test]
    fn build_info_is_read_to_its_bound() {
        let (_dir, fixture, folder) = installation();
        let build_info = folder.root().join(BUILD_INFO);
        let longest = usize::try_from(MAX_BUILD_INFO_LEN).unwrap();
        let later = json!({"schema": 2, "minLauncher": MIN_LAUNCHER});
        for (len, within) in [(longest, true), (longest + 1, false)] {
            // The shipped record with spaces after it, which reads as the same record, and a
            // receipt that pins it, so that only the bound can refuse it; then a later launcher's
            // receipt, which pins nothing.
            let mut bytes = fixture.runtime.build_info_bytes();
            bytes.resize(len, b' ');
            fs::write(&build_info, &bytes).unwrap();
            write_changed_receipt(&fixture, &[("buildInfoSha256", json!(sha256_hex(&bytes)))]);
            let own = read(&folder);
            fixture.write_receipt(&later).unwrap();
            let read_only = read(&folder);
            for (what, result) in [("its own receipt", own), ("a later receipt", read_only)] {
                let what = format!("{len} bytes under {what}");
                if within {
                    let game = result.unwrap_or_else(|error| panic!("{what}: {error}"));
                    assert_eq!(game.build_info.version, MIN_LAUNCHER, "{what}");
                } else {
                    assert_disagrees(result, BUILD_INFO, &what);
                }
            }
        }
    }

    #[test]
    fn the_bound_is_64_kib() {
        assert_eq!(MAX_RECEIPT_LEN, 65_536);
        let (_dir, fixture, folder) = installation();
        let receipt_file = receipt_path(&folder);
        // The fixture's receipt, padded by a key it does not know to exactly `len` bytes.
        let pad = |len: u64| {
            let mut receipt = fixture.receipt();
            receipt["padding"] = json!("");
            fixture.write_receipt(&receipt).unwrap();
            let unpadded = fs::metadata(&receipt_file).unwrap().len();
            let padding = "x".repeat(usize::try_from(len - unpadded).unwrap());
            receipt["padding"] = json!(padding);
            fixture.write_receipt(&receipt).unwrap();
            assert_eq!(fs::metadata(&receipt_file).unwrap().len(), len);
        };
        pad(MAX_RECEIPT_LEN);
        let receipt = read(&folder).unwrap().receipt.unwrap();
        assert!(receipt.other.contains_key("padding"));
        pad(MAX_RECEIPT_LEN + 1);
        let detail = assert_unreadable(read(&folder), "a byte too many");
        assert!(detail.contains("too large"), "{detail}");
    }

    #[test]
    fn a_receipt_is_written_only_within_the_bound_it_is_read_to() {
        /// How many items a key this launcher does not know holds in the receipt below: written
        /// compact they fit the bound, but not once written back indented, a line each.
        const FUTURE_ITEMS: usize = 10_000;

        let (_dir, fixture, folder) = installation();
        let receipt_file = receipt_path(&folder);
        let mut receipt = fixture.receipt();
        receipt["future"] = json!(vec![1; FUTURE_ITEMS]);
        // Written compact, as no launcher writes one, the receipt reads within its bound.
        let compact = serde_json::to_vec(&receipt).unwrap();
        assert!(u64::try_from(compact.len()).unwrap() <= MAX_RECEIPT_LEN);
        fs::write(&receipt_file, &compact).unwrap();
        let read_back = read(&folder).unwrap().receipt.unwrap();
        assert_eq!(read_back.other["future"], receipt["future"]);

        // Written back indented, a line an item, it would be a receipt no launcher reads, so it is
        // refused, and the receipt there is kept as it was.
        assert!(u64::try_from(pretty_json(&receipt).len()).unwrap() > MAX_RECEIPT_LEN);
        match read_back.write(&folder) {
            Err(FilesError::TooLarge { path, limit }) => {
                assert_eq!(path, receipt_file);
                assert_eq!(limit, MAX_RECEIPT_LEN);
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(fs::read(&receipt_file).unwrap(), compact);
        assert_eq!(folder_names(&folder.state_dir()).unwrap(), [RECEIPT_FILE]);
        read(&folder).unwrap();
    }

    #[test]
    fn a_linked_receipt_is_refused() {
        // A link to a folder in the receipt's place: on Windows a junction.
        let (dir, _fixture, folder) = installation();
        let receipt = receipt_path(&folder);
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        remove_file(&receipt);
        link_folder(&receipt, &elsewhere).unwrap();
        assert_links(read(&folder), &receipt);

        // The state folder a link, to a folder that holds the receipt as it was.
        let (dir, fixture, folder) = installation();
        let state = folder.state_dir();
        let moved = dir.path().join("moved state");
        rename(&state, &moved);
        link_folder(&state, &moved).unwrap();
        assert_links(read(&folder), &state);
        // Nor is a receipt written through it.
        let written: Receipt = serde_json::from_value(fixture.receipt()).unwrap();
        match written.write(&folder) {
            Err(FilesError::Link { path }) => assert_eq!(path, state),
            other => panic!("{other:?}"),
        }
        assert_eq!(folder_names(&moved).unwrap(), [RECEIPT_FILE]);

        // A link to the receipt itself, where this account may make one.
        let (dir, _fixture, folder) = installation();
        let receipt = receipt_path(&folder);
        let moved = dir.path().join("moved receipt.json");
        rename(&receipt, &moved);
        if !symlink_file_where_allowed(&receipt, &moved) {
            eprintln!("skipped the link to a file: this account may not make one");
            return;
        }
        assert_links(read(&folder), &receipt);
    }

    #[test]
    fn round_trips_and_keeps_unknown_keys() {
        let (_dir, fixture, folder) = installation();
        let mut receipt = fixture.receipt();
        receipt["future"] = json!({"x": 1});
        // A Linux receipt has no icon record, so one is given it here to carry a key too.
        if receipt.get("discIcon").is_none() {
            receipt["discIcon"] = json!({
                "path": DISC_ICON,
                "sha256": unmatched_digest(),
                "releaseExecutableSha256": unmatched_digest(),
            });
        }
        receipt["discIcon"]["kept"] = json!("by the icon");
        receipt["launcher"]["kept"] = json!([1, 2.5, null]);
        fixture.write_receipt(&receipt).unwrap();

        let read_back = read(&folder).unwrap().receipt.unwrap();
        assert_eq!(read_back.other.len(), 1);
        assert_eq!(read_back.other.get("future"), Some(&json!({"x": 1})));
        let icon = read_back.disc_icon.as_ref().unwrap();
        assert_eq!(icon.other.get("kept"), Some(&json!("by the icon")));
        assert_eq!(
            read_back.launcher.other.get("kept"),
            Some(&json!([1, 2.5, null]))
        );

        // Written back, it holds every key it was read with, its own first and in their order,
        // as the launcher writes JSON.
        read_back.write(&folder).unwrap();
        let bytes = fs::read(receipt_path(&folder)).unwrap();
        assert_eq!(serde_json::from_slice::<Value>(&bytes).unwrap(), receipt);
        let text = std::str::from_utf8(&bytes).unwrap();
        let start = format!(
            "{{\n  \"schema\": 1,\n  \"minLauncher\": \"{MIN_LAUNCHER}\",\n  \"version\": \"{MIN_LAUNCHER}\",\n  \"commit\": "
        );
        assert!(text.starts_with(&start), "{text}");
        assert!(
            text.ends_with("\n  \"future\": {\n    \"x\": 1\n  }\n}\n"),
            "{text}"
        );
        assert!(!text.contains('\r'), "{text}");
        assert_eq!(read(&folder).unwrap().receipt.unwrap(), read_back);
        // The write left nothing beside the receipt.
        assert_eq!(folder_names(&folder.state_dir()).unwrap(), [RECEIPT_FILE]);
    }

    #[test]
    fn linux_receipts_carry_no_disc_icon() {
        // The receipt each platform's installation gets, which takes no files to give.
        for platform in Platform::ALL {
            let fixture = InstallFixture {
                root: PathBuf::from("install"),
                runtime: RuntimeFiles::standard(platform.as_str(), MIN_LAUNCHER),
            };
            let value = fixture.receipt();
            let receipt: Receipt = serde_json::from_value(value.clone()).unwrap();
            // Read and written again, a receipt is what it was, with no icon record where it had
            // none.
            assert_eq!(
                serde_json::to_value(&receipt).unwrap(),
                value,
                "{platform:?}"
            );
            match platform {
                Platform::LinuxX64 => {
                    assert!(value.get("discIcon").is_none());
                    assert_eq!(receipt.disc_icon, None);
                    assert!(receipt.installed_digests.is_empty());
                }
                Platform::WindowsX64 => {
                    let icon = receipt
                        .disc_icon
                        .expect("a Windows receipt has an icon record");
                    assert_eq!(icon.path, DISC_ICON);
                    let personalised: Vec<&String> = receipt.installed_digests.keys().collect();
                    assert_eq!(personalised, ["gauntlet.exe"]);
                }
            }
        }

        // An icon record of null reads as none, and is not written back.
        let (_dir, fixture, folder) = installation();
        write_changed_receipt(&fixture, &[("discIcon", Value::Null)]);
        let receipt = read(&folder).unwrap().receipt.unwrap();
        assert_eq!(receipt.disc_icon, None);
        let written = serde_json::to_value(&receipt).unwrap();
        assert!(written.get("discIcon").is_none(), "{written}");
    }

    #[test]
    fn unreadable_receipts_are_refused() {
        let (_dir, fixture, folder) = installation();
        let receipt_file = receipt_path(&folder);
        let shown = receipt_file.display().to_string();
        let this = this_platform();

        // Bytes that hold no JSON object.
        let own = serde_json::to_vec_pretty(&fixture.receipt()).unwrap();
        for (what, bytes) in [
            ("not JSON", b"not JSON\n".to_vec()),
            ("nothing", Vec::new()),
            ("an array", b"[]\n".to_vec()),
            ("a text", b"\"receipt\"\n".to_vec()),
            (
                "a byte-order mark first",
                [&b"\xef\xbb\xbf"[..], &own[..]].concat(),
            ),
            (
                "a second object after it",
                [&own[..], &b"\n{}\n"[..]].concat(),
            ),
        ] {
            fs::write(&receipt_file, &bytes).unwrap();
            let detail = assert_unreadable(read(&folder), what);
            assert!(detail.starts_with(&shown), "{what}: {detail}");
        }

        // Fields that are not of their kinds, or do not hold what a receipt's do.
        let mut icon = fixture
            .receipt()
            .get("discIcon")
            .cloned()
            .unwrap_or_else(|| {
                json!({
                    "path": DISC_ICON,
                    "sha256": unmatched_digest(),
                    "releaseExecutableSha256": unmatched_digest(),
                })
            });
        icon["releaseExecutableSha256"] = json!("not a digest");
        let mut launcher_digest = fixture.receipt()["launcher"].clone();
        launcher_digest["sha256"] = json!(unmatched_digest().replace('0', "A"));
        let mut launcher_case = fixture.receipt()["launcher"].clone();
        launcher_case["name"] = json!(installed_launcher_name(this).to_ascii_lowercase());
        let mut alpha_launcher = fixture.receipt()["launcher"].clone();
        alpha_launcher["name"] = json!("GauntletDarkLegacy-Update.exe");
        let upper_digest = unmatched_digest().replace('0', "F");
        for (what, changes) in [
            ("schema 0", vec![("schema", json!(0))]),
            (
                "a lowest launcher that is a tag",
                vec![("minLauncher", json!("v1.0.0-beta"))],
            ),
            ("a commit of 5", vec![("commit", json!(5))]),
            ("a revision past a byte", vec![("discRevision", json!(256))]),
            ("a negative revision", vec![("discRevision", json!(-1))]),
            ("a revision as text", vec![("discRevision", json!("1"))]),
            (
                "a platform no release is built for",
                vec![("platform", json!("another-os"))],
            ),
            (
                "a platform in another letter case",
                vec![("platform", json!(this.as_str().to_ascii_uppercase()))],
            ),
            ("another disc", vec![("disc", json!("GUNE5E"))]),
            (
                "the disc in another letter case",
                vec![("disc", json!("gune5d"))],
            ),
            (
                "a digest in upper case",
                vec![("buildInfoSha256", json!(upper_digest))],
            ),
            ("a short digest", vec![("buildInfoSha256", json!("abc123"))]),
            (
                "an installed digest that is none",
                vec![(
                    "installedDigests",
                    json!({"data/config.json": "not a digest"}),
                )],
            ),
            (
                "installed digests in a list",
                vec![("installedDigests", json!([]))],
            ),
            ("an icon record with no digest", vec![("discIcon", icon)]),
            (
                "a launcher copy's digest in upper case",
                vec![("launcher", launcher_digest)],
            ),
            (
                "the launcher copy in another letter case",
                vec![("launcher", launcher_case)],
            ),
            (
                "the alpha updater as the launcher copy",
                vec![("launcher", alpha_launcher)],
            ),
            ("no launcher record", vec![("launcher", Value::Null)]),
        ] {
            write_changed_receipt(&fixture, &changes);
            let detail = assert_unreadable(read(&folder), what);
            assert!(detail.starts_with(&shown), "{what}: {detail}");
        }

        // Keys every receipt has, missing.
        for key in [
            "schema",
            "minLauncher",
            "version",
            "commit",
            "platform",
            "disc",
            "discRevision",
            "buildInfoSha256",
            "installedDigests",
            "launcher",
        ] {
            let mut receipt = fixture.receipt();
            receipt.as_object_mut().unwrap().remove(key);
            fixture.write_receipt(&receipt).unwrap();
            assert_unreadable(read(&folder), key);
        }

        // No receipt at all, or a folder in its place.
        remove_file(&receipt_file);
        let detail = assert_unreadable(read(&folder), "no receipt");
        assert!(detail.starts_with(&shown), "{detail}");
        fs::create_dir(&receipt_file).unwrap();
        let detail = assert_unreadable(read(&folder), "a folder");
        assert!(detail.starts_with(&shown), "{detail}");
        assert_eq!(
            ReceiptError::Unreadable {
                detail: "it is not JSON".into()
            }
            .to_string(),
            "Unreadable installation receipt: it is not JSON"
        );
    }

    #[test]
    fn a_newer_receipt_is_read_only_and_still_playable() {
        let (_dir, fixture, folder) = installation_of("1.0.1");
        for (what, receipt) in [
            (
                "a later schema",
                json!({
                    "schema": 2,
                    "minLauncher": MIN_LAUNCHER,
                    "version": 7,
                    "platform": ["linux-x64"],
                    "disc": null,
                    "launcher": "of a later kind",
                    "future": {"x": 1},
                }),
            ),
            (
                "a later launcher",
                json!({
                    "schema": 1,
                    "minLauncher": "1.0.1",
                    "buildInfoSha256": "not a digest",
                    "installedDigests": "of a later kind",
                }),
            ),
        ] {
            fixture.write_receipt(&receipt).unwrap();
            let game = read(&folder).unwrap_or_else(|error| panic!("{what}: {error}"));
            assert_eq!(game.access, Access::ReadOnly, "{what}");
            assert_eq!(game.receipt, None, "{what}");
            assert_eq!(game.build_info.version, "1.0.1", "{what}");
            let shipped = BuildInfo::parse(&fixture.runtime.build_info_bytes()).unwrap();
            assert_eq!(game.build_info, shipped, "{what}");
            let stamp = stamp_of(receipt.as_object().unwrap()).unwrap();
            assert_eq!(game.stamp, stamp, "{what}");
        }
        // The launcher the receipt asks for reads it as its own, and refuses what it holds.
        let detail = assert_unreadable(folder.installed(&version("1.0.1")), "its own launcher");
        assert!(detail.contains("of a later kind"), "{detail}");

        // It is still held to the files the game plays from.
        let version_file = folder.root().join(VERSION_FILE);
        fs::write(&version_file, format!("{MIN_LAUNCHER}\n")).unwrap();
        assert_disagrees(read(&folder), VERSION_FILE, "a VERSION of another version");
        fs::write(&version_file, b"1.0.1\n").unwrap();
        let executable = executable_of(&folder);
        let kept = fs::read(&executable).unwrap();
        remove_file(&executable);
        assert_game_missing(read(&folder), &executable);
        fs::write(&executable, kept).unwrap();
        let build_info = folder.root().join(BUILD_INFO);
        fs::write(&build_info, b"not JSON\n").unwrap();
        assert_disagrees(read(&folder), BUILD_INFO, "a record that is not one");
        fs::write(&build_info, fixture.runtime.build_info_bytes()).unwrap();
        assert_eq!(read(&folder).unwrap().access, Access::ReadOnly);

        // A stamp that cannot be read leaves nothing to judge by, whatever schema it gives.
        for receipt in [
            json!({"schema": 2}),
            json!({"schema": 2, "minLauncher": "v1.0.1"}),
            json!({"minLauncher": "9.0.0"}),
        ] {
            fixture.write_receipt(&receipt).unwrap();
            let detail = assert_unreadable(read(&folder), &receipt.to_string());
            assert!(
                detail.contains("schema") || detail.contains("minLauncher"),
                "{detail}"
            );
        }
    }

    #[cfg(unix)]
    #[test]
    fn refusals_to_read_are_the_systems() {
        use gdl_fixtures::DeniedAccess;

        let (_dir, _fixture, folder) = installation();
        let receipt = receipt_path(&folder);
        let assert_io =
            |result: Result<InstalledGame, ReceiptError>, at: &Path, what: &str| match result {
                Err(ReceiptError::Io { path, source }) => {
                    assert_eq!(path, at, "{what}");
                    assert_eq!(source.kind(), io::ErrorKind::PermissionDenied, "{what}");
                }
                other => panic!("{what}: {other:?}"),
            };
        let denied = DeniedAccess::deny(&[&receipt]).unwrap();
        if fs::read(&receipt).is_ok() {
            eprintln!("skipped: this user reads files of mode 0o000, as root does");
            return;
        }
        // A receipt that is there but may not be read is the system's refusal, not the
        // receipt's fault.
        assert_io(read(&folder), &receipt, "the receipt");
        drop(denied);

        // So is a file the receipt pins, and a folder that hides what it holds, which shows
        // neither a link nor an absence.
        let root = folder.root();
        for (what, hidden, refused) in [
            ("the state folder", folder.state_dir(), receipt.clone()),
            (
                "build-info.json",
                root.join(BUILD_INFO),
                root.join(BUILD_INFO),
            ),
            ("VERSION", root.join(VERSION_FILE), root.join(VERSION_FILE)),
            (
                "the folder above the installation",
                root.parent().unwrap().to_path_buf(),
                root.to_path_buf(),
            ),
        ] {
            let held = DeniedAccess::deny(&[&hidden]).unwrap();
            assert_io(read(&folder), &refused, what);
            drop(held);
        }
        read(&folder).unwrap();
    }

    #[test]
    fn receipt_errors_keep_their_kinds() {
        let path = Path::new("install").join(STATE_DIR).join(RECEIPT_FILE);
        let shown = path.display();
        let denied = || io::Error::new(io::ErrorKind::PermissionDenied, "denied");
        for (error, text) in [
            (
                ReceiptError::Links { path: path.clone() },
                format!("Choose a directory without links or junctions: {shown}"),
            ),
            (
                ReceiptError::Unreadable {
                    detail: "it is not JSON".into(),
                },
                "Unreadable installation receipt: it is not JSON".to_owned(),
            ),
            (
                ReceiptError::Disagrees { field: "commit" },
                "Installation receipts disagree; choose a complete installed game (commit)"
                    .to_owned(),
            ),
            (
                ReceiptError::Disagrees {
                    field: VERSION_FILE,
                },
                "The installed VERSION disagrees with its receipt".to_owned(),
            ),
            (
                ReceiptError::OtherPlatform {
                    platform: Platform::LinuxX64,
                },
                "This installation belongs to a different platform: linux-x64".to_owned(),
            ),
            (
                ReceiptError::GameMissing {
                    executable: path.clone(),
                },
                format!("The installed executable is missing: {shown}"),
            ),
            (
                ReceiptError::Io {
                    path: path.clone(),
                    source: denied(),
                },
                format!("{shown}: denied"),
            ),
        ] {
            assert_eq!(error.to_string(), text);
            match &error {
                ReceiptError::Io { .. } => {
                    assert_eq!(error.source().unwrap().to_string(), "denied");
                }
                _ => assert!(error.source().is_none(), "{text}"),
            }
        }

        // A link check's links and the system's refusals keep their kinds, and anything else it
        // came to report would be kept whole as the system's refusal of the path checked.
        let link = Path::new("link");
        match links_error(FilesError::Link { path: link.into() }, &path) {
            ReceiptError::Links { path: found } => assert_eq!(found, link),
            other => panic!("{other:?}"),
        }
        let refused = FilesError::Io {
            path: link.into(),
            source: denied(),
        };
        match links_error(refused, &path) {
            ReceiptError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, link);
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        match links_error(FilesError::NotRoot { name: "x".into() }, &path) {
            ReceiptError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, path);
                assert_eq!(source.to_string(), r#""x" is not a single staged root"#);
            }
            other => panic!("{other:?}"),
        }

        // A failed read keeps its kind too: nothing there, something that is no file and too much
        // in a file are the installation's fault; a refusal to read a file that is there is the
        // system's; and a link is a link.
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("receipt.json");
        fs::write(&file, b"{}").unwrap();
        let missing = dir.path().join("missing.json");
        let unfit = |detail: String| ReceiptError::Unreadable {
            detail: detail.into(),
        };
        for (what, error, at, shown) in [
            (
                "nothing there",
                FilesError::Io {
                    path: missing.clone(),
                    source: io::ErrorKind::NotFound.into(),
                },
                missing.clone(),
                format!("{}: ", missing.display()),
            ),
            (
                "a folder",
                FilesError::Io {
                    path: dir.path().to_path_buf(),
                    source: denied(),
                },
                dir.path().to_path_buf(),
                format!("{}: denied", dir.path().display()),
            ),
            (
                "too much",
                FilesError::TooLarge {
                    path: file.clone(),
                    limit: 1,
                },
                file.clone(),
                format!("Metadata is too large: {}", file.display()),
            ),
        ] {
            match read_error(error, &at, unfit) {
                ReceiptError::Unreadable { detail } => {
                    assert!(detail.starts_with(&shown), "{what}: {detail}");
                }
                other => panic!("{what}: {other:?}"),
            }
        }
        let refused = FilesError::Io {
            path: file.clone(),
            source: denied(),
        };
        match read_error(refused, &file, unfit) {
            ReceiptError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, file);
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        match read_error(FilesError::Link { path: link.into() }, &file, unfit) {
            ReceiptError::Links { path: found } => assert_eq!(found, link),
            other => panic!("{other:?}"),
        }

        assert!(size_of::<ReceiptError>() <= MOST_ERROR_BYTES);
    }

    #[test]
    fn launcher_copies_take_their_platforms_names() {
        assert_eq!(
            installed_launcher_name(Platform::WindowsX64),
            "GauntletDarkLegacy-Launcher.exe"
        );
        assert_eq!(
            installed_launcher_name(Platform::LinuxX64),
            "GauntletDarkLegacy-Launcher"
        );
        // Each is a name the launcher may write, and none is one of the alpha installer's.
        for platform in Platform::ALL {
            let name = installed_launcher_name(platform);
            check_component(name, &PAYLOAD_NAMES).unwrap();
            assert!(
                !ALPHA_NAMES
                    .iter()
                    .any(|alpha| alpha.eq_ignore_ascii_case(name)),
                "{name}"
            );
        }
    }
}
