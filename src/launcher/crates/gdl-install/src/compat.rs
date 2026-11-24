//! What this launcher does with state that a later launcher wrote. Every JSON state file of the
//! launcher's own carries a stamp, its schema and the lowest launcher version allowed to change
//! it, and a stamp of a later schema, or one that asks for a later launcher, leaves the
//! installation playable but closed to changes. Under `.gdl-launcher/` this launcher changes and
//! removes only the names its own rules make, so whatever else is there stays as it was.

use std::ffi::OsString;

use gdl_files::{PENDING_SUFFIX, is_staging_name};
use gdl_release::Version;
use serde_json::{Map, Value};

use crate::folder::{JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE};
use crate::install::{DISC_FILES, INSTALL_JOURNAL, INSTALL_STAGING_PREFIX};

/// The lowest launcher version allowed to change a state file of schema 1, which every such file
/// records. It is the first launcher's version, not the writer's own: a file that a later
/// launcher writes in a schema every launcher reads stays one that every launcher may change.
pub const MIN_LAUNCHER: &str = "1.0.0-beta";

/// The schema of the journals this launcher reads and writes.
pub const JOURNAL_SCHEMA: u64 = 1;

/// The journals this launcher keeps under `.gdl-launcher/journal/`, one for each kind of job it
/// runs: an install, an update and a change to either expansion.
pub const JOURNAL_NAMES: [&str; 4] = [
    INSTALL_JOURNAL,
    "update.json",
    "expansion-n64.json",
    "expansion-dreamcast.json",
];

/// The key of a state file's schema, in its top-level object.
const SCHEMA_KEY: &str = "schema";

/// The key of the lowest launcher version allowed to change a state file, in its top-level
/// object.
const MIN_LAUNCHER_KEY: &str = "minLauncher";

/// The first schema of every kind of state file.
const FIRST_SCHEMA: u64 = 1;

/// The most characters of a stamp's value that a refusal keeps: enough for any stamp a launcher
/// writes.
const MAX_SHOWN_CHARS: usize = 64;

/// The names this launcher gives its state directly under `.gdl-launcher/`, beside those it makes
/// with a random part, and what each holds. The receipt's, the lock's and the journals' are the
/// folder module's constants, and the disc's list the install's, so that each name has one
/// spelling.
const FIXED_NAMES: [(&str, StateName); 8] = [
    (RECEIPT_FILE, StateName::Receipt),
    (LOCK_FILE, StateName::Lock),
    (JOURNAL_DIR, StateName::Journal),
    ("state.json", StateName::State),
    (DISC_FILES, StateName::DiscFiles),
    ("update-backup", StateName::UpdateBackup),
    ("bin", StateName::Bin),
    ("logs", StateName::Logs),
];

/// The prefixes of the folders a job makes directly under `.gdl-launcher/`, each followed by a
/// random part of [`STAGING_RANDOM_CHARS`](gdl_files::STAGING_RANDOM_CHARS) ASCII letters and
/// digits, as [`is_staging_name`] tells, and what each holds.
const RANDOM_PREFIXES: [(&str, StateName); 5] = [
    (INSTALL_STAGING_PREFIX, StateName::Staging),
    ("stage-", StateName::Staging),
    ("stage-n64-", StateName::Staging),
    ("stage-dreamcast-", StateName::Staging),
    ("download-", StateName::Download),
];

/// The prefix of what an update or an expansion change puts aside under `.gdl-launcher/`, which
/// more of the name always follows.
const RETIRED_PREFIX: &str = "retired-";

/// The names a fresh install makes directly under `.gdl-launcher/`, beside its staging, before
/// its first journal is written or once the journal is removed: the lock, the journal folder and
/// the list of the disc's files.
const BEFORE_JOURNAL: [&str; 3] = [LOCK_FILE, JOURNAL_DIR, DISC_FILES];

/// The state files whose pending files a fresh install leaves directly under `.gdl-launcher/`
/// when a write of one is cut short, with no journal to say so.
const PENDING_BEFORE_JOURNAL: [&str; 2] = [DISC_FILES, RECEIPT_FILE];

/// What a JSON state file records of who may change it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stamp {
    /// The file's schema, the shape of what it holds: at least 1.
    pub schema: u64,
    /// The lowest launcher version allowed to change the file.
    pub min_launcher: Version,
}

/// What this launcher may do with an installation whose state carries a stamp.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    /// The launcher reads the state and may change it.
    Writable,
    /// The state is a later launcher's: this launcher may still play the game, and changes
    /// nothing.
    ReadOnly,
}

/// Reads the stamp of a JSON state file from `object`, the file's top-level object: its schema,
/// an integer of at least 1, judged first, then the lowest launcher version allowed to change it,
/// a semantic version as a release's own files record one. The file's other keys are its own
/// reader's to judge.
pub fn stamp_of(object: &Map<String, Value>) -> Result<Stamp, StampError> {
    let given = object.get(SCHEMA_KEY);
    let Some(schema) = given
        .and_then(Value::as_u64)
        .filter(|&number| number >= FIRST_SCHEMA)
    else {
        return Err(StampError::Schema {
            found: found(given),
        });
    };
    let given = object.get(MIN_LAUNCHER_KEY);
    let Some(min_launcher) = given
        .and_then(Value::as_str)
        .and_then(|text| Version::parse(text).ok())
    else {
        return Err(StampError::MinLauncher {
            found: found(given),
        });
    };
    Ok(Stamp {
        schema,
        min_launcher,
    })
}

/// Whether this launcher, which reads state files of schemas up to `known_schema` and runs as
/// version `running`, may change a file stamped `stamp`: not when the file's schema is later than
/// any it knows, nor when the file asks for a later launcher. Versions compare by precedence, so
/// build metadata alone never closes a file.
pub fn access(stamp: &Stamp, known_schema: u64, running: &Version) -> Access {
    if stamp.schema > known_schema || stamp.min_launcher > *running {
        Access::ReadOnly
    } else {
        Access::Writable
    }
}

/// What a name directly under `.gdl-launcher/` holds, among the names this launcher's rules make
/// there.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StateName {
    /// `receipt.json`: what the installation holds.
    Receipt,
    /// `lock`: held while a job runs.
    Lock,
    /// `journal`: the folder of the journals of the jobs under way.
    Journal,
    /// `state.json`: the installation's remembered dump paths and launch preferences.
    State,
    /// `disc-files.json`: the size and digest of every disc file the installation copied.
    DiscFiles,
    /// A JSON state file's name with [`PENDING_SUFFIX`] after it: the file's next contents,
    /// written whole before they replace it.
    Pending,
    /// `install-`, `stage-`, `stage-n64-` or `stage-dreamcast-` and a random part: where a job
    /// makes what it will publish.
    Staging,
    /// `download-` and a random part: where an update downloads a launcher file.
    Download,
    /// `update-backup`: what an update or a repair replaced.
    UpdateBackup,
    /// `retired-` and more: what an update or an expansion change put aside.
    Retired,
    /// `bin`: the copy of the window program a Linux launcher runs.
    Bin,
    /// `logs`: the launcher's and the game's logs.
    Logs,
}

impl StateName {
    /// Whether the name is a JSON state file's, whose next contents are written through a pending
    /// file beside it.
    fn is_json_file(self) -> bool {
        matches!(
            self,
            StateName::Receipt | StateName::State | StateName::DiscFiles
        )
    }
}

/// What `name`, a name directly under `.gdl-launcher/`, holds when it is one that this launcher's
/// rules make, in exactly that letter case. Any other name is `None`: a later launcher's or the
/// player's, which this launcher leaves as it is.
pub fn launcher_state_name(name: &str) -> Option<StateName> {
    if let Some(kind) = fixed_name(name) {
        return Some(kind);
    }
    let pending = name.strip_suffix(PENDING_SUFFIX).and_then(fixed_name);
    if pending.is_some_and(StateName::is_json_file) {
        return Some(StateName::Pending);
    }
    let random = RANDOM_PREFIXES
        .into_iter()
        .find(|&(prefix, _)| is_staging_name(prefix, name));
    if let Some((_, kind)) = random {
        return Some(kind);
    }
    name.strip_prefix(RETIRED_PREFIX)
        .filter(|rest| !rest.is_empty())
        .map(|_| StateName::Retired)
}

/// Whether a state folder holding `names`, whose journal folder holds `journal` (`None` when there
/// is none), is one a fresh install left before its first journal: one that holds nothing but what
/// an install makes before one, an empty state folder included. Each of `names` must be the lock,
/// the journal folder, the list of the disc's files, the pending file of that list or of the
/// receipt, or an install's staging, by the name its rules give it; and each name in the journal
/// folder the pending file of the install's journal. A receipt, a journal, or any other name, a
/// later launcher's or the player's, makes it no such folder.
pub(crate) fn abandoned_install(names: &[OsString], journal: Option<&[OsString]>) -> bool {
    let before_journal = |name: &OsString| {
        name.to_str().is_some_and(|name| {
            BEFORE_JOURNAL.contains(&name)
                || name
                    .strip_suffix(PENDING_SUFFIX)
                    .is_some_and(|file| PENDING_BEFORE_JOURNAL.contains(&file))
                || is_staging_name(INSTALL_STAGING_PREFIX, name)
        })
    };
    let pending_journal = |name: &OsString| {
        name.to_str()
            .and_then(|name| name.strip_suffix(PENDING_SUFFIX))
            .is_some_and(|file| file == INSTALL_JOURNAL)
    };
    names.iter().all(before_journal)
        && journal.is_none_or(|names| names.iter().all(pending_journal))
}

/// What `name` holds when it is one of the names this launcher gives its state without a random
/// part.
fn fixed_name(name: &str) -> Option<StateName> {
    FIXED_NAMES
        .into_iter()
        .find(|&(fixed, _)| fixed == name)
        .map(|(_, kind)| kind)
}

/// What a refusal keeps of `value`, the value a state file holds under one of its stamp's keys:
/// its JSON text, cut after [`MAX_SHOWN_CHARS`] characters, since a damaged file may hold anything
/// there.
fn found(value: Option<&Value>) -> Option<Box<str>> {
    value.map(|value| {
        let text = value.to_string();
        match text.char_indices().nth(MAX_SHOWN_CHARS) {
            Some((end, _)) => format!("{}...", &text[..end]).into(),
            None => text.into(),
        }
    })
}

/// How a refusal shows what a state file holds under one of its stamp's keys.
fn shown(found: Option<&str>) -> &str {
    found.unwrap_or("missing")
}

/// Why a state file's stamp cannot be read, which leaves this launcher unable to tell whether it
/// may change the file: the key whose value is missing or of the wrong kind, and what the file
/// holds there, as technical detail for logs.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum StampError {
    /// The file has no schema, or one that is not an integer of at least 1.
    #[error("schema is {}, not an integer of at least 1", shown(.found.as_deref()))]
    Schema {
        /// The JSON text of what the file holds as its schema, cut short when it is long; `None`
        /// when it holds nothing there.
        found: Option<Box<str>>,
    },
    /// The file has no lowest launcher version allowed to change it, or one that is not a
    /// semantic version.
    #[error("minLauncher is {}, not a semantic version", shown(.found.as_deref()))]
    MinLauncher {
        /// The JSON text of what the file holds as that version, cut short when it is long; `None`
        /// when it holds nothing there.
        found: Option<Box<str>>,
    },
}

#[cfg(test)]
mod tests {
    use std::ffi::{OsStr, OsString};
    use std::path::Path;

    use gdl_files::{ALPHA_NAMES, Staging, pending_path};
    use gdl_release::Version;
    use serde_json::{Map, Value, json};

    use super::{
        Access, JOURNAL_NAMES, JOURNAL_SCHEMA, MAX_SHOWN_CHARS, MIN_LAUNCHER, RANDOM_PREFIXES,
        StampError, StateName, abandoned_install, access, launcher_state_name, stamp_of,
    };

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The top-level object of a state file that holds `value`.
    fn object(value: Value) -> Map<String, Value> {
        match value {
            Value::Object(object) => object,
            other => panic!("{other} is not a JSON object"),
        }
    }

    /// The top-level object of a state file stamped with `schema` and `min_launcher`.
    fn stamped(schema: Value, min_launcher: Value) -> Map<String, Value> {
        object(json!({"schema": schema, "minLauncher": min_launcher}))
    }

    /// The refusal of a stamp whose schema holds `found`.
    fn bad_schema(found: Option<&str>) -> Result<Access, StampError> {
        Err(StampError::Schema {
            found: found.map(Into::into),
        })
    }

    /// The refusal of a stamp whose lowest launcher version holds `found`.
    fn bad_min_launcher(found: Option<&str>) -> Result<Access, StampError> {
        Err(StampError::MinLauncher {
            found: found.map(Into::into),
        })
    }

    #[test]
    fn access_table() {
        let first = version(MIN_LAUNCHER);
        let built = version("1.0.0-beta+build.7");
        let later = version("1.0.1");
        let own = || json!(MIN_LAUNCHER);
        let other_keys = object(json!({
            "schema": 1,
            "minLauncher": MIN_LAUNCHER,
            "phase": "staging",
            "future": {"x": 1},
        }));
        for (what, file, known, running, expected) in [
            (
                "this launcher's own stamp",
                stamped(json!(1), own()),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::Writable),
            ),
            (
                "a later schema",
                stamped(json!(2), own()),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::ReadOnly),
            ),
            (
                "an earlier schema than the latest it reads",
                stamped(json!(1), own()),
                2,
                &first,
                Ok(Access::Writable),
            ),
            (
                "the largest schema",
                stamped(json!(u64::MAX), own()),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::ReadOnly),
            ),
            (
                "a later release",
                stamped(json!(1), json!("1.0.1")),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::ReadOnly),
            ),
            (
                "the release after the prerelease",
                stamped(json!(1), json!("1.0.0")),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::ReadOnly),
            ),
            (
                "a later prerelease",
                stamped(json!(1), json!("1.0.0-beta.1")),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::ReadOnly),
            ),
            (
                "an earlier launcher",
                stamped(json!(1), json!("0.1.0-alpha.3")),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::Writable),
            ),
            (
                "its own version with build metadata",
                stamped(json!(1), json!("1.0.0-beta+x")),
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::Writable),
            ),
            (
                "a launcher built with build metadata",
                stamped(json!(1), own()),
                JOURNAL_SCHEMA,
                &built,
                Ok(Access::Writable),
            ),
            (
                "the launcher the file asks for",
                stamped(json!(1), json!("1.0.1")),
                JOURNAL_SCHEMA,
                &later,
                Ok(Access::Writable),
            ),
            (
                "keys of the file's own and a later launcher's",
                other_keys,
                JOURNAL_SCHEMA,
                &first,
                Ok(Access::Writable),
            ),
            (
                "schema 0",
                stamped(json!(0), own()),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(Some("0")),
            ),
            (
                "no schema",
                object(json!({"minLauncher": MIN_LAUNCHER})),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(None),
            ),
            (
                "a schema that is text",
                stamped(json!("1"), own()),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(Some(r#""1""#)),
            ),
            (
                "a negative schema",
                stamped(json!(-1), own()),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(Some("-1")),
            ),
            (
                "a schema that is a fraction",
                stamped(json!(1.5), own()),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(Some("1.5")),
            ),
            (
                "a whole schema written as a fraction",
                stamped(json!(1.0), own()),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(Some("1.0")),
            ),
            (
                "a null schema",
                stamped(Value::Null, own()),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(Some("null")),
            ),
            (
                "a bad schema, judged before a missing version",
                object(json!({"schema": [2]})),
                JOURNAL_SCHEMA,
                &first,
                bad_schema(Some("[2]")),
            ),
            (
                "no version",
                object(json!({"schema": 1})),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(None),
            ),
            (
                "no version in a later schema",
                object(json!({"schema": 2})),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(None),
            ),
            (
                "a tag",
                stamped(json!(1), json!("v1.0.0-beta")),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(Some(r#""v1.0.0-beta""#)),
            ),
            (
                "a version with a space before it",
                stamped(json!(1), json!(" 1.0.0-beta")),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(Some(r#"" 1.0.0-beta""#)),
            ),
            (
                "a version short of its patch",
                stamped(json!(1), json!("1.0")),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(Some(r#""1.0""#)),
            ),
            (
                "an empty version",
                stamped(json!(1), json!("")),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(Some(r#""""#)),
            ),
            (
                "a version that is a number",
                stamped(json!(1), json!(1)),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(Some("1")),
            ),
            (
                "a null version",
                stamped(json!(1), Value::Null),
                JOURNAL_SCHEMA,
                &first,
                bad_min_launcher(Some("null")),
            ),
        ] {
            let judged = stamp_of(&file).map(|stamp| access(&stamp, known, running));
            assert_eq!(judged, expected, "{what}");
        }
    }

    #[test]
    fn a_stamp_keeps_the_version_as_the_file_writes_it() {
        let stamp = stamp_of(&stamped(json!(3), json!("1.2.3-rc.1+build.5")))
            .unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(stamp.schema, 3);
        assert_eq!(stamp.min_launcher.to_string(), "1.2.3-rc.1+build.5");
    }

    #[test]
    fn refusals_show_what_the_file_holds() {
        for (file, shown) in [
            (
                object(json!({})),
                "schema is missing, not an integer of at least 1",
            ),
            (
                stamped(json!("1"), json!(MIN_LAUNCHER)),
                r#"schema is "1", not an integer of at least 1"#,
            ),
            (
                stamped(json!(0), json!(MIN_LAUNCHER)),
                "schema is 0, not an integer of at least 1",
            ),
            (
                object(json!({"schema": 1})),
                "minLauncher is missing, not a semantic version",
            ),
            (
                stamped(json!(1), json!("v1.0.0-beta")),
                r#"minLauncher is "v1.0.0-beta", not a semantic version"#,
            ),
            (
                stamped(json!(1), json!({"major": 1})),
                r#"minLauncher is {"major":1}, not a semantic version"#,
            ),
        ] {
            let error = stamp_of(&file).expect_err(shown);
            assert_eq!(error.to_string(), shown);
        }
        // A long value is kept only in part, cut between two characters, and a value of the
        // longest length kept is kept whole.
        for letter in ["x", "\u{e9}"] {
            let quoted = MAX_SHOWN_CHARS - 2;
            for (version, kept) in [
                (
                    letter.repeat(quoted),
                    format!(r#""{}""#, letter.repeat(quoted)),
                ),
                (
                    letter.repeat(quoted + 1),
                    format!(r#""{}..."#, letter.repeat(quoted + 1)),
                ),
                (
                    letter.repeat(1000),
                    format!(r#""{}..."#, letter.repeat(quoted + 1)),
                ),
            ] {
                let error = stamp_of(&stamped(json!(1), json!(version))).expect_err(letter);
                assert_eq!(
                    error,
                    StampError::MinLauncher {
                        found: Some(kept.into())
                    },
                    "{} of {letter}",
                    version.chars().count()
                );
            }
        }
    }

    #[test]
    fn names_this_launcher_owns() {
        for (name, kind) in [
            ("receipt.json", StateName::Receipt),
            ("lock", StateName::Lock),
            ("journal", StateName::Journal),
            ("state.json", StateName::State),
            ("disc-files.json", StateName::DiscFiles),
            ("receipt.json.pending", StateName::Pending),
            ("state.json.pending", StateName::Pending),
            ("disc-files.json.pending", StateName::Pending),
            ("install-AbC123xy", StateName::Staging),
            ("stage-AbC123xy", StateName::Staging),
            ("stage-n64-AbC123xy", StateName::Staging),
            ("stage-dreamcast-AbC123xy", StateName::Staging),
            ("download-AbC123xy", StateName::Download),
            ("update-backup", StateName::UpdateBackup),
            (
                "retired-launcher-1.0.0-beta-AbC123xy.exe",
                StateName::Retired,
            ),
            ("retired-n64-AbC123xy", StateName::Retired),
            ("retired-x", StateName::Retired),
            ("bin", StateName::Bin),
            ("logs", StateName::Logs),
        ] {
            assert_eq!(launcher_state_name(name), Some(kind), "{name:?}");
        }
        for name in [
            "notes.txt",
            "",
            "install-AbC12",
            "install-AbC123xyz",
            "install-AbC123x-",
            "install-AbC123x\u{e9}",
            "install-",
            "install",
            "stage-n64-AbC12",
            "stage-dreamcast-",
            "download-AbC123xy.part",
            "Receipt.json",
            "RECEIPT.JSON",
            "Lock",
            "Install-AbC123xy",
            "LOGS",
            "lock.pending",
            "journal.pending",
            "update-backup.pending",
            "receipt.json.pending.pending",
            "receipt.pending",
            ".pending",
            "receipt.json.tmp",
            "retired-",
            "retired",
            "update-backup-2",
            "bins",
            "logs.txt",
            " lock",
        ] {
            assert_eq!(launcher_state_name(name), None, "{name:?}");
        }
        // The journals have a folder of their own, and the alpha installer's names are never this
        // launcher's.
        for name in JOURNAL_NAMES.into_iter().chain(ALPHA_NAMES) {
            assert_eq!(launcher_state_name(name), None, "{name:?}");
        }
        assert_eq!(
            JOURNAL_NAMES,
            [
                "install.json",
                "update.json",
                "expansion-n64.json",
                "expansion-dreamcast.json"
            ]
        );
    }

    #[test]
    fn the_names_its_jobs_make_are_its_own() {
        let dest = tempfile::tempdir().unwrap();
        for (prefix, kind) in RANDOM_PREFIXES {
            let staging = Staging::create_in(dest.path(), prefix)
                .unwrap_or_else(|error| panic!("{prefix}: {error}"));
            let name = staging
                .path()
                .file_name()
                .and_then(OsStr::to_str)
                .expect("a staging folder's name is text");
            assert_eq!(launcher_state_name(name), Some(kind), "{name:?}");
        }
        for name in ["receipt.json", "state.json", "disc-files.json"] {
            let pending = pending_path(Path::new(name));
            let pending = pending.to_str().expect("a pending file's name is text");
            assert_eq!(
                launcher_state_name(pending),
                Some(StateName::Pending),
                "{pending:?}"
            );
        }
    }

    /// `names`, as a folder's listing gives them.
    fn listed(names: &[&str]) -> Vec<OsString> {
        names.iter().map(OsString::from).collect()
    }

    #[test]
    fn a_state_folder_left_before_its_journal_holds_only_what_an_install_makes_before_one() {
        let pending_journal = listed(&["install.json.pending"]);
        let empty = listed(&[]);
        let journal = listed(&["install.json"]);
        let other_pending = listed(&["update.json.pending"]);
        for (what, names, journals, abandoned) in [
            ("an empty state folder", vec![], None, true),
            ("the lock", vec!["lock"], None, true),
            (
                "the lock and an empty journal folder",
                vec!["journal", "lock"],
                Some(&empty),
                true,
            ),
            (
                "the journal's pending file",
                vec!["journal", "lock"],
                Some(&pending_journal),
                true,
            ),
            (
                "a staging no journal names",
                vec!["install-AbC123xy", "lock"],
                None,
                true,
            ),
            (
                "the list of the disc's files and the pending files",
                vec![
                    "disc-files.json",
                    "disc-files.json.pending",
                    "lock",
                    "receipt.json.pending",
                ],
                None,
                true,
            ),
            ("a receipt", vec!["lock", "receipt.json"], None, false),
            ("a journal", vec!["journal", "lock"], Some(&journal), false),
            (
                "another job's pending journal",
                vec!["journal", "lock"],
                Some(&other_pending),
                false,
            ),
            (
                "a name of the player's",
                vec!["lock", "notes.txt"],
                None,
                false,
            ),
            (
                "an update's backup",
                vec!["lock", "update-backup"],
                None,
                false,
            ),
            (
                "another job's staging",
                vec!["lock", "stage-AbC123xy"],
                None,
                false,
            ),
            (
                "a name that is no staging's",
                vec!["install-AbC12", "lock"],
                None,
                false,
            ),
            (
                "a pending file of no state file of the install's",
                vec!["lock", "state.json.pending"],
                None,
                false,
            ),
            ("a name in another letter case", vec!["Lock"], None, false),
        ] {
            let names = listed(&names);
            let journals = journals.map(Vec::as_slice);
            assert_eq!(abandoned_install(&names, journals), abandoned, "{what}");
        }
        // A name that is not text is none of the launcher's.
        #[cfg(unix)]
        {
            use std::os::unix::ffi::OsStringExt;

            let odd = OsString::from_vec(vec![b'l', 0xFF]);
            assert!(!abandoned_install(std::slice::from_ref(&odd), None));
            assert!(!abandoned_install(&listed(&["journal"]), Some(&[odd])));
        }
    }
}
