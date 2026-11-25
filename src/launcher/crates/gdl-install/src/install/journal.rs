//! The journal of a fresh install, `.gdl-launcher/journal/install.json`: how far the install has
//! come, the folders it made, its staging and the roots it publishes, written whole at every step,
//! so that whatever stops the job, the next start knows what to finish or take back. From the
//! publish on, it carries the receipt the install writes, so that the receipt a recovery writes is
//! the one the job would have written. The next start reads it back to finish the install or take
//! it back.

use std::fs;
use std::path::{Path, PathBuf};

use gdl_files::{
    FilesError, Journal, MAX_JSON_LEN, RelPath, is_absent, pending_path, retry_locked,
    write_json_atomic,
};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use crate::compat::{JOURNAL_SCHEMA, MIN_LAUNCHER};
use crate::faults::{FaultPoint, FaultPoints};
use crate::receipt::Receipt;

/// The install's journal, in `.gdl-launcher/journal/`.
pub const INSTALL_JOURNAL: &str = "install.json";

/// How far an install has come, as its journal records it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum InstallPhase {
    /// The install makes its files in its staging; nothing is in place yet.
    Staging,
    /// The install puts its roots in place, and writes its receipt once they all are.
    Publishing,
    /// What the install put in place goes back into its staging, which then goes.
    RollingBack,
    /// The receipt is written and the installation stands; what is left is cleaning up.
    Committed,
}

/// What an install's journal holds. The keys a journal holds beyond these are kept, so that what a
/// later launcher adds survives this one's rewrite.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct InstallJournal {
    /// The journal's schema: [`JOURNAL_SCHEMA`].
    pub(crate) schema: u64,
    /// The lowest launcher version allowed to change the journal: [`MIN_LAUNCHER`].
    pub(crate) min_launcher: String,
    /// How far the install has come.
    pub(crate) phase: InstallPhase,
    /// How many folders the install made: the destination, when it was not there, and each
    /// missing folder above it, counted from the destination up.
    pub(crate) made_folders: u32,
    /// The staging's name, in the state folder.
    pub(crate) staging: String,
    /// Every root the publish puts in place, in byte order; none before the publish.
    pub(crate) roots: Vec<String>,
    /// The roots in place so far, in the order they moved.
    pub(crate) published: Vec<String>,
    /// The receipt the install writes once every root is in place, from the publish on.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub(crate) receipt: Option<Receipt>,
    /// The keys the journal holds beyond these, kept as they are.
    #[serde(flatten)]
    pub(crate) other: Map<String, Value>,
}

impl InstallJournal {
    /// The journal of an install that has made `made_folders` folders and the staging named
    /// `staging`, and is to make its files there.
    pub(crate) fn staging(made_folders: u32, staging: &str) -> InstallJournal {
        InstallJournal {
            schema: JOURNAL_SCHEMA,
            min_launcher: MIN_LAUNCHER.to_owned(),
            phase: InstallPhase::Staging,
            made_folders,
            staging: staging.to_owned(),
            roots: Vec::new(),
            published: Vec::new(),
            receipt: None,
            other: Map::new(),
        }
    }
}

/// The journal of an install under way, where it is written, and the points the job passes as it
/// records the publish. Every change writes the journal whole, through its pending file, so that a
/// reader finds the journal before the change or after it, never part of either. The caller holds
/// the installation's lock throughout.
pub(crate) struct JournalFile<'a> {
    /// The journal file.
    path: PathBuf,
    /// What it holds.
    journal: InstallJournal,
    /// Where the job passes its fault points.
    faults: &'a dyn FaultPoints,
}

impl<'a> JournalFile<'a> {
    /// Writes `journal` as the install's journal in the journal folder `folder`, and gives it back
    /// to be changed as the job goes on.
    pub(crate) fn begin(
        folder: &Path,
        journal: InstallJournal,
        faults: &'a dyn FaultPoints,
    ) -> Result<JournalFile<'a>, FilesError> {
        let file = JournalFile {
            path: folder.join(INSTALL_JOURNAL),
            journal,
            faults,
        };
        file.write()?;
        Ok(file)
    }

    /// The journal `journal`, as an install left it in the journal folder `folder`, given back to
    /// be changed as the install's recovery goes on. Nothing is written until it changes.
    pub(crate) fn resume(
        folder: &Path,
        journal: InstallJournal,
        faults: &'a dyn FaultPoints,
    ) -> JournalFile<'a> {
        JournalFile {
            path: folder.join(INSTALL_JOURNAL),
            journal,
            faults,
        }
    }

    /// The journal file.
    pub(crate) fn path(&self) -> &Path {
        &self.path
    }

    /// How far the install has come, as the journal records it.
    pub(crate) fn phase(&self) -> InstallPhase {
        self.journal.phase
    }

    /// How many folders the install made: the destination, when it was not there, and each
    /// missing folder above it.
    pub(crate) fn made_folders(&self) -> u32 {
        self.journal.made_folders
    }

    /// The name of the install's staging, in the state folder.
    pub(crate) fn staging_name(&self) -> &str {
        &self.journal.staging
    }

    /// Every root the publish puts in place, in byte order: none before the publish.
    pub(crate) fn roots(&self) -> &[String] {
        &self.journal.roots
    }

    /// The roots the publish has recorded in place, in the order they moved.
    pub(crate) fn published_roots(&self) -> &[String] {
        &self.journal.published
    }

    /// The receipt the install writes once every root is in place, from the publish on.
    pub(crate) fn receipt(&self) -> Option<&Receipt> {
        self.journal.receipt.as_ref()
    }

    /// Moves the journal to publishing, with every root of `roots` the publish puts in place, in
    /// byte order, and the receipt it writes once they all are.
    pub(crate) fn publishing(
        &mut self,
        roots: &[RelPath],
        receipt: &Receipt,
    ) -> Result<(), FilesError> {
        let mut roots: Vec<String> = roots.iter().map(|root| root.as_str().to_owned()).collect();
        roots.sort();
        self.journal.phase = InstallPhase::Publishing;
        self.journal.roots = roots;
        self.journal.published.clear();
        self.journal.receipt = Some(receipt.clone());
        self.write()
    }

    /// Moves the journal to rolling back: what the publish put in place goes back into the
    /// staging, which then goes, before the receipt is written. The roots and the receipt stay, so
    /// that whoever finishes the rollback knows every root to look for.
    pub(crate) fn rolling_back(&mut self) -> Result<(), FilesError> {
        self.journal.phase = InstallPhase::RollingBack;
        self.write()
    }

    /// Moves the journal to committed: the receipt is written, and only cleaning up is left.
    pub(crate) fn committed(&mut self) -> Result<(), FilesError> {
        self.journal.phase = InstallPhase::Committed;
        self.write()
    }

    /// Removes the journal file, then the pending file that an interrupted write of it left, each
    /// counted removed when it is already gone, as a cleanup a crash cut short leaves it. A file a
    /// scanner holds for a moment is waited for.
    pub(crate) fn remove(&self) -> Result<(), FilesError> {
        remove_if_there(&self.path)?;
        remove_if_there(&pending_path(&self.path))?;
        Ok(())
    }

    /// Writes the journal whole, to the bound its readers read journals to.
    fn write(&self) -> Result<(), FilesError> {
        write_json_atomic(&self.path, &self.journal, MAX_JSON_LEN)
    }
}

impl Journal for JournalFile<'_> {
    /// Passes the root's move, then records the root as in place.
    fn published(&mut self, root: &RelPath) -> Result<(), FilesError> {
        self.faults
            .pass(FaultPoint::Publish)
            .map_err(|source| FilesError::Io {
                path: self.path.clone(),
                source,
            })?;
        self.journal.published.push(root.as_str().to_owned());
        self.write()
    }
}

/// Removes the file at `path`, counting one already gone as removed, and says whether it was
/// there. A file a scanner holds for a moment is waited for.
pub(crate) fn remove_if_there(path: &Path) -> Result<bool, FilesError> {
    match retry_locked(|| fs::remove_file(path)) {
        Ok(()) => Ok(true),
        Err(error) if is_absent(&error) => Ok(false),
        Err(source) => Err(FilesError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs;
    use std::io;

    use gdl_files::{Journal, PAYLOAD_NAMES, RelPath, pending_path};
    use serde_json::{Map, Value, json};

    use super::{INSTALL_JOURNAL, InstallJournal, InstallPhase, JournalFile};
    use crate::faults::{FaultPoint, FaultPoints, NoFaults};
    use crate::receipt::{InstalledLauncher, Receipt};

    /// The journal of an install that made its destination and the staging `install-AbC123xy`,
    /// as the job writes it before it stages anything.
    const STAGING_JOURNAL: &str = r#"{
  "schema": 1,
  "minLauncher": "1.0.0-beta",
  "phase": "staging",
  "madeFolders": 1,
  "staging": "install-AbC123xy",
  "roots": [],
  "published": []
}
"#;

    /// A receipt as an install of Linux's runtime writes one.
    fn receipt() -> Receipt {
        Receipt {
            schema: 1,
            min_launcher: "1.0.0-beta".to_owned(),
            version: "1.0.0-beta".to_owned(),
            commit: "test-commit".to_owned(),
            platform: "linux-x64".to_owned(),
            disc: "GUNE5D".to_owned(),
            disc_revision: 1,
            build_info_sha256: "a".repeat(64),
            installed_digests: BTreeMap::new(),
            disc_icon: None,
            launcher: InstalledLauncher {
                name: "GauntletDarkLegacy-Launcher".to_owned(),
                sha256: "b".repeat(64),
                other: Map::new(),
            },
            other: Map::new(),
        }
    }

    fn root(name: &str) -> RelPath {
        RelPath::parse(name, &PAYLOAD_NAMES).unwrap()
    }

    /// The journal file at `folder`'s, as JSON.
    fn read(folder: &std::path::Path) -> Value {
        serde_json::from_slice(&fs::read(folder.join(INSTALL_JOURNAL)).unwrap()).unwrap()
    }

    /// Fault points that fail every pass of the publish.
    struct FailingPublish;

    impl FaultPoints for FailingPublish {
        fn pass(&self, point: FaultPoint) -> io::Result<()> {
            if point == FaultPoint::Publish {
                Err(io::Error::other("injected"))
            } else {
                Ok(())
            }
        }
    }

    #[test]
    fn a_staging_journal_is_written_as_its_format_gives_it() {
        let folder = tempfile::tempdir().unwrap();
        let journal = InstallJournal::staging(1, "install-AbC123xy");
        let file = JournalFile::begin(folder.path(), journal, &NoFaults).unwrap();
        assert_eq!(file.path(), folder.path().join(INSTALL_JOURNAL));
        assert_eq!(
            fs::read_to_string(folder.path().join(INSTALL_JOURNAL)).unwrap(),
            STAGING_JOURNAL
        );
        // Nothing but the journal is left: its pending file went into its place.
        assert_eq!(
            gdl_fixtures::folder_names(folder.path()).unwrap(),
            [INSTALL_JOURNAL]
        );
    }

    #[test]
    fn the_publish_lists_its_roots_records_each_move_and_carries_the_receipt() {
        let folder = tempfile::tempdir().unwrap();
        let journal = InstallJournal::staging(0, "install-AbC123xy");
        let mut file = JournalFile::begin(folder.path(), journal, &NoFaults).unwrap();
        let receipt = receipt();
        file.publishing(&[root("saves"), root("Gauntlet"), root("data")], &receipt)
            .unwrap();
        // The roots in byte order, then the receipt as its own object, indented one level more.
        let shown = serde_json::to_string_pretty(&receipt)
            .unwrap()
            .replace('\n', "\n  ");
        let expected = format!(
            "{{\n  \"schema\": 1,\n  \"minLauncher\": \"1.0.0-beta\",\n  \"phase\": \
             \"publishing\",\n  \"madeFolders\": 0,\n  \"staging\": \"install-AbC123xy\",\n  \
             \"roots\": [\n    \"Gauntlet\",\n    \"data\",\n    \"saves\"\n  ],\n  \
             \"published\": [],\n  \"receipt\": {shown}\n}}\n"
        );
        assert_eq!(
            fs::read_to_string(folder.path().join(INSTALL_JOURNAL)).unwrap(),
            expected
        );
        assert_eq!(
            read(folder.path())["receipt"],
            serde_json::to_value(&receipt).unwrap()
        );

        file.published(&root("Gauntlet")).unwrap();
        file.published(&root("data")).unwrap();
        assert_eq!(
            read(folder.path())["published"],
            json!(["Gauntlet", "data"])
        );
        file.committed().unwrap();
        let written = read(folder.path());
        assert_eq!(written["phase"], "committed");
        assert_eq!(written["published"], json!(["Gauntlet", "data"]));

        // A pending file a write left goes with the journal, and a journal already gone counts as
        // removed.
        fs::write(pending_path(file.path()), b"{").unwrap();
        file.remove().unwrap();
        assert!(
            gdl_fixtures::folder_names(folder.path())
                .unwrap()
                .is_empty()
        );
        file.remove().unwrap();
    }

    #[test]
    fn a_rollback_keeps_the_roots_and_the_receipt() {
        let folder = tempfile::tempdir().unwrap();
        let journal = InstallJournal::staging(2, "install-AbC123xy");
        let mut file = JournalFile::begin(folder.path(), journal, &NoFaults).unwrap();
        assert_eq!(file.phase(), InstallPhase::Staging);
        assert!(file.roots().is_empty());
        let receipt = receipt();
        file.publishing(&[root("saves"), root("Gauntlet")], &receipt)
            .unwrap();
        file.published(&root("Gauntlet")).unwrap();
        assert_eq!(file.phase(), InstallPhase::Publishing);
        assert_eq!(file.roots(), ["Gauntlet", "saves"]);

        file.rolling_back().unwrap();
        assert_eq!(file.phase(), InstallPhase::RollingBack);
        let written = read(folder.path());
        assert_eq!(written["phase"], "rollingBack");
        assert_eq!(written["madeFolders"], 2);
        assert_eq!(written["roots"], json!(["Gauntlet", "saves"]));
        assert_eq!(written["published"], json!(["Gauntlet"]));
        assert_eq!(written["receipt"], serde_json::to_value(&receipt).unwrap());
    }

    #[test]
    fn a_failed_pass_records_no_move() {
        let folder = tempfile::tempdir().unwrap();
        let journal = InstallJournal::staging(0, "install-AbC123xy");
        let mut file = JournalFile::begin(folder.path(), journal, &FailingPublish).unwrap();
        file.publishing(&[root("data")], &receipt()).unwrap();
        let refused = file.published(&root("data")).unwrap_err();
        assert_eq!(refused.rule(), "io", "{refused:?}");
        assert_eq!(read(folder.path())["published"], json!([]));
    }

    #[test]
    fn a_journal_taken_back_is_changed_where_the_install_left_it() {
        let folder = tempfile::tempdir().unwrap();
        let mut journal = InstallJournal::staging(2, "install-AbC123xy");
        journal.phase = InstallPhase::Publishing;
        journal.roots = vec!["Gauntlet".to_owned(), "data".to_owned()];
        journal.published = vec!["Gauntlet".to_owned()];
        journal.receipt = Some(receipt());
        journal.other.insert("future".to_owned(), json!({"x": 1}));
        let mut file = JournalFile::resume(folder.path(), journal, &NoFaults);
        // Nothing is written until the journal changes.
        assert!(
            gdl_fixtures::folder_names(folder.path())
                .unwrap()
                .is_empty()
        );
        assert_eq!(file.path(), folder.path().join(INSTALL_JOURNAL));
        assert_eq!(file.phase(), InstallPhase::Publishing);
        assert_eq!(file.made_folders(), 2);
        assert_eq!(file.staging_name(), "install-AbC123xy");
        assert_eq!(file.roots(), ["Gauntlet", "data"]);
        assert_eq!(file.published_roots(), ["Gauntlet"]);
        assert_eq!(file.receipt(), Some(&receipt()));

        // The next move is recorded after those before it, and what a later launcher added stays.
        file.published(&root("data")).unwrap();
        let written = read(folder.path());
        assert_eq!(written["published"], json!(["Gauntlet", "data"]));
        assert_eq!(written["madeFolders"], 2);
        assert_eq!(written["future"], json!({"x": 1}));
        assert_eq!(written["receipt"], serde_json::to_value(receipt()).unwrap());
    }

    #[test]
    fn a_journal_keeps_what_it_does_not_know() {
        let text = json!({
            "schema": 1,
            "minLauncher": "1.0.0-beta",
            "phase": "rollingBack",
            "madeFolders": 2,
            "staging": "install-AbC123xy",
            "roots": ["data"],
            "published": ["data"],
            "future": {"x": 1},
        });
        let journal: InstallJournal = serde_json::from_value(text.clone()).unwrap();
        assert_eq!(journal.phase, InstallPhase::RollingBack);
        assert_eq!(journal.made_folders, 2);
        assert_eq!(journal.receipt, None);
        assert_eq!(journal.other["future"], json!({"x": 1}));
        assert_eq!(serde_json::to_value(&journal).unwrap(), text);
        // Each phase is written by its name in the journal's format.
        for (phase, name) in [
            (InstallPhase::Staging, "staging"),
            (InstallPhase::Publishing, "publishing"),
            (InstallPhase::RollingBack, "rollingBack"),
            (InstallPhase::Committed, "committed"),
        ] {
            assert_eq!(serde_json::to_value(phase).unwrap(), name);
        }
    }
}
