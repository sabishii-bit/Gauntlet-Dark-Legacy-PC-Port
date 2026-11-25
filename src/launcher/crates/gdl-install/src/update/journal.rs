//! The journal of an update, `.gdl-launcher/journal/update.json`: the installed names it moves
//! aside, the new names it puts in place, the folders it makes for them, the launcher copy's
//! retired name and the receipt it writes. Every move is listed before the first is made, and the
//! journal is written whole as the update reaches each phase, never once a move, so that the next
//! start tells each move's state from where its file is.

use std::collections::BTreeSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{
    FilesError, MAX_JSON_LEN, PAYLOAD_NAMES, RelPath, is_absent, is_staging_name, retry_locked,
    write_json_atomic,
};
use gdl_release::{Platform, Version};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use super::UpdateError;
use crate::compat::{JOURNAL_SCHEMA, MIN_LAUNCHER};
use crate::payload::{MAX_PAYLOAD_ENTRIES, check_root};
use crate::receipt::{Receipt, installed_launcher_name};

/// The update's journal, in `.gdl-launcher/journal/`.
pub const UPDATE_JOURNAL: &str = "update.json";

/// The prefix of an update's staging folder in the state folder, which a random part follows.
pub const UPDATE_STAGING_PREFIX: &str = "stage-";

/// The prefix of the folder in the state folder that a release's download is made in, which a
/// random part follows.
pub const DOWNLOAD_PREFIX: &str = "download-";

/// The folder in the state folder that the installed files an update replaces or removes are
/// moved aside into, until the update is committed or taken back.
pub const UPDATE_BACKUP: &str = "update-backup";

/// The prefix of the name the installed launcher copy is moved aside to in the state folder,
/// which its version, a dash and the staging's random part follow.
pub const RETIRED_LAUNCHER_PREFIX: &str = "retired-launcher-";

/// How far an update has come, as its journal records it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum UpdatePhase {
    /// Everything the update puts in place is staged; nothing has moved.
    Staged,
    /// The installed files move aside and the new ones into place, then the receipt is written.
    Swapping,
    /// What moved goes back to where it was.
    RollingBack,
    /// The receipt is written and the new release stands; what is left is cleaning up.
    Committed,
}

/// What an update's journal holds. The keys a journal holds beyond these are kept, so that what a
/// later launcher adds survives this one's rewrite.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct UpdateJournal {
    /// The journal's schema: [`JOURNAL_SCHEMA`].
    pub(crate) schema: u64,
    /// The lowest launcher version allowed to change the journal: [`MIN_LAUNCHER`].
    pub(crate) min_launcher: String,
    /// How far the update has come.
    pub(crate) phase: UpdatePhase,
    /// The installed release's version.
    pub(crate) from: String,
    /// The new release's version.
    pub(crate) to: String,
    /// The staging's name, in the state folder.
    pub(crate) staging: String,
    /// The download folder's name, in the state folder, for an update from a release.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub(crate) download: Option<String>,
    /// The installed names moved aside into [`UPDATE_BACKUP`], in byte order.
    pub(crate) backup: Vec<String>,
    /// The new names put in place from the staging, in byte order.
    pub(crate) publish: Vec<String>,
    /// The folders under the installation's top that the publish makes, outermost first; none
    /// before the swap.
    pub(crate) made_folders: Vec<String>,
    /// The launcher copy's name at the installation's top.
    pub(crate) launcher: String,
    /// Whether no launcher copy was there to move aside.
    #[serde(default, skip_serializing_if = "is_false")]
    pub(crate) launcher_missing: bool,
    /// The name, in the state folder, the installed launcher copy is moved aside to.
    pub(crate) retired: String,
    /// The receipt the update writes once every move is made.
    pub(crate) receipt: Receipt,
    /// The keys the journal holds beyond these, kept as they are.
    #[serde(flatten)]
    pub(crate) other: Map<String, Value>,
}

/// Whether `value` is false, so that a flag that is not set is not written.
fn is_false(value: &bool) -> bool {
    !*value
}

impl UpdateJournal {
    /// The journal `object`, a journal of this launcher's kind whose stamp lets it be changed, held
    /// to what an update writes: its staging and download folders and its retired launcher copy
    /// named as an update names them, its launcher copy the one an installation for this platform
    /// keeps, its versions semantic versions, every name it moves a runtime's name, none twice in
    /// a list, every folder it makes one above a name it puts in place, and its receipt one of
    /// this platform's. Otherwise what is wrong, so that no journal can have a job move or remove
    /// the game's disc files, a player's file or anything outside the installation.
    pub(crate) fn from_object(object: &Map<String, Value>) -> Result<UpdateJournal, String> {
        let journal: UpdateJournal = serde_json::from_value(Value::Object(object.clone()))
            .map_err(|error| error.to_string())?;
        journal.check()?;
        Ok(journal)
    }

    /// Holds the journal to what an update writes, as [`from_object`](Self::from_object) says.
    fn check(&self) -> Result<(), String> {
        if !is_staging_name(UPDATE_STAGING_PREFIX, &self.staging) {
            return Err(format!("its staging {:?} is no update's", self.staging));
        }
        if let Some(download) = &self.download
            && !is_staging_name(DOWNLOAD_PREFIX, download)
        {
            return Err(format!("its download {download:?} is no update's"));
        }
        for (key, version) in [("from", &self.from), ("to", &self.to)] {
            Version::parse(version)
                .map_err(|_| format!("its {key} {version:?} is not a semantic version"))?;
        }
        let platform = Platform::of_this_build()
            .ok_or_else(|| "this launcher is built for no platform a release is".to_owned())?;
        let launcher = installed_launcher_name(platform);
        if self.launcher != launcher {
            return Err(format!(
                "its launcher {:?} is not {launcher}",
                self.launcher
            ));
        }
        let retired = retired_name(&self.from, &self.staging, platform);
        if retired.as_deref() != Some(self.retired.as_str()) {
            return Err(format!(
                "its retired copy {:?} is not the one the update names",
                self.retired
            ));
        }
        runtime_names("backup", &self.backup)?;
        let publish = runtime_names("publish", &self.publish)?;
        for folder in &self.made_folders {
            let above = format!("{folder}/");
            let made = RelPath::parse(folder, &PAYLOAD_NAMES).is_ok()
                && publish.iter().any(|name| name.as_str().starts_with(&above));
            if !made {
                return Err(format!(
                    "its made folder {folder:?} is above no name it puts in place"
                ));
            }
        }
        match self.receipt.checked_platform() {
            Ok(receipt) if receipt == platform => Ok(()),
            Ok(other) => Err(format!(
                "its receipt is of {}, not {}",
                other.as_str(),
                platform.as_str()
            )),
            Err(detail) => Err(format!("its receipt is no receipt: {detail}")),
        }
    }
}

/// The names of the journal's list `key`, `names`, each a runtime's name, none twice, and no more
/// than a runtime holds with its record.
fn runtime_names(key: &str, names: &[String]) -> Result<BTreeSet<RelPath>, String> {
    if names.len() > MAX_PAYLOAD_ENTRIES + 1 {
        return Err(format!("its {key} lists more than a runtime holds"));
    }
    let mut parsed = BTreeSet::new();
    for text in names {
        let name = RelPath::parse(text, &PAYLOAD_NAMES)
            .map_err(|error| format!("its {key} name {text:?}: {error}"))?;
        check_root(&name).map_err(|error| format!("its {key} name {text:?}: {error}"))?;
        if !parsed.insert(name) {
            return Err(format!("its {key} lists {text:?} twice"));
        }
    }
    Ok(parsed)
}

/// The name, in the state folder, an update from the release of version `from`, staged in the
/// staging folder `staging`, moves the installed launcher copy for `platform` aside to: the
/// version and the staging's random part, with Windows' suffix on Windows; `None` when `staging`
/// is no update's staging.
pub(crate) fn retired_name(from: &str, staging: &str, platform: Platform) -> Option<String> {
    let random = staging
        .strip_prefix(UPDATE_STAGING_PREFIX)
        .filter(|_| is_staging_name(UPDATE_STAGING_PREFIX, staging))?;
    let suffix = match platform {
        Platform::WindowsX64 => ".exe",
        Platform::LinuxX64 => "",
    };
    Some(format!("{RETIRED_LAUNCHER_PREFIX}{from}-{random}{suffix}"))
}

/// A staging's name of the length an update's has, which stands for the one an update makes
/// while the journal it would write is judged by its size, before anything is staged.
pub(super) const PLANNED_STAGING: &str = "stage-00000000";

/// Refuses `journal` as [`UpdateError::JournalTooLarge`] when its JSON, as it is written, would
/// take more than [`MAX_JSON_LEN`] bytes, the bound its readers read journals to.
pub(super) fn check_journal_len(journal: &UpdateJournal) -> Result<(), UpdateError> {
    let too_large = UpdateError::JournalTooLarge {
        limit: MAX_JSON_LEN,
    };
    let json = serde_json::to_vec_pretty(journal).map_err(|_| too_large)?;
    // As the journal is written: its JSON, then a newline.
    let len = u64::try_from(json.len())
        .unwrap_or(u64::MAX)
        .saturating_add(1);
    if len > MAX_JSON_LEN {
        return Err(UpdateError::JournalTooLarge {
            limit: MAX_JSON_LEN,
        });
    }
    Ok(())
}

/// The journal of an update under way and where it is written. Every change writes it whole,
/// through its pending file, so that a reader finds the journal before the change or after it,
/// never part of either. The caller holds the installation's lock throughout.
pub(super) struct JournalFile {
    /// The journal file.
    path: PathBuf,
    /// What it holds.
    journal: UpdateJournal,
}

impl JournalFile {
    /// Writes `journal` as the update's journal in the journal folder `folder`, which is made when
    /// it is not there, and gives it back to be changed as the update goes on.
    pub(super) fn begin(folder: &Path, journal: UpdateJournal) -> Result<JournalFile, UpdateError> {
        match fs::create_dir(folder) {
            Ok(()) => {}
            Err(error)
                if error.kind() == io::ErrorKind::AlreadyExists
                    && fs::symlink_metadata(folder).is_ok_and(|entry| entry.is_dir()) => {}
            Err(source) => {
                return Err(UpdateError::Io {
                    path: folder.to_path_buf(),
                    source,
                });
            }
        }
        let file = JournalFile {
            path: folder.join(UPDATE_JOURNAL),
            journal,
        };
        file.write()?;
        Ok(file)
    }

    /// The journal file.
    pub(super) fn path(&self) -> &Path {
        &self.path
    }

    /// What the journal holds.
    pub(super) fn journal(&self) -> &UpdateJournal {
        &self.journal
    }

    /// Moves the journal to swapping, with the folders the publish makes, outermost first.
    pub(super) fn swapping(&mut self, made_folders: Vec<String>) -> Result<(), UpdateError> {
        self.journal.phase = UpdatePhase::Swapping;
        self.journal.made_folders = made_folders;
        self.write()
    }

    /// Moves the journal to committed: the receipt is written, and only cleaning up is left.
    pub(super) fn committed(&mut self) -> Result<(), UpdateError> {
        self.journal.phase = UpdatePhase::Committed;
        self.write()
    }

    /// Removes the journal file, counted removed when it is already gone; the pending file an
    /// interrupted write of it left is the caller's, in the order its step removes them. A file a
    /// scanner holds for a moment is waited for.
    pub(super) fn remove(&self) -> Result<(), FilesError> {
        remove_if_there(&self.path)
    }

    /// Writes the journal whole, to the bound its readers read journals to: a journal that would
    /// pass it is [`UpdateError::JournalTooLarge`], refused before anything is written.
    fn write(&self) -> Result<(), UpdateError> {
        write_json_atomic(&self.path, &self.journal, MAX_JSON_LEN).map_err(|error| match error {
            FilesError::TooLarge { limit, .. } => UpdateError::JournalTooLarge { limit },
            other => UpdateError::Files(other),
        })
    }
}

/// Removes the file at `path`, counting one already gone as removed. A file a scanner holds for a
/// moment is waited for.
pub(super) fn remove_if_there(path: &Path) -> Result<(), FilesError> {
    match retry_locked(|| fs::remove_file(path)) {
        Ok(()) => Ok(()),
        Err(error) if is_absent(&error) => Ok(()),
        Err(source) => Err(FilesError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// The journal an update of the release `from` to `to`, staged in `staging`, writes once it is
/// staged: the names it moves aside and puts in place, the launcher copy for `platform`, whether
/// none was there to move aside, and the receipt it writes.
pub(super) struct Planned<'a> {
    /// The installed release's version, as its receipt writes it.
    pub(super) from: &'a str,
    /// The new release's version, as its record writes it.
    pub(super) to: &'a str,
    /// The staging's name.
    pub(super) staging: &'a str,
    /// The installed names moved aside, in byte order.
    pub(super) backup: &'a [RelPath],
    /// The new names put in place, in byte order.
    pub(super) publish: &'a [RelPath],
    /// The platform the installation is for.
    pub(super) platform: Platform,
    /// Whether no launcher copy was there to move aside.
    pub(super) launcher_missing: bool,
    /// The receipt the update writes.
    pub(super) receipt: &'a Receipt,
}

impl Planned<'_> {
    /// The journal of the update planned, at [`UpdatePhase::Staged`].
    pub(super) fn staged(&self) -> UpdateJournal {
        let texts = |names: &[RelPath]| -> Vec<String> {
            names.iter().map(|name| name.as_str().to_owned()).collect()
        };
        UpdateJournal {
            schema: JOURNAL_SCHEMA,
            min_launcher: MIN_LAUNCHER.to_owned(),
            phase: UpdatePhase::Staged,
            from: self.from.to_owned(),
            to: self.to.to_owned(),
            staging: self.staging.to_owned(),
            download: None,
            backup: texts(self.backup),
            publish: texts(self.publish),
            made_folders: Vec::new(),
            launcher: installed_launcher_name(self.platform).to_owned(),
            launcher_missing: self.launcher_missing,
            retired: retired_name(self.from, self.staging, self.platform)
                .expect("an update's staging is named as one"),
            receipt: self.receipt.clone(),
            other: Map::new(),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs;
    use std::path::Path;

    use gdl_files::{FilesError, MAX_JSON_LEN, PAYLOAD_NAMES, RelPath, read_json_bounded};
    use gdl_fixtures::{RuntimeFiles, sha256_hex};
    use gdl_release::Platform;
    use serde_json::{Map, Value, json};

    use super::{
        JournalFile, PLANNED_STAGING, Planned, UPDATE_JOURNAL, UpdateJournal, UpdatePhase,
        check_journal_len,
    };
    use crate::receipt::{DiscIcon, GUNE5D, InstalledLauncher, Receipt, installed_launcher_name};
    use crate::test_platform::{other_platform, this_platform};
    use crate::update::UpdateError;

    /// A change a test makes to a sound journal.
    type Change<'a> = dyn Fn(&mut UpdateJournal) + 'a;

    /// The staging the tests' journals name.
    const STAGING: &str = "stage-AbC123xy";

    /// What the tests' new launcher copy holds.
    const LAUNCHER_BYTES: &[u8] = b"the new launcher";

    /// `texts`, each as a name.
    fn names(texts: &[&str]) -> Vec<RelPath> {
        texts
            .iter()
            .map(|text| RelPath::parse(text, &PAYLOAD_NAMES).unwrap())
            .collect()
    }

    /// The receipt an update of the standard runtime to its next, of `1.0.0-beta.1`, writes for
    /// `platform`: the Windows one records its executable's digest and icon.
    fn receipt_for(platform: Platform) -> Receipt {
        let next = RuntimeFiles::standard(platform.as_str(), "1.0.0-beta").next("1.0.0-beta.1");
        let (installed_digests, disc_icon) = match platform {
            Platform::WindowsX64 => (
                BTreeMap::from([("gauntlet.exe".to_owned(), sha256_hex(b"personalised"))]),
                Some(DiscIcon {
                    path: "carddemo/icon.tpl".to_owned(),
                    sha256: sha256_hex(b"icon"),
                    release_executable_sha256: sha256_hex(b"not a real executable"),
                    other: Map::new(),
                }),
            ),
            Platform::LinuxX64 => (BTreeMap::new(), None),
        };
        Receipt {
            schema: 1,
            min_launcher: "1.0.0-beta".to_owned(),
            version: "1.0.0-beta.1".to_owned(),
            commit: "next-commit".to_owned(),
            platform: platform.as_str().to_owned(),
            disc: GUNE5D.to_owned(),
            disc_revision: 1,
            build_info_sha256: sha256_hex(&next.build_info_bytes()),
            installed_digests,
            disc_icon,
            launcher: InstalledLauncher {
                name: installed_launcher_name(platform).to_owned(),
                sha256: sha256_hex(LAUNCHER_BYTES),
                other: Map::new(),
            },
            other: Map::new(),
        }
    }

    /// The journal at staged of an update of the standard runtime to its next for `platform`,
    /// staged in [`STAGING`].
    fn staged_for(platform: Platform) -> UpdateJournal {
        let receipt = receipt_for(platform);
        let backup = names(&[
            "VERSION",
            "build-info.json",
            "data/text/en.json",
            "shaders/test.spv",
        ]);
        let publish = names(&[
            "VERSION",
            "build-info.json",
            "data/text/en.json",
            "shaders/next.spv",
        ]);
        Planned {
            from: "1.0.0-beta",
            to: "1.0.0-beta.1",
            staging: STAGING,
            backup: &backup,
            publish: &publish,
            platform,
            launcher_missing: false,
            receipt: &receipt,
        }
        .staged()
    }

    /// `journal` as the object a journal file holds.
    fn object_of(journal: &UpdateJournal) -> Map<String, Value> {
        match serde_json::to_value(journal).unwrap() {
            Value::Object(object) => object,
            other => panic!("{other}"),
        }
    }

    /// The journal the tests expect for Linux, with the two digests filled in.
    fn expected_linux_journal(build_info: &str, launcher: &str) -> String {
        [
            "{",
            "  \"schema\": 1,",
            "  \"minLauncher\": \"1.0.0-beta\",",
            "  \"phase\": \"staged\",",
            "  \"from\": \"1.0.0-beta\",",
            "  \"to\": \"1.0.0-beta.1\",",
            "  \"staging\": \"stage-AbC123xy\",",
            "  \"backup\": [",
            "    \"VERSION\",",
            "    \"build-info.json\",",
            "    \"data/text/en.json\",",
            "    \"shaders/test.spv\"",
            "  ],",
            "  \"publish\": [",
            "    \"VERSION\",",
            "    \"build-info.json\",",
            "    \"data/text/en.json\",",
            "    \"shaders/next.spv\"",
            "  ],",
            "  \"madeFolders\": [],",
            "  \"launcher\": \"GauntletDarkLegacy-Launcher\",",
            "  \"retired\": \"retired-launcher-1.0.0-beta-AbC123xy\",",
            "  \"receipt\": {",
            "    \"schema\": 1,",
            "    \"minLauncher\": \"1.0.0-beta\",",
            "    \"version\": \"1.0.0-beta.1\",",
            "    \"commit\": \"next-commit\",",
            "    \"platform\": \"linux-x64\",",
            "    \"disc\": \"GUNE5D\",",
            "    \"discRevision\": 1,",
            &format!("    \"buildInfoSha256\": \"{build_info}\","),
            "    \"installedDigests\": {},",
            "    \"launcher\": {",
            "      \"name\": \"GauntletDarkLegacy-Launcher\",",
            &format!("      \"sha256\": \"{launcher}\""),
            "    }",
            "  }",
            "}",
            "",
        ]
        .join("\n")
    }

    #[test]
    fn the_journal_is_byte_for_byte() {
        let dir = tempfile::tempdir().unwrap();
        let journals = dir.path().join("journal");
        let journal = staged_for(Platform::LinuxX64);
        let file = JournalFile::begin(&journals, journal).unwrap();
        assert_eq!(file.path(), journals.join(UPDATE_JOURNAL));
        let next = RuntimeFiles::standard("linux-x64", "1.0.0-beta").next("1.0.0-beta.1");
        let expected = expected_linux_journal(
            &sha256_hex(&next.build_info_bytes()),
            &sha256_hex(LAUNCHER_BYTES),
        );
        assert_eq!(
            String::from_utf8(fs::read(file.path()).unwrap()).unwrap(),
            expected
        );

        // On Windows the copies take the system's suffix; a launcher copy that was not there is
        // recorded, where one that was is not.
        let mut journal = staged_for(Platform::WindowsX64);
        assert_eq!(journal.launcher, "GauntletDarkLegacy-Launcher.exe");
        assert_eq!(journal.retired, "retired-launcher-1.0.0-beta-AbC123xy.exe");
        let written = serde_json::to_string(&journal).unwrap();
        assert!(!written.contains("launcherMissing"), "{written}");
        journal.launcher_missing = true;
        let written = serde_json::to_string(&journal).unwrap();
        assert!(written.contains("\"launcherMissing\":true"), "{written}");
    }

    #[test]
    fn a_journal_cannot_claim_the_game_or_a_player_file() {
        let platform = this_platform();
        let sound = staged_for(platform);
        assert_eq!(
            UpdateJournal::from_object(&object_of(&sound)),
            Ok(sound.clone())
        );

        let claiming = |change: &dyn Fn(&mut UpdateJournal)| {
            let mut journal = sound.clone();
            change(&mut journal);
            UpdateJournal::from_object(&object_of(&journal))
        };
        for name in [
            "saves/hero.json",
            "../x",
            "/abs",
            "GauntletDarkLegacy-Update.exe",
            "installation.json",
            "GauntletDarkLegacy-Launcher.exe",
            "Gauntlet/PDATA/WAR.WAD",
            "carddemo/icon.tpl",
        ] {
            let in_backup = claiming(&|journal| journal.backup.push(name.to_owned()));
            assert!(in_backup.is_err(), "backup {name}: {in_backup:?}");
            let in_publish = claiming(&|journal| journal.publish.push(name.to_owned()));
            assert!(in_publish.is_err(), "publish {name}: {in_publish:?}");
        }
        let refusals: [(&str, &Change<'_>); 10] = [
            ("a name twice", &|journal| {
                journal.publish.push("VERSION".to_owned());
            }),
            ("the game as the retired copy", &|journal| {
                journal.retired = platform.executable().to_owned();
            }),
            ("another version's retired copy", &|journal| {
                journal.retired = journal.retired.replace("1.0.0-beta", "0.9.0");
            }),
            ("the game as the launcher copy", &|journal| {
                journal.launcher = "gauntlet.exe".to_owned();
            }),
            ("a staging of another job", &|journal| {
                journal.staging = "install-AbC123xy".to_owned();
            }),
            ("a download of another name", &|journal| {
                journal.download = Some("downloads".to_owned());
            }),
            ("a version that is none", &|journal| {
                journal.to = "next".to_owned();
            }),
            ("a made folder above no new name", &|journal| {
                journal.made_folders = vec!["saves".to_owned()];
            }),
            ("a receipt of another platform", &|journal| {
                let other = other_platform();
                journal.receipt.platform = other.as_str().to_owned();
                journal.receipt.launcher.name = installed_launcher_name(other).to_owned();
            }),
            ("a receipt with no digest", &|journal| {
                journal.receipt.build_info_sha256 = "nothing".to_owned();
            }),
        ];
        for (what, change) in refusals {
            let refused = claiming(change);
            assert!(refused.is_err(), "{what}: {refused:?}");
        }
        // What the journal may hold of its own passes.
        let made = claiming(&|journal| {
            journal.download = Some("download-Qw3rTy12".to_owned());
            journal.made_folders = vec!["shaders".to_owned()];
        });
        assert!(made.is_ok(), "{made:?}");
    }

    #[test]
    fn unknown_keys_are_kept_through_each_phase() {
        let dir = tempfile::tempdir().unwrap();
        let mut object = object_of(&staged_for(this_platform()));
        object.insert("later".to_owned(), json!({"kept": [1, 2]}));
        object["receipt"]["laterReceipt"] = json!("kept too");
        let journal = UpdateJournal::from_object(&object).unwrap();
        let read =
            |path: &Path| -> Map<String, Value> { read_json_bounded(path, MAX_JSON_LEN).unwrap() };
        let mut file = JournalFile::begin(dir.path(), journal).unwrap();
        let written = read(file.path());
        assert_eq!(written["phase"], json!("staged"));
        assert_eq!(written["later"], json!({"kept": [1, 2]}));
        assert_eq!(written["receipt"]["laterReceipt"], json!("kept too"));
        file.swapping(vec!["shaders".to_owned()]).unwrap();
        let written = read(file.path());
        assert_eq!(written["phase"], json!("swapping"));
        assert_eq!(written["madeFolders"], json!(["shaders"]));
        assert_eq!(written["later"], json!({"kept": [1, 2]}));
        file.committed().unwrap();
        let written = read(file.path());
        assert_eq!(written["phase"], json!("committed"));
        assert_eq!(written["later"], json!({"kept": [1, 2]}));
        assert_eq!(written["receipt"]["laterReceipt"], json!("kept too"));
        assert_eq!(
            UpdateJournal::from_object(&written).unwrap().phase,
            UpdatePhase::Committed
        );
        file.remove().unwrap();
        assert!(!file.path().exists());
        // Removing it again counts as removed.
        file.remove().unwrap();
    }

    #[test]
    fn a_journal_past_its_bound_is_refused_before_it_is_written() {
        let mut journal = staged_for(this_platform());
        assert!(check_journal_len(&journal).is_ok());
        let long = "x".repeat(200);
        let names: Vec<String> = (0..12_000)
            .map(|number| format!("shaders/{long}/{number}.spv"))
            .collect();
        journal.backup = names.clone();
        journal.publish = names;
        match check_journal_len(&journal) {
            Err(UpdateError::JournalTooLarge { limit }) => assert_eq!(limit, MAX_JSON_LEN),
            other => panic!("{other:?}"),
        }
        // The file itself is held to the same bound, and nothing is written.
        let dir = tempfile::tempdir().unwrap();
        match JournalFile::begin(dir.path(), journal) {
            Err(UpdateError::JournalTooLarge { limit }) => assert_eq!(limit, MAX_JSON_LEN),
            Err(other) => panic!("{other:?}"),
            Ok(_) => panic!("a journal past its bound was written"),
        }
        assert!(!dir.path().join(UPDATE_JOURNAL).exists());
        // The stand-in staging is as long as a staging's name.
        assert_eq!(PLANNED_STAGING.len(), STAGING.len());
    }

    #[test]
    fn a_journal_folder_that_is_a_file_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("journal");
        fs::write(&folder, b"not a folder").unwrap();
        match JournalFile::begin(&folder, staged_for(this_platform())) {
            Err(UpdateError::Io { path, .. }) => assert_eq!(path, folder),
            Err(UpdateError::Files(FilesError::Io { .. })) => {}
            Err(other) => panic!("{other:?}"),
            Ok(_) => panic!("a journal was written into a file"),
        }
        assert_eq!(fs::read(&folder).unwrap(), b"not a folder");
    }
}
