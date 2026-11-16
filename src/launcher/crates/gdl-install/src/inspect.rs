//! What a folder is to the launcher, as it reports it before choosing what to do there: nothing
//! yet, a folder it cannot use, an alpha installation, an empty or foreign folder, a job left
//! unfinished, or an installation of its own and how its release stands against the launcher that
//! looks. A folder is only looked at: nothing in it is made, changed or locked, no file of the
//! alpha installer's is opened, and whatever goes wrong is reported as the folder's state.

use std::cmp::Ordering;
use std::ffi::{OsStr, OsString};
use std::fmt;
use std::fs;
use std::io;
use std::path::Path;

use gdl_files::{
    FilesError, MAX_JSON_LEN, PENDING_SUFFIX, absolute_lexical, is_absent, read_json_bounded,
    strip_verbatim,
};
use gdl_release::{Platform, Version};
use serde_json::{Map, Value};

use crate::compat::{Access, JOURNAL_NAMES, JOURNAL_SCHEMA, access, launcher_state_name, stamp_of};
use crate::folder::{
    FolderError, InstallFolder, JOURNAL_DIR, RECEIPT_FILE, markers_among, names_in,
};
use crate::receipt::{InstalledGame, ReceiptError};

/// The schema of the report an inspection gives the programs that read it.
pub const INSPECTION_SCHEMA: u32 = 1;

/// What an inspection found at a folder.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Inspection {
    /// The folder as the launcher takes it, absolute and folded by name, shown without a verbatim
    /// prefix.
    pub path: String,
    /// What the folder is to the launcher that looked.
    pub state: FolderState,
    /// The text of the refusal that decided the state, as technical detail for logs; `None` when
    /// no refusal decided it.
    pub detail: Option<String>,
    /// The alpha installer's names at the folder's top, as [`crate::alpha_markers`] gives them,
    /// in every state; none when the folder could not be listed.
    pub alpha_markers: Vec<&'static str>,
    /// The journals found in `.gdl-launcher/journal/`, by name, sorted, without the pending files
    /// that interrupted writes leave.
    pub journals: Vec<String>,
    /// The names directly under `.gdl-launcher/` that this launcher's rules do not make, sorted: a
    /// later launcher's or the player's, which this launcher leaves alone.
    pub unknown_names: Vec<String>,
    /// The installation, in every state, whenever its receipt and the files it pins read as one.
    pub installation: Option<InstallationSummary>,
}

/// What a folder is to the launcher that inspects it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FolderState {
    /// Nothing is at the path.
    Absent,
    /// Something other than a directory is at the path.
    NotDirectory,
    /// The folder, a folder above it, or an entry of the launcher's state in it is a link.
    Links,
    /// The system refused to let the folder, or the launcher's state in it, be looked at or read.
    Unreadable,
    /// A journal of one of this launcher's kinds cannot be read as one. It is left for the
    /// launcher that wrote it.
    JournalUnreadable,
    /// The folder holds an alpha installation, known by its names.
    AlphaInstall,
    /// The folder holds nothing.
    Empty,
    /// The folder holds something, but no installation, job or alpha installation.
    NotInstalled,
    /// A job of this launcher's was left unfinished, to be finished or rolled back.
    Pending,
    /// An installation of the launcher's whose receipt and files agree, and which this launcher
    /// may change.
    Installed,
    /// The receipt cannot be read, or disagrees with the files it pins.
    ReceiptInvalid,
    /// The installation is for another platform than the one this launcher was built for.
    OtherPlatform,
    /// The installation's executable is not there.
    GameMissing,
    /// A later launcher wrote the installation's receipt or a journal, or left a journal of a kind
    /// this launcher does not know: the game stays playable, and this launcher changes nothing.
    NewerLauncher,
}

impl FolderState {
    /// The state's name in the launcher's reports, in camelCase.
    pub fn as_str(self) -> &'static str {
        match self {
            FolderState::Absent => "absent",
            FolderState::NotDirectory => "notDirectory",
            FolderState::Links => "links",
            FolderState::Unreadable => "unreadable",
            FolderState::JournalUnreadable => "journalUnreadable",
            FolderState::AlphaInstall => "alphaInstall",
            FolderState::Empty => "empty",
            FolderState::NotInstalled => "notInstalled",
            FolderState::Pending => "pending",
            FolderState::Installed => "installed",
            FolderState::ReceiptInvalid => "receiptInvalid",
            FolderState::OtherPlatform => "otherPlatform",
            FolderState::GameMissing => "gameMissing",
            FolderState::NewerLauncher => "newerLauncher",
        }
    }
}

/// What an inspection tells of the installation in a folder.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InstallationSummary {
    /// The installed release's version, as its `build-info.json` records it.
    pub version: String,
    /// The commit the release was built from.
    pub commit: String,
    /// The platform the release was built for.
    pub platform: Platform,
    /// The disc the game's data was copied from, as the receipt records it; `None` when a later
    /// launcher wrote the receipt, which this launcher does not interpret.
    pub disc: Option<String>,
    /// That disc's revision; `None` when a later launcher wrote the receipt.
    pub disc_revision: Option<u8>,
    /// The version of the launcher that inspected the folder.
    pub launcher: String,
    /// How that launcher stands against the installed release.
    pub relation: Relation,
    /// What that launcher may do with the installation.
    pub access: Access,
}

/// How the launcher that inspects a folder stands against the release installed there, by
/// precedence, so that build metadata alone makes neither the newer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Relation {
    /// The launcher is older than the installed release.
    Older,
    /// The launcher is of the installed release.
    Same,
    /// The launcher is newer than the installed release, which it may update.
    Newer,
}

impl InstallFolder {
    /// Reports what the folder at `root` is to the launcher of version `running`, the folder taken
    /// absolute and folded by name as [`InstallFolder::at`] takes it. The state is the first of
    /// these that holds:
    ///
    /// - nothing is at the path, whatever lies above it, which leaves a folder yet to be made to
    ///   the job that would make it: [`FolderState::Absent`];
    /// - the folder is refused as [`InstallFolder::at`] refuses it, or cannot be listed;
    /// - the journals in `.gdl-launcher/journal/`, pending files passed over: one of this
    ///   launcher's kinds that cannot be read gives [`FolderState::JournalUnreadable`], or the
    ///   state of a link or of the system's refusal when that is why; else one that a later
    ///   launcher wrote, or of a kind this launcher does not know, gives
    ///   [`FolderState::NewerLauncher`]; else any gives [`FolderState::Pending`];
    /// - a receipt, read through [`InstallFolder::installed`];
    /// - the alpha installer's names, then nothing in the folder, then anything else.
    ///
    /// Nothing is made or changed, the installation's lock is not taken, so the inspection runs
    /// beside a job, and no entry the alpha installer named is opened. A failure is the folder's
    /// state, never an error.
    pub fn inspect(root: &Path, running: &Version) -> Inspection {
        let root = match absolute_lexical(root) {
            Ok(absolute) => absolute,
            Err(error) => return Inspection::decided(root, Verdict::refused(root, error)),
        };
        let folder = match folder_at(&root) {
            Ok(folder) => folder,
            Err(verdict) => return Inspection::decided(&root, verdict),
        };
        let listing = Listing::of(&root);
        let state = StateFolder::of(&folder, running);
        let (receipt, installation) = read_receipt(&folder, running);
        let (verdict, markers) = match listing {
            Ok(listing) => {
                let verdict = state
                    .verdict
                    .or(receipt)
                    .unwrap_or_else(|| Verdict::of(listing.state()));
                (verdict, listing.alpha_markers)
            }
            Err(verdict) => (verdict, Vec::new()),
        };
        Inspection {
            path: shown(&root),
            state: verdict.state,
            detail: verdict.detail,
            alpha_markers: markers,
            journals: state.journals,
            unknown_names: state.unknown_names,
            installation,
        }
    }
}

impl Inspection {
    /// The inspection of the folder at `path` whose state `verdict` decided before anything in
    /// the folder could be looked at.
    fn decided(path: &Path, verdict: Verdict) -> Inspection {
        Inspection {
            path: shown(path),
            state: verdict.state,
            detail: verdict.detail,
            alpha_markers: Vec::new(),
            journals: Vec::new(),
            unknown_names: Vec::new(),
            installation: None,
        }
    }
}

impl InstallationSummary {
    /// What an inspection tells of `game` for the launcher of version `running`. The release is
    /// named by its `build-info.json`, which a later launcher's receipt leaves as the release
    /// shipped it.
    fn of(game: &InstalledGame, running: &Version) -> InstallationSummary {
        let build_info = &game.build_info;
        let receipt = game.receipt.as_ref();
        InstallationSummary {
            version: build_info.version.clone(),
            commit: build_info.commit.clone(),
            platform: build_info.platform,
            disc: receipt.map(|receipt| receipt.disc.clone()),
            disc_revision: receipt.map(|receipt| receipt.disc_revision),
            launcher: running.to_string(),
            relation: Relation::of(running, &build_info.version()),
            access: game.access,
        }
    }
}

impl Relation {
    /// How the launcher of version `running` stands against a release of version `installed`.
    fn of(running: &Version, installed: &Version) -> Relation {
        match running.cmp(installed) {
            Ordering::Less => Relation::Older,
            Ordering::Equal => Relation::Same,
            Ordering::Greater => Relation::Newer,
        }
    }
}

/// A folder's state, with the text of the refusal that decided it when one did.
#[derive(Debug)]
struct Verdict {
    /// The state.
    state: FolderState,
    /// The refusal's text.
    detail: Option<String>,
}

impl Verdict {
    /// A state that no refusal decided.
    fn of(state: FolderState) -> Verdict {
        Verdict {
            state,
            detail: None,
        }
    }

    /// A state that `refusal` decided.
    fn by(state: FolderState, refusal: impl fmt::Display) -> Verdict {
        Verdict {
            state,
            detail: Some(refusal.to_string()),
        }
    }

    /// The system's refusal, for `error`, to look at or read `path`.
    fn refused(path: &Path, error: impl fmt::Display) -> Verdict {
        Verdict::by(
            FolderState::Unreadable,
            format_args!("{}: {error}", path.display()),
        )
    }

    /// The refusal of `path`, a link.
    fn link(path: &Path) -> Verdict {
        Verdict::by(
            FolderState::Links,
            FilesError::Link {
                path: path.to_path_buf(),
            },
        )
    }
}

/// The folder at `root`, an absolute path folded by name, once it is a directory with no link on
/// its path; otherwise the state that keeps it from being looked into. Nothing at the path is
/// judged before the links above it.
fn folder_at(root: &Path) -> Result<InstallFolder, Verdict> {
    if matches!(fs::symlink_metadata(root), Err(error) if is_absent(&error)) {
        return Err(Verdict::of(FolderState::Absent));
    }
    InstallFolder::at(root).map_err(|error| {
        let state = match error {
            FolderError::Links { .. } => FolderState::Links,
            FolderError::NotDirectory { .. } => FolderState::NotDirectory,
            // Gone since it was looked at.
            FolderError::Absent { .. } => return Verdict::of(FolderState::Absent),
            FolderError::Io { .. } => FolderState::Unreadable,
        };
        Verdict::by(state, error)
    })
}

/// What a folder's top shows of the folder's state, listed without opening any entry.
struct Listing {
    /// The alpha installer's names among the entries.
    alpha_markers: Vec<&'static str>,
    /// Whether the folder holds nothing at all.
    empty: bool,
}

impl Listing {
    /// Lists the folder at `root`, once, so that its markers and whether it holds anything are
    /// of the same moment.
    fn of(root: &Path) -> Result<Listing, Verdict> {
        let names = names_in(root).map_err(|error| Verdict::refused(root, error))?;
        Ok(Listing {
            alpha_markers: markers_among(&names),
            empty: names.is_empty(),
        })
    }

    /// The state of the folder when neither a journal nor a receipt decides it.
    fn state(&self) -> FolderState {
        if !self.alpha_markers.is_empty() {
            FolderState::AlphaInstall
        } else if self.empty {
            FolderState::Empty
        } else {
            FolderState::NotInstalled
        }
    }
}

/// What an installation's state folder shows: the names in it that are not the launcher's, the
/// journals of the jobs under way, and the state those decide when they decide one.
#[derive(Default)]
struct StateFolder {
    /// The names this launcher's rules do not make, sorted.
    unknown_names: Vec<String>,
    /// The journals found, sorted.
    journals: Vec<String>,
    /// The state the journals decide.
    verdict: Option<Verdict>,
}

impl StateFolder {
    /// Looks into `folder`'s state folder for the launcher of version `running`, judging each
    /// journal by its stamp.
    fn of(folder: &InstallFolder, running: &Version) -> StateFolder {
        let mut found = StateFolder::default();
        let state = folder.state_dir();
        let names = match names_at(&state) {
            Ok(Names::Listed(names)) => names,
            // Without a state folder there is no state of the launcher's to judge.
            Ok(Names::Missing | Names::NotFolder) => return found,
            Err(verdict) => return found.decided(verdict),
        };
        found.unknown_names = names
            .iter()
            .filter(|name| name.to_str().and_then(launcher_state_name).is_none())
            .map(|name| shown_name(name))
            .collect();
        let dir = state.join(JOURNAL_DIR);
        let names = match names_at(&dir) {
            Ok(Names::Listed(names)) => names,
            Ok(Names::Missing) => return found,
            Ok(Names::NotFolder) => {
                let not_folder = io::Error::from(io::ErrorKind::NotADirectory);
                let detail = format!("{}: {not_folder}", dir.display());
                return found.decided(Verdict::by(FolderState::JournalUnreadable, detail));
            }
            Err(verdict) => return found.decided(verdict),
        };
        let mut refusal = None;
        let mut later = false;
        let mut unfinished = false;
        for name in names.iter().filter(|name| !is_pending(name)) {
            match Journal::judge(&dir, name, running) {
                Journal::Gone => continue,
                Journal::Unfinished => unfinished = true,
                Journal::Later => later = true,
                Journal::Refused(verdict) => {
                    refusal.get_or_insert(verdict);
                }
            }
            found.journals.push(shown_name(name));
        }
        found.verdict = refusal
            .or_else(|| later.then(|| Verdict::of(FolderState::NewerLauncher)))
            .or_else(|| unfinished.then(|| Verdict::of(FolderState::Pending)));
        found
    }

    /// What was found, with the state that `verdict` decided.
    fn decided(mut self, verdict: Verdict) -> StateFolder {
        self.verdict = Some(verdict);
        self
    }
}

/// What a journal is to this launcher.
#[derive(Debug)]
enum Journal {
    /// One it reads and may change: of one of its kinds, with a stamp that lets it.
    Unfinished,
    /// A later launcher's: of a later schema, asking for a later launcher, or of a kind this
    /// launcher does not know, which it does not read.
    Later,
    /// Gone since its folder was listed, as a job that ends removes its journal.
    Gone,
    /// One that cannot be read, with the state that decides.
    Refused(Verdict),
}

impl Journal {
    /// Judges the journal `name` in the journal folder `dir` for the launcher of version
    /// `running`.
    fn judge(dir: &Path, name: &OsStr, running: &Version) -> Journal {
        let Some(name) = name.to_str().filter(|name| JOURNAL_NAMES.contains(name)) else {
            return Journal::Later;
        };
        let path = dir.join(name);
        let object = match read_json_bounded::<Map<String, Value>>(&path, MAX_JSON_LEN) {
            Ok(object) => object,
            Err(error) => return Journal::unread(error, &path),
        };
        match stamp_of(&object) {
            Ok(stamp) => match access(&stamp, JOURNAL_SCHEMA, running) {
                Access::Writable => Journal::Unfinished,
                Access::ReadOnly => Journal::Later,
            },
            Err(error) => Journal::Refused(Verdict::by(
                FolderState::JournalUnreadable,
                format_args!("{}: {error}", path.display()),
            )),
        }
    }

    /// What the journal at `path` is once reading it failed for `error`. A link is refused as one;
    /// nothing there means the journal is gone; something there that is no regular file, a file
    /// too large and one that holds no JSON object are journals this launcher cannot read; and a
    /// refusal to read a file that is there is the system's. Systems differ in how they refuse to
    /// open a folder as a file, so the entry is looked at rather than the refusal.
    fn unread(error: FilesError, path: &Path) -> Journal {
        let state = match error {
            FilesError::Link { .. } => FolderState::Links,
            FilesError::TooLarge { .. } | FilesError::Json { .. } => FolderState::JournalUnreadable,
            FilesError::Io { .. } => match fs::symlink_metadata(path) {
                Err(looked) if is_absent(&looked) => return Journal::Gone,
                Ok(metadata) if !metadata.is_file() => FolderState::JournalUnreadable,
                _ => FolderState::Unreadable,
            },
            _ => FolderState::Unreadable,
        };
        Journal::Refused(Verdict::by(state, error))
    }
}

/// What is at a folder of the launcher's state.
enum Names {
    /// Nothing.
    Missing,
    /// Something other than a folder.
    NotFolder,
    /// A folder holding these names, sorted.
    Listed(Vec<OsString>),
}

/// What is at `path`, where the launcher keeps a folder of its state, looked at without following
/// a link and listed without opening any entry. A link there is refused, and so is anything the
/// system will not let be looked at or listed.
fn names_at(path: &Path) -> Result<Names, Verdict> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() => return Err(Verdict::link(path)),
        Ok(metadata) if !metadata.is_dir() => return Ok(Names::NotFolder),
        Ok(_) => {}
        Err(error) if is_absent(&error) => return Ok(Names::Missing),
        Err(error) => return Err(Verdict::refused(path, error)),
    }
    match names_in(path) {
        Ok(mut names) => {
            names.sort();
            Ok(Names::Listed(names))
        }
        Err(error) => Err(Verdict::refused(path, error)),
    }
}

/// Reads the installation in `folder` through its receipt, when anything is at the receipt's
/// name, for the launcher of version `running`: the state the reading decides, and the
/// installation when it reads as one.
fn read_receipt(
    folder: &InstallFolder,
    running: &Version,
) -> (Option<Verdict>, Option<InstallationSummary>) {
    let receipt = folder.state_dir().join(RECEIPT_FILE);
    if matches!(fs::symlink_metadata(&receipt), Err(error) if is_absent(&error)) {
        return (None, None);
    }
    match folder.installed(running) {
        Ok(game) => {
            let state = match game.access {
                Access::Writable => FolderState::Installed,
                Access::ReadOnly => FolderState::NewerLauncher,
            };
            let summary = InstallationSummary::of(&game, running);
            (Some(Verdict::of(state)), Some(summary))
        }
        Err(error) => {
            let state = match error {
                ReceiptError::Links { .. } => FolderState::Links,
                ReceiptError::Unreadable { .. } | ReceiptError::Disagrees { .. } => {
                    FolderState::ReceiptInvalid
                }
                ReceiptError::OtherPlatform { .. } => FolderState::OtherPlatform,
                ReceiptError::GameMissing { .. } => FolderState::GameMissing,
                ReceiptError::Io { .. } => FolderState::Unreadable,
            };
            (Some(Verdict::by(state, error)), None)
        }
    }
}

/// Whether `name` is a pending file's, which an interrupted write leaves and the next write
/// removes.
fn is_pending(name: &OsStr) -> bool {
    name.as_encoded_bytes().ends_with(PENDING_SUFFIX.as_bytes())
}

/// How a report shows the name `name`.
fn shown_name(name: &OsStr) -> String {
    name.to_string_lossy().into_owned()
}

/// How a report shows the path `path`: without a verbatim prefix.
fn shown(path: &Path) -> String {
    strip_verbatim(path).display().to_string()
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};

    use gdl_files::{ALPHA_NAMES, FilesError, MAX_JSON_LEN, retry_locked};
    use gdl_fixtures::{
        InstallFixture, RuntimeFiles, backdate, folder_names, hold_closed, holds_bind, link_folder,
        make_alpha_markers, pretty_json, snapshot,
    };
    use gdl_release::{Platform, Version};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::{
        FolderState, INSPECTION_SCHEMA, Inspection, InstallationSummary, Journal, Relation,
    };
    use crate::compat::{Access, JOURNAL_NAMES, MIN_LAUNCHER};
    use crate::folder::{InstallFolder, JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE, STATE_DIR};
    use crate::payload::BUILD_INFO;
    use crate::receipt::GUNE5D;

    /// Every state, by the name the launcher's reports give it.
    const STATES: [(FolderState, &str); 14] = [
        (FolderState::Absent, "absent"),
        (FolderState::NotDirectory, "notDirectory"),
        (FolderState::Links, "links"),
        (FolderState::Unreadable, "unreadable"),
        (FolderState::JournalUnreadable, "journalUnreadable"),
        (FolderState::AlphaInstall, "alphaInstall"),
        (FolderState::Empty, "empty"),
        (FolderState::NotInstalled, "notInstalled"),
        (FolderState::Pending, "pending"),
        (FolderState::Installed, "installed"),
        (FolderState::ReceiptInvalid, "receiptInvalid"),
        (FolderState::OtherPlatform, "otherPlatform"),
        (FolderState::GameMissing, "gameMissing"),
        (FolderState::NewerLauncher, "newerLauncher"),
    ];

    /// The commit the standard runtime says it was built from.
    const STANDARD_COMMIT: &str = "test-commit";

    /// The disc revision an installation fixture's receipt records.
    const DISC_REVISION: u8 = 1;

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The version the tests run the launcher as: the first launcher's.
    fn running() -> Version {
        version(MIN_LAUNCHER)
    }

    /// The platform this test program was built for, whose installations it reads.
    fn this_platform() -> Platform {
        Platform::of_this_build().expect("a release is built for the system the tests run on")
    }

    /// Inspects the folder at `root` as the first launcher does.
    fn inspect(root: &Path) -> Inspection {
        InstallFolder::inspect(root, &running())
    }

    /// A new, empty folder in a folder of the test's own.
    fn bare_folder() -> (TempDir, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("folder");
        fs::create_dir(&root).unwrap();
        (dir, root)
    }

    /// An installation of the standard runtime of `version` for the platform this test program
    /// was built for, as the launcher leaves one, in a folder of the test's own.
    fn installation_of(version: &str) -> (TempDir, InstallFixture) {
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(this_platform().as_str(), version);
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        (dir, fixture)
    }

    /// An installation of the standard runtime of the first launcher's version.
    fn installation() -> (TempDir, InstallFixture) {
        installation_of(MIN_LAUNCHER)
    }

    /// The journal folder of the folder at `root`.
    fn journal_dir(root: &Path) -> PathBuf {
        root.join(STATE_DIR).join(JOURNAL_DIR)
    }

    /// Writes `bytes` as the journal `name` of the folder at `root`, making the state folder and
    /// the journal folder when they are missing.
    fn write_journal(root: &Path, name: &str, bytes: &[u8]) {
        let dir = journal_dir(root);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join(name), bytes).unwrap();
    }

    /// A journal of a job partway through, stamped with `schema` and `min_launcher`.
    fn journal(schema: Value, min_launcher: Value) -> Vec<u8> {
        pretty_json(&json!({"schema": schema, "minLauncher": min_launcher, "phase": "staging"}))
    }

    /// A journal as the first launcher writes one.
    fn own_journal() -> Vec<u8> {
        journal(json!(1), json!(MIN_LAUNCHER))
    }

    /// Removes the file at `path`, waiting out a scanner that still looks at a new one, as one may
    /// on Windows.
    fn remove_file(path: &Path) {
        retry_locked(|| fs::remove_file(path))
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
    }

    /// What an inspection says of the link at `path`.
    fn link_refusal(path: &Path) -> String {
        format!(
            "Choose a directory without links or junctions: {}",
            path.display()
        )
    }

    /// Checks that `inspection` found the state `state`, for the case `what` names.
    #[track_caller]
    fn assert_state(inspection: &Inspection, state: FolderState, what: &str) {
        assert_eq!(inspection.state, state, "{what}: {inspection:?}");
    }

    /// What the launcher of version `launcher` is told of an installation of the standard runtime
    /// of `version` for this platform, whose receipt it reads, and which it stands to as
    /// `relation`.
    fn summary(version: &str, launcher: &str, relation: Relation) -> InstallationSummary {
        InstallationSummary {
            version: version.to_owned(),
            commit: STANDARD_COMMIT.to_owned(),
            platform: this_platform(),
            disc: Some(GUNE5D.to_owned()),
            disc_revision: Some(DISC_REVISION),
            launcher: launcher.to_owned(),
            relation,
            access: Access::Writable,
        }
    }

    #[test]
    fn states() {
        let mut covered = BTreeSet::new();
        let mut check = |inspection: Inspection, state: FolderState, what: &str| {
            assert_state(&inspection, state, what);
            covered.insert(state.as_str());
            inspection
        };
        let dir = tempfile::tempdir().unwrap();
        let base = dir.path();
        let file = base.join("file.txt");
        fs::write(&file, b"not a folder").unwrap();
        let real = base.join("real");
        fs::create_dir_all(real.join("sub")).unwrap();
        let link = base.join("link");
        link_folder(&link, &real).unwrap();

        // Nothing at the path, at the top or deeper, under a file or under a link: a folder yet to
        // be made is absent whatever lies above it.
        let missing = base.join("missing");
        for path in [
            missing.clone(),
            missing.join("deeper"),
            file.join("inside"),
            link.join("missing"),
        ] {
            let inspection = check(inspect(&path), FolderState::Absent, "nothing");
            assert_eq!(inspection.detail, None, "{}", path.display());
        }
        let inspection = check(inspect(&file), FolderState::NotDirectory, "a file");
        assert_eq!(
            inspection.detail,
            Some(format!(
                "Install folder is not a directory: {}",
                file.display()
            ))
        );
        // A link, and a folder reached through one.
        for path in [link.clone(), link.join("sub")] {
            let inspection = check(inspect(&path), FolderState::Links, "a link");
            assert_eq!(inspection.detail, Some(link_refusal(&link)));
        }
        // A path that names nothing has no absolute form to look at.
        let inspection = check(inspect(Path::new("")), FolderState::Unreadable, "no path");
        assert!(inspection.detail.is_some());

        // Folders that hold no installation.
        let (_empty, root) = bare_folder();
        check(inspect(&root), FolderState::Empty, "an empty folder");
        fs::write(root.join("notes.txt"), b"the player's").unwrap();
        check(
            inspect(&root),
            FolderState::NotInstalled,
            "a folder holding a file",
        );
        let (_state_file, root) = bare_folder();
        fs::write(root.join(STATE_DIR), b"").unwrap();
        check(
            inspect(&root),
            FolderState::NotInstalled,
            "a file in the state folder's place, which holds no state",
        );
        let (_alpha, root) = bare_folder();
        make_alpha_markers(&root, &["installation.json"]).unwrap();
        check(inspect(&root), FolderState::AlphaInstall, "an alpha folder");

        // Jobs.
        let (_job, root) = bare_folder();
        write_journal(&root, "install.json", &own_journal());
        check(inspect(&root), FolderState::Pending, "a job to finish");
        let (_bad_job, root) = bare_folder();
        write_journal(&root, "install.json", b"not JSON\n");
        check(
            inspect(&root),
            FolderState::JournalUnreadable,
            "a journal that is not JSON",
        );

        // Installations of the launcher's own release, of an earlier one and of a later one.
        let (_same, fixture) = installation();
        let inspection = check(inspect(&fixture.root), FolderState::Installed, "its own");
        assert_eq!(
            inspection.installation,
            Some(summary(MIN_LAUNCHER, MIN_LAUNCHER, Relation::Same))
        );
        assert_eq!(inspection.detail, None);
        // Versions compare by precedence, so build metadata alone makes no launcher the newer.
        for (launcher, relation) in [
            ("1.0.0-beta+build.7", Relation::Same),
            ("1.0.1", Relation::Newer),
        ] {
            let inspection = check(
                InstallFolder::inspect(&fixture.root, &version(launcher)),
                FolderState::Installed,
                launcher,
            );
            assert_eq!(
                inspection.installation,
                Some(summary(MIN_LAUNCHER, launcher, relation))
            );
        }
        let (_older, fixture) = installation_of("1.0.1");
        let inspection = check(
            inspect(&fixture.root),
            FolderState::Installed,
            "a later one",
        );
        assert_eq!(
            inspection.installation,
            Some(summary("1.0.1", MIN_LAUNCHER, Relation::Older))
        );

        // Installations that do not read as one.
        let (_invalid, fixture) = installation();
        let mut receipt = fixture.receipt();
        receipt["commit"] = json!("another-commit");
        fixture.write_receipt(&receipt).unwrap();
        let inspection = check(
            inspect(&fixture.root),
            FolderState::ReceiptInvalid,
            "a receipt that disagrees",
        );
        assert_eq!(
            inspection.detail.as_deref(),
            Some("Installation receipts disagree; choose a complete installed game (commit)")
        );
        assert_eq!(inspection.installation, None);
        let receipt_file = fixture.root.join(STATE_DIR).join(RECEIPT_FILE);
        remove_file(&receipt_file);
        fs::create_dir(&receipt_file).unwrap();
        check(
            inspect(&fixture.root),
            FolderState::ReceiptInvalid,
            "a folder in the receipt's place",
        );

        // Another platform's records over this platform's files, since a Linux installation
        // cannot be laid out where names ignore letter case.
        let other = Platform::ALL
            .into_iter()
            .find(|&platform| platform != this_platform())
            .unwrap();
        let (_other, mut fixture) = installation();
        fixture.runtime = RuntimeFiles::standard(other.as_str(), MIN_LAUNCHER);
        let build_info = fixture.runtime.build_info_bytes();
        fs::write(fixture.root.join(BUILD_INFO), build_info).unwrap();
        fixture.write_receipt(&fixture.receipt()).unwrap();
        let inspection = check(
            inspect(&fixture.root),
            FolderState::OtherPlatform,
            "another platform's",
        );
        assert_eq!(
            inspection.detail,
            Some(format!(
                "This installation belongs to a different platform: {}",
                other.as_str()
            ))
        );

        let (_no_game, fixture) = installation();
        let executable = fixture.root.join(this_platform().executable());
        remove_file(&executable);
        let inspection = check(inspect(&fixture.root), FolderState::GameMissing, "no game");
        assert_eq!(
            inspection.detail,
            Some(format!(
                "The installed executable is missing: {}",
                executable.display()
            ))
        );

        let (_newer, fixture) = installation();
        let later = json!({"schema": 2, "minLauncher": MIN_LAUNCHER});
        fixture.write_receipt(&later).unwrap();
        check(
            inspect(&fixture.root),
            FolderState::NewerLauncher,
            "a later launcher's receipt",
        );

        // A folder the system will not list, which it refuses every user but root.
        #[cfg(unix)]
        {
            let (_dir, root) = bare_folder();
            let denied = gdl_fixtures::DeniedAccess::deny(&[&root]).unwrap();
            if fs::read_dir(&root).is_ok() {
                eprintln!("skipped the folder that cannot be listed: this user lists any, as root");
            } else {
                let inspection = check(inspect(&root), FolderState::Unreadable, "unlisted");
                let detail = inspection.detail.unwrap_or_default();
                let shown = format!("{}: ", root.display());
                assert!(detail.starts_with(&shown), "{detail}");
            }
            drop(denied);
        }

        let every: BTreeSet<&str> = STATES.iter().map(|&(_, name)| name).collect();
        assert_eq!(covered, every);
    }

    #[test]
    fn states_and_reports_keep_their_names() {
        for (state, name) in STATES {
            assert_eq!(state.as_str(), name);
        }
        let names: BTreeSet<&str> = STATES.iter().map(|&(_, name)| name).collect();
        assert_eq!(names.len(), STATES.len());
        assert_eq!(INSPECTION_SCHEMA, 1);
    }

    #[test]
    fn journals_are_judged_by_their_stamps() {
        // Each of the launcher's own journals, stamped as it stamps them, is a job to finish or to
        // roll back.
        for name in JOURNAL_NAMES {
            let (_dir, root) = bare_folder();
            write_journal(&root, name, &own_journal());
            let inspection = inspect(&root);
            assert_state(&inspection, FolderState::Pending, name);
            assert_eq!(inspection.journals, [name]);
            assert_eq!(inspection.detail, None, "{name}");
        }

        // The stamp decides, its version compared by precedence; a journal holding no stamp that
        // can be read is one this launcher cannot read.
        let own_keys = json!({"schema": 1, "minLauncher": MIN_LAUNCHER, "future": [1]});
        for (what, bytes, state) in [
            (
                "its own version with build metadata",
                journal(json!(1), json!("1.0.0-beta+x")),
                FolderState::Pending,
            ),
            (
                "an earlier launcher's",
                journal(json!(1), json!("0.1.0-alpha.3")),
                FolderState::Pending,
            ),
            (
                "keys of its own",
                pretty_json(&own_keys),
                FolderState::Pending,
            ),
            (
                "schema 2",
                journal(json!(2), json!(MIN_LAUNCHER)),
                FolderState::NewerLauncher,
            ),
            (
                "a later launcher",
                journal(json!(1), json!("9.0.0")),
                FolderState::NewerLauncher,
            ),
            (
                "not JSON",
                b"not JSON\n".to_vec(),
                FolderState::JournalUnreadable,
            ),
            ("nothing", Vec::new(), FolderState::JournalUnreadable),
            ("an array", b"[]\n".to_vec(), FolderState::JournalUnreadable),
            (
                "no stamp",
                pretty_json(&json!({"phase": "staging"})),
                FolderState::JournalUnreadable,
            ),
            (
                "a schema as text",
                journal(json!("1"), json!(MIN_LAUNCHER)),
                FolderState::JournalUnreadable,
            ),
            (
                "a tag for a version",
                journal(json!(1), json!("v1.0.0-beta")),
                FolderState::JournalUnreadable,
            ),
            (
                "a later schema without a version",
                pretty_json(&json!({"schema": 2})),
                FolderState::JournalUnreadable,
            ),
        ] {
            let (_dir, root) = bare_folder();
            write_journal(&root, "install.json", &bytes);
            let inspection = inspect(&root);
            assert_state(&inspection, state, what);
            assert_eq!(inspection.journals, ["install.json"], "{what}");
            let refused = state == FolderState::JournalUnreadable;
            assert_eq!(inspection.detail.is_some(), refused, "{what}");
        }
        // The refusal names the journal and what its stamp lacks.
        let (_dir, root) = bare_folder();
        write_journal(
            &root,
            "update.json",
            &pretty_json(&json!({"minLauncher": "1.0.0"})),
        );
        let path = journal_dir(&root).join("update.json");
        assert_eq!(
            inspect(&root).detail,
            Some(format!(
                "{}: schema is missing, not an integer of at least 1",
                path.display()
            ))
        );

        // A kind this launcher does not know is a later launcher's, whatever it holds, and is not
        // read; so is a known kind in another letter case.
        for name in ["repair.json", "Install.json", "install.json.old"] {
            let (_dir, root) = bare_folder();
            write_journal(&root, name, b"not JSON\n");
            let inspection = inspect(&root);
            assert_state(&inspection, FolderState::NewerLauncher, name);
            assert_eq!(inspection.journals, [name]);
            assert_eq!(inspection.detail, None, "{name}");
        }

        // The pending files that interrupted writes leave are passed over, so a journal folder
        // holding nothing else leaves an installation as it is.
        let (_dir, fixture) = installation();
        let root = &fixture.root;
        for name in ["install.json.pending", "repair.json.pending"] {
            write_journal(root, name, &own_journal());
        }
        let inspection = inspect(root);
        assert_state(&inspection, FolderState::Installed, "pending files alone");
        assert!(inspection.journals.is_empty(), "{inspection:?}");
        // A job comes before the receipt, and the installation is still told of.
        write_journal(root, "install.json", &own_journal());
        let inspection = inspect(root);
        assert_state(
            &inspection,
            FolderState::Pending,
            "a job beside an installation",
        );
        assert_eq!(inspection.journals, ["install.json"]);
        assert_eq!(
            inspection.installation,
            Some(summary(MIN_LAUNCHER, MIN_LAUNCHER, Relation::Same))
        );

        // A journal that cannot be read comes before a later launcher's, which comes before the
        // launcher's own. Journals are listed in name order.
        let later = || journal(json!(2), json!(MIN_LAUNCHER));
        for (what, journals, state) in [
            (
                "a later launcher's beside the launcher's own",
                vec![("install.json", own_journal()), ("update.json", later())],
                FolderState::NewerLauncher,
            ),
            (
                "a kind it does not know beside the launcher's own",
                vec![
                    ("install.json", own_journal()),
                    ("repair.json", own_journal()),
                ],
                FolderState::NewerLauncher,
            ),
            (
                "one that cannot be read beside both",
                vec![
                    ("expansion-n64.json", own_journal()),
                    ("install.json", b"[]\n".to_vec()),
                    ("update.json", later()),
                ],
                FolderState::JournalUnreadable,
            ),
        ] {
            let (_dir, root) = bare_folder();
            for (name, bytes) in journals.iter().rev() {
                write_journal(&root, name, bytes);
            }
            let inspection = inspect(&root);
            assert_state(&inspection, state, what);
            let names: Vec<&str> = journals.iter().map(|&(name, _)| name).collect();
            assert_eq!(inspection.journals, names, "{what}");
        }

        // A folder in a journal's place, a file in the journal folder's place and a journal past
        // the bound are no journals this launcher can read.
        let (_dir, root) = bare_folder();
        fs::create_dir_all(journal_dir(&root).join("install.json")).unwrap();
        let inspection = inspect(&root);
        assert_state(&inspection, FolderState::JournalUnreadable, "a folder");
        assert_eq!(inspection.journals, ["install.json"]);
        let (_dir, root) = bare_folder();
        fs::create_dir(root.join(STATE_DIR)).unwrap();
        fs::write(journal_dir(&root), b"").unwrap();
        let inspection = inspect(&root);
        assert_state(
            &inspection,
            FolderState::JournalUnreadable,
            "a file of journals",
        );
        assert_eq!(
            inspection.detail,
            Some(format!("{}: not a directory", journal_dir(&root).display()))
        );
        assert!(inspection.journals.is_empty());
        let bound = usize::try_from(MAX_JSON_LEN).unwrap();
        let (_dir, root) = bare_folder();
        write_journal(&root, "install.json", &vec![b' '; bound + 1]);
        let inspection = inspect(&root);
        assert_state(
            &inspection,
            FolderState::JournalUnreadable,
            "past the bound",
        );
        let path = journal_dir(&root).join("install.json");
        assert_eq!(
            inspection.detail,
            Some(format!("Metadata is too large: {}", path.display()))
        );
        // One of exactly the bound is read.
        let mut padded = own_journal();
        padded.resize(bound, b' ');
        write_journal(&root, "install.json", &padded);
        assert_state(&inspect(&root), FolderState::Pending, "at the bound");
    }

    #[test]
    fn failed_journal_reads_keep_their_kinds() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("install.json");
        fs::write(&file, own_journal()).unwrap();
        let folder = dir.path().join("update.json");
        fs::create_dir(&folder).unwrap();
        let gone = dir.path().join("expansion-n64.json");
        let refused_at = |path: &Path| FilesError::Io {
            path: path.to_path_buf(),
            source: io::Error::new(io::ErrorKind::PermissionDenied, "refused"),
        };
        let not_json = serde_json::from_slice::<Value>(b"not JSON").unwrap_err();
        // A link is refused as one, a journal that holds what no journal can is one this
        // launcher cannot read, a refusal to read a file that is there is the system's, and a
        // journal no longer there went with its job.
        for (what, error, path, state) in [
            (
                "a link",
                FilesError::Link { path: file.clone() },
                &file,
                Some(FolderState::Links),
            ),
            (
                "too much",
                FilesError::TooLarge {
                    path: file.clone(),
                    limit: 1,
                },
                &file,
                Some(FolderState::JournalUnreadable),
            ),
            (
                "no JSON",
                FilesError::Json {
                    path: file.clone(),
                    source: not_json,
                },
                &file,
                Some(FolderState::JournalUnreadable),
            ),
            (
                "a folder",
                refused_at(&folder),
                &folder,
                Some(FolderState::JournalUnreadable),
            ),
            (
                "a file the system will not read",
                refused_at(&file),
                &file,
                Some(FolderState::Unreadable),
            ),
            (
                "anything else",
                FilesError::NotRoot { name: "x".into() },
                &file,
                Some(FolderState::Unreadable),
            ),
            ("nothing there", refused_at(&gone), &gone, None),
        ] {
            let shown = error.to_string();
            match (Journal::unread(error, path), state) {
                (Journal::Refused(verdict), Some(state)) => {
                    assert_eq!(verdict.state, state, "{what}");
                    assert_eq!(verdict.detail.as_deref(), Some(shown.as_str()), "{what}");
                }
                (Journal::Gone, None) => {}
                (other, _) => panic!("{what}: {other:?}"),
            }
        }
    }

    #[test]
    fn links_in_the_state_folder_are_refused() {
        // The state folder a link, to a folder that holds a job and a receipt: nothing behind it
        // is judged, and nothing there changes.
        let (dir, fixture) = installation();
        write_journal(&fixture.root, "install.json", &own_journal());
        let state = fixture.root.join(STATE_DIR);
        let moved = dir.path().join("moved state");
        retry_locked(|| fs::rename(&state, &moved)).unwrap();
        link_folder(&state, &moved).unwrap();
        backdate(&moved).unwrap();
        let before = snapshot(&moved).unwrap();
        let inspection = inspect(&fixture.root);
        assert_state(&inspection, FolderState::Links, "a linked state folder");
        assert_eq!(inspection.detail, Some(link_refusal(&state)));
        assert!(inspection.journals.is_empty());
        assert!(inspection.unknown_names.is_empty());
        assert_eq!(inspection.installation, None);
        assert_eq!(snapshot(&moved).unwrap(), before);

        // The journal folder a link, to a folder holding a job.
        let (dir, root) = bare_folder();
        fs::create_dir(root.join(STATE_DIR)).unwrap();
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        fs::write(elsewhere.join("install.json"), own_journal()).unwrap();
        link_folder(&journal_dir(&root), &elsewhere).unwrap();
        let inspection = inspect(&root);
        assert_state(&inspection, FolderState::Links, "a linked journal folder");
        assert_eq!(inspection.detail, Some(link_refusal(&journal_dir(&root))));
        assert!(inspection.journals.is_empty());

        // A journal a link to a folder: on Windows a junction.
        let (dir, root) = bare_folder();
        fs::create_dir_all(journal_dir(&root)).unwrap();
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        let linked = journal_dir(&root).join("install.json");
        link_folder(&linked, &elsewhere).unwrap();
        let inspection = inspect(&root);
        assert_state(&inspection, FolderState::Links, "a linked journal");
        assert_eq!(inspection.detail, Some(link_refusal(&linked)));
        assert_eq!(inspection.journals, ["install.json"]);
        assert!(folder_names(&elsewhere).unwrap().is_empty());

        // The receipt a link.
        let (dir, fixture) = installation();
        let receipt = fixture.root.join(STATE_DIR).join(RECEIPT_FILE);
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        remove_file(&receipt);
        link_folder(&receipt, &elsewhere).unwrap();
        let inspection = inspect(&fixture.root);
        assert_state(&inspection, FolderState::Links, "a linked receipt");
        assert_eq!(inspection.detail, Some(link_refusal(&receipt)));
        assert_eq!(inspection.installation, None);
    }

    #[test]
    fn a_newer_receipt_inspects_with_its_summary() {
        let (_dir, fixture) = installation_of("1.0.1");
        let read_only = InstallationSummary {
            disc: None,
            disc_revision: None,
            access: Access::ReadOnly,
            ..summary("1.0.1", MIN_LAUNCHER, Relation::Older)
        };
        for (what, receipt) in [
            (
                "a later schema",
                json!({"schema": 2, "minLauncher": MIN_LAUNCHER, "disc": [7], "future": {"x": 1}}),
            ),
            (
                "a later launcher",
                json!({"schema": 1, "minLauncher": "1.0.1"}),
            ),
        ] {
            fixture.write_receipt(&receipt).unwrap();
            let inspection = inspect(&fixture.root);
            assert_state(&inspection, FolderState::NewerLauncher, what);
            assert_eq!(inspection.detail, None, "{what}");
            assert_eq!(inspection.installation.as_ref(), Some(&read_only), "{what}");
        }
        // The launcher the receipt asks for reads it as its own, and refuses what it holds.
        let inspection = InstallFolder::inspect(&fixture.root, &version("1.0.1"));
        assert_state(
            &inspection,
            FolderState::ReceiptInvalid,
            "the launcher it asks for",
        );

        // A job beside it comes first, whether this launcher's or a later one's, and the
        // installation is still told of.
        write_journal(&fixture.root, "install.json", &own_journal());
        let inspection = inspect(&fixture.root);
        assert_state(&inspection, FolderState::Pending, "a job beside it");
        assert_eq!(inspection.installation.as_ref(), Some(&read_only));
        let later = journal(json!(2), json!(MIN_LAUNCHER));
        write_journal(&fixture.root, "update.json", &later);
        let inspection = inspect(&fixture.root);
        assert_state(
            &inspection,
            FolderState::NewerLauncher,
            "a later job beside it",
        );
        assert_eq!(inspection.installation.as_ref(), Some(&read_only));

        // Without its game it is no installation to tell of.
        for name in ["install.json", "update.json"] {
            remove_file(&journal_dir(&fixture.root).join(name));
        }
        remove_file(&fixture.root.join(this_platform().executable()));
        let inspection = inspect(&fixture.root);
        assert_state(&inspection, FolderState::GameMissing, "no game");
        assert_eq!(inspection.installation, None);
    }

    #[test]
    fn inspect_never_writes() {
        // Folders in many states side by side, compared from the folder above them, so that
        // nothing made beside one goes unseen either.
        let dir = tempfile::tempdir().unwrap();
        let base = dir.path();
        let runtime = || RuntimeFiles::standard(this_platform().as_str(), MIN_LAUNCHER);
        let installed = InstallFixture::write(&base.join("installed"), runtime()).unwrap();
        let pending = InstallFixture::write(&base.join("pending"), runtime()).unwrap();
        write_journal(&pending.root, "install.json", &own_journal());
        let unreadable = InstallFixture::write(&base.join("unreadable"), runtime()).unwrap();
        write_journal(&unreadable.root, "install.json", b"not JSON\n");
        let newer = InstallFixture::write(&base.join("newer"), runtime()).unwrap();
        let later = json!({"schema": 2, "minLauncher": MIN_LAUNCHER});
        newer.write_receipt(&later).unwrap();
        let invalid = InstallFixture::write(&base.join("invalid"), runtime()).unwrap();
        invalid.write_receipt(&json!([])).unwrap();
        let alpha = base.join("alpha");
        fs::create_dir(&alpha).unwrap();
        make_alpha_markers(&alpha, &ALPHA_NAMES).unwrap();
        let empty = base.join("empty");
        fs::create_dir(&empty).unwrap();
        let link = base.join("link");
        link_folder(&link, &installed.root).unwrap();
        let file = base.join("file");
        fs::write(&file, b"").unwrap();
        let fixtures = [&installed, &pending, &unreadable, &newer, &invalid];
        let mut roots: Vec<PathBuf> = fixtures
            .iter()
            .map(|fixture| fixture.root.clone())
            .collect();
        roots.extend([alpha, empty, link, file, base.join("missing")]);

        // Dated back first, so that even bytes written again as they were would show.
        backdate(base).unwrap();
        let before = snapshot(base).unwrap();
        let states: Vec<FolderState> = roots.iter().map(|root| inspect(root).state).collect();
        assert_eq!(snapshot(base).unwrap(), before);
        assert_eq!(
            states,
            [
                FolderState::Installed,
                FolderState::Pending,
                FolderState::JournalUnreadable,
                FolderState::NewerLauncher,
                FolderState::ReceiptInvalid,
                FolderState::AlphaInstall,
                FolderState::Empty,
                FolderState::Links,
                FolderState::NotDirectory,
                FolderState::Absent,
            ]
        );
        // No lock was taken, so no installation holds a lock file.
        for fixture in fixtures {
            let lock_file = fixture.root.join(STATE_DIR).join(LOCK_FILE);
            assert!(fs::symlink_metadata(&lock_file).is_err(), "{fixture:?}");
        }
        // A job that holds the lock does not keep an inspection waiting.
        let held = InstallFolder::at(&installed.root).unwrap().lock().unwrap();
        assert_state(
            &inspect(&installed.root),
            FolderState::Installed,
            "beside a job",
        );
        drop(held);
    }

    #[test]
    fn an_alpha_folder_inspects_as_alpha_install() {
        let upper: Vec<String> = ALPHA_NAMES
            .iter()
            .map(|name| name.to_ascii_uppercase())
            .collect();
        let mut cases: Vec<(Vec<&str>, Vec<&str>)> = Vec::new();
        for (name, shouted) in ALPHA_NAMES.into_iter().zip(&upper) {
            cases.push((vec![name], vec![name]));
            cases.push((vec![shouted.as_str()], vec![name]));
        }
        cases.push((ALPHA_NAMES.to_vec(), ALPHA_NAMES.to_vec()));
        // Asked apart from the holds, so that a hold which kept nothing out fails the test.
        let probe = tempfile::tempdir().unwrap();
        let binds = holds_bind(probe.path()).unwrap();
        for (made, found) in cases {
            let (_dir, root) = bare_folder();
            make_alpha_markers(&root, &made).unwrap();
            let markers: Vec<PathBuf> = made.iter().map(|name| root.join(name)).collect();
            let paths: Vec<&Path> = markers.iter().map(PathBuf::as_path).collect();
            // Any open of a marker fails while it is held, but for root's on Unix.
            let held = hold_closed(&paths).unwrap();
            let inspection = inspect(&root);
            let proven = held.proven();
            drop(held);
            let what = format!("{made:?}");
            assert_eq!(proven, binds, "{what}");
            assert_state(&inspection, FolderState::AlphaInstall, &what);
            assert_eq!(inspection.alpha_markers, found, "{what}");
            assert_eq!(inspection.detail, None, "{what}");
            assert_eq!(inspection.installation, None, "{what}");
        }
        if !binds {
            eprintln!(
                "only the listing was checked: the proof that no marker is opened needs a user \
                 other than root, who opens entries of mode 0o000"
            );
        }

        // Beside an installation, and beside a job, the markers are reported in the state those
        // decide.
        let (_dir, fixture) = installation();
        make_alpha_markers(&fixture.root, &ALPHA_NAMES).unwrap();
        let inspection = inspect(&fixture.root);
        assert_state(
            &inspection,
            FolderState::Installed,
            "beside an installation",
        );
        assert_eq!(inspection.alpha_markers, ALPHA_NAMES);
        write_journal(&fixture.root, "install.json", &own_journal());
        let inspection = inspect(&fixture.root);
        assert_state(&inspection, FolderState::Pending, "beside a job");
        assert_eq!(inspection.alpha_markers, ALPHA_NAMES);

        // Markers in a folder inside the folder are not the folder's.
        let (_dir, root) = bare_folder();
        let saves = root.join("saves");
        fs::create_dir(&saves).unwrap();
        make_alpha_markers(&saves, &ALPHA_NAMES).unwrap();
        let inspection = inspect(&root);
        assert_state(&inspection, FolderState::NotInstalled, "markers further in");
        assert!(inspection.alpha_markers.is_empty());
    }

    #[test]
    fn inspections_show_the_folder_and_the_names_in_its_state() {
        // The folder is shown absolute and folded by name.
        let (dir, fixture) = installation();
        let dotted = dir.path().join("x").join("..").join(".").join("install");
        let inspection = inspect(&dotted);
        assert_state(&inspection, FolderState::Installed, "a dotted path");
        assert_eq!(inspection.path, fixture.root.display().to_string());
        // And without the verbatim prefix it was given with.
        #[cfg(windows)]
        {
            use std::path::{Component, Prefix};

            let plain = std::path::absolute(&fixture.root).unwrap();
            let on_a_drive = matches!(
                plain.components().next(),
                Some(Component::Prefix(prefix)) if matches!(prefix.kind(), Prefix::Disk(_))
            );
            if on_a_drive {
                let verbatim = PathBuf::from(format!(r"\\?\{}", plain.display()));
                let inspection = inspect(&verbatim);
                assert_state(&inspection, FolderState::Installed, "a verbatim path");
                assert_eq!(inspection.path, plain.display().to_string());
            } else {
                eprintln!("skipped the verbatim path: the test's folder is on no drive");
            }
        }

        // Names in the state folder that the launcher's rules do not make are listed, sorted, and
        // change nothing; the launcher's own are not listed.
        let state = fixture.root.join(STATE_DIR);
        for name in ["notes.txt", "receipt.json.pending", LOCK_FILE] {
            fs::write(state.join(name), b"").unwrap();
        }
        for name in ["later-state", "install-AbC123xy", "logs", "retired-x"] {
            fs::create_dir(state.join(name)).unwrap();
        }
        let inspection = inspect(&fixture.root);
        assert_state(
            &inspection,
            FolderState::Installed,
            "names beside the receipt",
        );
        assert_eq!(inspection.unknown_names, ["later-state", "notes.txt"]);
        // Journals are listed sorted, the pending files left out; the journal folder is the
        // launcher's own.
        for name in ["update.json", "install.json", "expansion-n64.json.pending"] {
            write_journal(&fixture.root, name, &own_journal());
        }
        let inspection = inspect(&fixture.root);
        assert_state(&inspection, FolderState::Pending, "two jobs");
        assert_eq!(inspection.journals, ["install.json", "update.json"]);
        assert_eq!(inspection.unknown_names, ["later-state", "notes.txt"]);
        assert_eq!(inspection.detail, None);
    }

    #[cfg(unix)]
    #[test]
    fn refusals_to_look_are_the_systems() {
        use gdl_fixtures::DeniedAccess;

        let (_dir, fixture) = installation();
        let root = fixture.root.clone();
        write_journal(&root, "install.json", &own_journal());
        let state = root.join(STATE_DIR);
        let journals = journal_dir(&root);
        let journal_file = journals.join("install.json");
        let probe = DeniedAccess::deny(&[&journal_file]).unwrap();
        let readable = fs::read(&journal_file).is_ok();
        drop(probe);
        if readable {
            eprintln!("skipped: this user reads files of mode 0o000, as root does");
            return;
        }
        // A journal, a folder of the launcher's state or a folder above the installation that may
        // not be read shows neither a link nor what it holds.
        for (what, denied, refused) in [
            ("a journal", journal_file.clone(), journal_file.clone()),
            ("the journal folder", journals.clone(), journals.clone()),
            ("the state folder", state.clone(), state.clone()),
            (
                "the folder above the installation",
                root.parent().unwrap().to_path_buf(),
                root.clone(),
            ),
        ] {
            let held = DeniedAccess::deny(&[&denied]).unwrap();
            let inspection = inspect(&root);
            drop(held);
            assert_state(&inspection, FolderState::Unreadable, what);
            let detail = inspection.detail.unwrap_or_default();
            let shown = format!("{}: ", refused.display());
            assert!(detail.starts_with(&shown), "{what}: {detail}");
        }
        assert_state(&inspect(&root), FolderState::Pending, "given back");

        // With no job to decide, a receipt, or a file it pins, that may not be read is the
        // system's refusal too, not a receipt that is wrong, and tells of no installation.
        let (_other, fixture) = installation();
        let root = fixture.root.clone();
        for (what, denied) in [
            ("the receipt", root.join(STATE_DIR).join(RECEIPT_FILE)),
            ("build-info.json", root.join(BUILD_INFO)),
        ] {
            let held = DeniedAccess::deny(&[&denied]).unwrap();
            let inspection = inspect(&root);
            drop(held);
            assert_state(&inspection, FolderState::Unreadable, what);
            let detail = inspection.detail.unwrap_or_default();
            let shown = format!("{}: ", denied.display());
            assert!(detail.starts_with(&shown), "{what}: {detail}");
            assert_eq!(inspection.installation, None, "{what}");
        }
        assert_state(&inspect(&root), FolderState::Installed, "read again");
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_fifo_in_the_state_is_refused_at_once() {
        use std::sync::mpsc;
        use std::thread;
        use std::time::Duration;

        /// Far longer than any refusal takes, and short enough for a test to fail in.
        const MOST_WAIT: Duration = Duration::from_secs(10);

        let mkfifo = |path: &Path| {
            let made = std::process::Command::new("mkfifo")
                .arg(path)
                .status()
                .unwrap();
            assert!(made.success(), "mkfifo: {made}");
        };
        // A FIFO where a journal belongs, and one where the receipt does: a plain open of either
        // would wait for a writer.
        let (_job, job) = bare_folder();
        fs::create_dir_all(journal_dir(&job)).unwrap();
        let journal_fifo = journal_dir(&job).join("install.json");
        mkfifo(&journal_fifo);
        let (_receipt, fixture) = installation();
        let receipt_fifo = fixture.root.join(STATE_DIR).join(RECEIPT_FILE);
        remove_file(&receipt_fifo);
        mkfifo(&receipt_fifo);
        for (root, fifo, state) in [
            (job, journal_fifo, FolderState::JournalUnreadable),
            (
                fixture.root.clone(),
                receipt_fifo,
                FolderState::ReceiptInvalid,
            ),
        ] {
            let (told, answer) = mpsc::channel();
            let inspected = root.clone();
            let inspector = thread::spawn(move || {
                let _ = told.send(inspect(&inspected));
            });
            match answer.recv_timeout(MOST_WAIT) {
                Ok(inspection) => assert_state(&inspection, state, &root.display().to_string()),
                Err(_) => {
                    // A writer lets the waiting reader go, so that the test ends.
                    let _ = fs::OpenOptions::new().write(true).open(&fifo);
                    panic!("inspecting {} waited for a writer", root.display());
                }
            }
            inspector.join().unwrap();
        }
    }
}
