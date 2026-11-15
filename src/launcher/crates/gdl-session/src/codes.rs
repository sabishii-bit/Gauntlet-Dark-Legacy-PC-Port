//! The codes of the problems the launcher's page shows, which its string table turns into words.
//! The crates below report a refusal by its kind and its values; this is the one place that gives
//! each refusal its code, so every code the page needs words for is in [`CODES`]. A job asked to
//! stop has no code: it ends as cancelled rather than failed, so it is never shown as a problem or
//! retried as one.

use gdl_install::{
    FolderError, FolderState, GameStartError, GameUseError, LockError, PayloadError, ReceiptError,
    SectionsError,
};

use crate::privilege::Refusal;

/// A runtime zip, or the launcher file that carries one, breaks a rule the launcher holds it to,
/// or does not hold the bytes its records give.
const PAYLOAD_DAMAGED: &str = "payload.damaged";

/// Looking at, reading or writing a file or folder failed.
const IO_FAILED: &str = "io.failed";

/// The installation's folder, a folder above it, or an entry in it that the launcher reads or
/// changes is a link or a junction.
const FOLDER_LINKS: &str = "folder.links";

/// Nothing, or something other than a folder, is at the installation's path.
const FOLDER_NOT_DIRECTORY: &str = "folder.notDirectory";

/// The launcher may not write where it must, such as the game's executable.
const FOLDER_NOT_WRITABLE: &str = "folder.notWritable";

/// The folder holds an installation that the alpha's installer made, which the launcher leaves as
/// it is.
const FOLDER_ALPHA_INSTALL: &str = "folder.alphaInstall";

/// Another job holds the installation's lock.
const LOCK_BUSY: &str = "lock.busy";

/// The game is running from the installation.
const GAME_RUNNING: &str = "game.running";

/// The installation's executable is not there.
const GAME_MISSING: &str = "game.missing";

/// The game did not start and print its version.
const GAME_PROBE_FAILED: &str = "game.probeFailed";

/// The launcher's receipt cannot be read, or disagrees with the files it pins.
const INSTALL_RECEIPT_INVALID: &str = "install.receiptInvalid";

/// The installation is for another platform than the one the launcher was built for.
const INSTALL_OTHER_PLATFORM: &str = "install.otherPlatform";

/// A later launcher wrote the installation's state: the game stays playable, and this launcher
/// changes nothing.
const STATE_NEWER_LAUNCHER: &str = "state.newerLauncher";

/// A journal of a job left unfinished cannot be read, so that job is left as it is.
const JOURNAL_UNREADABLE: &str = "journal.unreadable";

/// The window does not start with an administrator's elevated token.
const ELEVATED_REFUSED: &str = "elevated.refused";

/// The window does not start as root unless the player allows it.
const ROOT_REFUSED: &str = "root.refused";

/// Every code this module's functions give, each once: the codes the page's string table holds
/// words for.
pub const CODES: [&str; 16] = [
    PAYLOAD_DAMAGED,
    IO_FAILED,
    FOLDER_LINKS,
    FOLDER_NOT_DIRECTORY,
    FOLDER_NOT_WRITABLE,
    FOLDER_ALPHA_INSTALL,
    LOCK_BUSY,
    GAME_RUNNING,
    GAME_MISSING,
    GAME_PROBE_FAILED,
    INSTALL_RECEIPT_INVALID,
    INSTALL_OTHER_PLATFORM,
    STATE_NEWER_LAUNCHER,
    JOURNAL_UNREADABLE,
    ELEVATED_REFUSED,
    ROOT_REFUSED,
];

/// What a refusal met while reading a runtime, or the launcher file that carries one, reports.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Report {
    /// The job was asked to stop, and ends as cancelled.
    Cancelled,
    /// The job fails with the problem this code names.
    Error(&'static str),
}

// Each match below names every variant rather than ending in a wildcard, so that a refusal added
// later does not build until it is given its code on purpose: what the page does next goes by it.

/// The code a refusal of a runtime zip reports. A cancel has none, and the system's failure to read
/// the archive or to write a copy of a file is `io.failed`. A refusal of the launcher file's
/// sections, which carry the runtime, reports what [`sections`] gives it. Any other refusal is
/// `payload.damaged`.
pub fn payload(error: &PayloadError) -> Report {
    match error {
        PayloadError::Cancelled => Report::Cancelled,
        PayloadError::Io(_) => Report::Error(IO_FAILED),
        PayloadError::Sections(error) => sections(error),
        PayloadError::Archive
        | PayloadError::Zip64
        | PayloadError::Rows { .. }
        | PayloadError::TooManyEntries { .. }
        | PayloadError::TooLarge { .. }
        | PayloadError::Encrypted { .. }
        | PayloadError::Method { .. }
        | PayloadError::DataDescriptor { .. }
        | PayloadError::LocalHeader { .. }
        | PayloadError::Name { .. }
        | PayloadError::Directory { .. }
        | PayloadError::Link { .. }
        | PayloadError::Duplicate { .. }
        | PayloadError::Conflict { .. }
        | PayloadError::UnknownRoot { .. }
        | PayloadError::FileAsFolder { .. }
        | PayloadError::FolderAsFile { .. }
        | PayloadError::PlatformRoot { .. }
        | PayloadError::DataNotJson { .. }
        | PayloadError::Missing { .. }
        | PayloadError::BuildInfoTooLarge { .. }
        | PayloadError::BuildInfo { .. }
        | PayloadError::Version { .. }
        | PayloadError::Platform { .. }
        | PayloadError::Executable { .. }
        | PayloadError::Digest { .. }
        | PayloadError::Inventory { .. }
        | PayloadError::VersionFile
        | PayloadError::Damaged { .. }
        | PayloadError::OtherPlatform { .. }
        | PayloadError::OtherVersion { .. } => Report::Error(PAYLOAD_DAMAGED),
    }
}

/// The code a refusal of the launcher file's sections reports. A cancel has none, and the system's
/// failure to read the file is `io.failed`. A file that breaks the sections' rules is
/// `payload.damaged`, since the runtime it carries is then damaged too.
pub fn sections(error: &SectionsError) -> Report {
    match error {
        SectionsError::Cancelled => Report::Cancelled,
        SectionsError::Io(_) => Report::Error(IO_FAILED),
        SectionsError::UnknownProgram
        | SectionsError::PeHeaders { .. }
        | SectionsError::ElfHeaders { .. }
        | SectionsError::CertificatesUnaligned
        | SectionsError::CertificatesNotAtEnd
        | SectionsError::CertificatesInsideImage
        | SectionsError::TableLength { .. }
        | SectionsError::TableHash
        | SectionsError::TableJson { .. }
        | SectionsError::Schema { .. }
        | SectionsError::ProgramLength { .. }
        | SectionsError::SectionName { .. }
        | SectionsError::SectionDigestText { .. }
        | SectionsError::SectionTooLarge { .. }
        | SectionsError::SectionPlace { .. }
        | SectionsError::TableNotAfterSections
        | SectionsError::SectionDigest { .. }
        | SectionsError::MissingSection { .. }
        | SectionsError::NoSections => Report::Error(PAYLOAD_DAMAGED),
    }
}

/// The code a refusal of an installation's receipt, or of the files it pins, reports: a receipt
/// that cannot be read and one that disagrees with the files are both `install.receiptInvalid`.
pub fn receipt(error: &ReceiptError) -> &'static str {
    match error {
        ReceiptError::Links { .. } => FOLDER_LINKS,
        ReceiptError::Unreadable { .. } | ReceiptError::Disagrees { .. } => INSTALL_RECEIPT_INVALID,
        ReceiptError::OtherPlatform { .. } => INSTALL_OTHER_PLATFORM,
        ReceiptError::GameMissing { .. } => GAME_MISSING,
        ReceiptError::Io { .. } => IO_FAILED,
    }
}

/// The code a refusal of a folder as an installation's reports: an absent folder, like a file, is
/// `folder.notDirectory`.
pub fn folder(error: &FolderError) -> &'static str {
    match error {
        FolderError::Links { .. } => FOLDER_LINKS,
        FolderError::NotDirectory { .. } | FolderError::Absent { .. } => FOLDER_NOT_DIRECTORY,
        FolderError::Io { .. } => IO_FAILED,
    }
}

/// The code a refusal of an installation's lock reports. `lock.busy` is a hold rather than a
/// failure: the job can wait for the one that holds the lock. A missing state folder is
/// `io.failed`, like any failure to open or lock the lock file.
pub fn lock(error: &LockError) -> &'static str {
    match error {
        LockError::Busy => LOCK_BUSY,
        LockError::Links { .. } => FOLDER_LINKS,
        LockError::NoStateFolder { .. } | LockError::Io { .. } => IO_FAILED,
    }
}

/// The code a refusal to change an installation's game reports. `game.running` is a hold rather
/// than a failure: the job can wait for the game to end.
pub fn game_use(error: &GameUseError) -> &'static str {
    match error {
        GameUseError::Running => GAME_RUNNING,
        GameUseError::Missing => GAME_MISSING,
        GameUseError::NotWritable { .. } => FOLDER_NOT_WRITABLE,
        GameUseError::Io { .. } => IO_FAILED,
    }
}

/// The code a failed version probe of the game reports, whatever stopped it.
pub fn game_start(error: &GameStartError) -> &'static str {
    match error {
        GameStartError::NotAbsolute
        | GameStartError::Start(_)
        | GameStartError::TimedOut
        | GameStartError::Io(_) => GAME_PROBE_FAILED,
    }
}

/// The code of the problem a folder's state shows, or `None` when the state is no problem: a folder
/// to install into, a job to recover, or an installation the launcher made and may change.
pub fn state(state: FolderState) -> Option<&'static str> {
    match state {
        FolderState::Absent
        | FolderState::Empty
        | FolderState::NotInstalled
        | FolderState::Pending
        | FolderState::Installed => None,
        FolderState::NotDirectory => Some(FOLDER_NOT_DIRECTORY),
        FolderState::Links => Some(FOLDER_LINKS),
        FolderState::Unreadable => Some(IO_FAILED),
        FolderState::JournalUnreadable => Some(JOURNAL_UNREADABLE),
        FolderState::AlphaInstall => Some(FOLDER_ALPHA_INSTALL),
        FolderState::ReceiptInvalid => Some(INSTALL_RECEIPT_INVALID),
        FolderState::OtherPlatform => Some(INSTALL_OTHER_PLATFORM),
        FolderState::GameMissing => Some(GAME_MISSING),
        FolderState::NewerLauncher => Some(STATE_NEWER_LAUNCHER),
    }
}

/// The code of the token or user the window refuses to start under.
pub fn refusal(refusal: Refusal) -> &'static str {
    match refusal {
        Refusal::Elevated => ELEVATED_REFUSED,
        Refusal::Root => ROOT_REFUSED,
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::io;
    use std::path::PathBuf;

    use gdl_install::{
        BuildInfo, FolderError, FolderState, GameStartError, GameUseError, LockError, PayloadError,
        ReceiptError, SectionsError,
    };
    use gdl_release::Platform;

    use super::{
        CODES, Report, folder, game_start, game_use, lock, payload, receipt, refusal, sections,
        state,
    };
    use crate::privilege::Refusal;

    /// What a broken rule of a runtime, or of the launcher file that carries it, reports.
    const DAMAGED: Report = Report::Error("payload.damaged");

    /// What a failure to read or copy a runtime, or to read the launcher file, reports.
    const FAILED: Report = Report::Error("io.failed");

    /// Every state a folder can be in, with the code of the problem it shows.
    const STATES: [(FolderState, Option<&str>); 14] = [
        (FolderState::Absent, None),
        (FolderState::NotDirectory, Some("folder.notDirectory")),
        (FolderState::Links, Some("folder.links")),
        (FolderState::Unreadable, Some("io.failed")),
        (FolderState::JournalUnreadable, Some("journal.unreadable")),
        (FolderState::AlphaInstall, Some("folder.alphaInstall")),
        (FolderState::Empty, None),
        (FolderState::NotInstalled, None),
        (FolderState::Pending, None),
        (FolderState::Installed, None),
        (FolderState::ReceiptInvalid, Some("install.receiptInvalid")),
        (FolderState::OtherPlatform, Some("install.otherPlatform")),
        (FolderState::GameMissing, Some("game.missing")),
        (FolderState::NewerLauncher, Some("state.newerLauncher")),
    ];

    /// Every token or user the window refuses to start under, with its code.
    const REFUSALS: [(Refusal, &str); 2] = [
        (Refusal::Elevated, "elevated.refused"),
        (Refusal::Root, "root.refused"),
    ];

    /// A path that a refusal names.
    fn path() -> PathBuf {
        PathBuf::from("Gauntlet Dark Legacy")
    }

    /// A name that a refusal names.
    fn name() -> Box<str> {
        "data/config.json".into()
    }

    /// What a refusal says is wrong.
    fn detail() -> Box<str> {
        "what was found".into()
    }

    /// The system's refusal to let a path be looked at or read.
    fn denied() -> io::Error {
        io::Error::from(io::ErrorKind::PermissionDenied)
    }

    /// The refusal a runtime's release record gets for an inventory that names a file outside the
    /// runtime, as the record's reader gives it.
    fn name_refusal() -> PayloadError {
        let record =
            br#"{"version": "1.0.0-beta", "commit": "test-commit", "platform": "linux-x64",
            "executable": "gauntlet", "files": {"../escaped": ""}}"#;
        let error = BuildInfo::parse(record).expect_err("a name outside the runtime is refused");
        assert!(matches!(error, PayloadError::Name { .. }), "{error}");
        error
    }

    /// One refusal of every kind a runtime zip gets, with what each reports. A refusal of the
    /// launcher file's sections comes once for each report the sections give.
    fn payload_refusals() -> Vec<(PayloadError, Report)> {
        vec![
            (PayloadError::Archive, DAMAGED),
            (PayloadError::Zip64, DAMAGED),
            (PayloadError::Rows { detail: detail() }, DAMAGED),
            (PayloadError::TooManyEntries { count: 20_001 }, DAMAGED),
            (PayloadError::TooLarge { bytes: 7 }, DAMAGED),
            (PayloadError::Encrypted { name: name() }, DAMAGED),
            (
                PayloadError::Method {
                    name: name(),
                    method: 12,
                },
                DAMAGED,
            ),
            (PayloadError::DataDescriptor { name: name() }, DAMAGED),
            (PayloadError::LocalHeader { detail: detail() }, DAMAGED),
            (name_refusal(), DAMAGED),
            (PayloadError::Directory { name: name() }, DAMAGED),
            (PayloadError::Link { name: name() }, DAMAGED),
            (PayloadError::Duplicate { name: name() }, DAMAGED),
            (PayloadError::Conflict { name: name() }, DAMAGED),
            (PayloadError::UnknownRoot { name: name() }, DAMAGED),
            (PayloadError::FileAsFolder { name: name() }, DAMAGED),
            (PayloadError::FolderAsFile { name: name() }, DAMAGED),
            (PayloadError::PlatformRoot { name: name() }, DAMAGED),
            (PayloadError::DataNotJson { name: name() }, DAMAGED),
            (PayloadError::Missing { name: name() }, DAMAGED),
            (PayloadError::BuildInfoTooLarge { bytes: 7 }, DAMAGED),
            (PayloadError::BuildInfo { detail: detail() }, DAMAGED),
            (
                PayloadError::Version {
                    text: "v1.0".into(),
                },
                DAMAGED,
            ),
            (
                PayloadError::Platform {
                    text: "another-os".into(),
                },
                DAMAGED,
            ),
            (
                PayloadError::Executable {
                    name: "game".into(),
                },
                DAMAGED,
            ),
            (PayloadError::Digest { name: name() }, DAMAGED),
            (PayloadError::Inventory { name: name() }, DAMAGED),
            (PayloadError::VersionFile, DAMAGED),
            (
                PayloadError::Damaged {
                    name: name(),
                    detail: detail(),
                },
                DAMAGED,
            ),
            (
                PayloadError::OtherPlatform {
                    platform: Platform::LinuxX64,
                },
                DAMAGED,
            ),
            (
                PayloadError::OtherVersion {
                    found: "1.0.1".into(),
                    expected: "1.0.0-beta".into(),
                },
                DAMAGED,
            ),
            (PayloadError::Cancelled, Report::Cancelled),
            (
                PayloadError::Sections(SectionsError::Cancelled),
                Report::Cancelled,
            ),
            (PayloadError::Sections(SectionsError::Io(denied())), FAILED),
            (
                PayloadError::Sections(SectionsError::MissingSection {
                    name: "runtime.zip".into(),
                }),
                DAMAGED,
            ),
            (PayloadError::Io(denied()), FAILED),
        ]
    }

    /// One refusal of every kind the launcher file's sections get, with what each reports.
    fn sections_refusals() -> Vec<(SectionsError, Report)> {
        let section = || -> Box<str> { "runtime.zip".into() };
        vec![
            (SectionsError::Io(denied()), FAILED),
            (SectionsError::UnknownProgram, DAMAGED),
            (
                SectionsError::PeHeaders {
                    detail: "PE headers run past the file",
                },
                DAMAGED,
            ),
            (
                SectionsError::ElfHeaders {
                    detail: "ELF ranges run past the file",
                },
                DAMAGED,
            ),
            (SectionsError::CertificatesUnaligned, DAMAGED),
            (SectionsError::CertificatesNotAtEnd, DAMAGED),
            (SectionsError::CertificatesInsideImage, DAMAGED),
            (SectionsError::TableLength { len: 0 }, DAMAGED),
            (SectionsError::TableHash, DAMAGED),
            (SectionsError::TableJson { detail: detail() }, DAMAGED),
            (SectionsError::Schema { found: 2 }, DAMAGED),
            (
                SectionsError::ProgramLength {
                    recorded: 0x200,
                    actual: 0x400,
                },
                DAMAGED,
            ),
            (SectionsError::SectionName { name: section() }, DAMAGED),
            (
                SectionsError::SectionDigestText { name: section() },
                DAMAGED,
            ),
            (SectionsError::SectionTooLarge { name: section() }, DAMAGED),
            (
                SectionsError::SectionPlace {
                    name: section(),
                    offset: 0x200,
                    size: 0x100,
                    reason: "does not start where the program or the section before it ends",
                },
                DAMAGED,
            ),
            (SectionsError::TableNotAfterSections, DAMAGED),
            (SectionsError::SectionDigest { name: section() }, DAMAGED),
            (SectionsError::MissingSection { name: section() }, DAMAGED),
            (SectionsError::NoSections, DAMAGED),
            (SectionsError::Cancelled, Report::Cancelled),
        ]
    }

    /// One refusal of every kind an installation's receipt gets, with its code.
    fn receipt_refusals() -> Vec<(ReceiptError, &'static str)> {
        vec![
            (ReceiptError::Links { path: path() }, "folder.links"),
            (
                ReceiptError::Unreadable { detail: detail() },
                "install.receiptInvalid",
            ),
            (
                ReceiptError::Disagrees { field: "version" },
                "install.receiptInvalid",
            ),
            (
                ReceiptError::OtherPlatform {
                    platform: Platform::LinuxX64,
                },
                "install.otherPlatform",
            ),
            (
                ReceiptError::GameMissing { executable: path() },
                "game.missing",
            ),
            (
                ReceiptError::Io {
                    path: path(),
                    source: denied(),
                },
                "io.failed",
            ),
        ]
    }

    /// One refusal of every kind a folder gets as an installation's, with its code.
    fn folder_refusals() -> Vec<(FolderError, &'static str)> {
        vec![
            (FolderError::Links { path: path() }, "folder.links"),
            (
                FolderError::NotDirectory { path: path() },
                "folder.notDirectory",
            ),
            (FolderError::Absent { path: path() }, "folder.notDirectory"),
            (
                FolderError::Io {
                    path: path(),
                    source: denied(),
                },
                "io.failed",
            ),
        ]
    }

    /// One refusal of every kind an installation's lock gets, with its code.
    fn lock_refusals() -> Vec<(LockError, &'static str)> {
        vec![
            (LockError::Busy, "lock.busy"),
            (LockError::Links { path: path() }, "folder.links"),
            (LockError::NoStateFolder { path: path() }, "io.failed"),
            (
                LockError::Io {
                    path: path(),
                    source: denied(),
                },
                "io.failed",
            ),
        ]
    }

    /// One refusal of every kind a change to an installation's game gets, with its code.
    fn game_use_refusals() -> Vec<(GameUseError, &'static str)> {
        vec![
            (GameUseError::Running, "game.running"),
            (GameUseError::Missing, "game.missing"),
            (
                GameUseError::NotWritable { path: path() },
                "folder.notWritable",
            ),
            (
                GameUseError::Io {
                    path: path(),
                    source: denied(),
                },
                "io.failed",
            ),
        ]
    }

    /// One failure of every kind the game's version probe meets, with its code.
    fn game_start_refusals() -> Vec<(GameStartError, &'static str)> {
        vec![
            (GameStartError::NotAbsolute, "game.probeFailed"),
            (GameStartError::Start(denied()), "game.probeFailed"),
            (GameStartError::TimedOut, "game.probeFailed"),
            (GameStartError::Io(denied()), "game.probeFailed"),
        ]
    }

    #[test]
    fn every_refusal_maps_to_its_code() {
        for (error, expected) in payload_refusals() {
            assert_eq!(payload(&error), expected, "{}: {error}", error.rule());
        }
        for (error, expected) in sections_refusals() {
            assert_eq!(sections(&error), expected, "{}: {error}", error.rule());
        }
        for (error, expected) in receipt_refusals() {
            assert_eq!(receipt(&error), expected, "{error:?}");
        }
        for (error, expected) in folder_refusals() {
            assert_eq!(folder(&error), expected, "{error:?}");
        }
        for (error, expected) in lock_refusals() {
            assert_eq!(lock(&error), expected, "{error:?}");
        }
        for (error, expected) in game_use_refusals() {
            assert_eq!(game_use(&error), expected, "{error:?}");
        }
        for (error, expected) in game_start_refusals() {
            assert_eq!(game_start(&error), expected, "{error:?}");
        }
        for (folder_state, expected) in STATES {
            assert_eq!(state(folder_state), expected, "{}", folder_state.as_str());
        }
        for (refused, expected) in REFUSALS {
            assert_eq!(refusal(refused), expected, "{refused:?}");
        }
    }

    #[test]
    fn a_runtime_refused_for_its_sections_reports_what_the_sections_do() {
        // The runtime is read from the launcher file's sections, so a cancel or a failed read
        // there reports as the sections report it, never as a damaged payload.
        for (error, _) in sections_refusals() {
            let expected = sections(&error);
            let error = PayloadError::Sections(error);
            assert_eq!(payload(&error), expected, "{}: {error}", error.rule());
        }
    }

    #[test]
    fn the_code_list_is_exactly_what_the_mappings_return() {
        let listed: BTreeSet<&str> = CODES.into_iter().collect();
        assert_eq!(
            listed.len(),
            CODES.len(),
            "a code is listed twice: {CODES:?}"
        );
        let code_of = |report: Report| match report {
            Report::Error(code) => Some(code),
            Report::Cancelled => None,
        };
        let mut returned = BTreeSet::new();
        returned.extend(
            payload_refusals()
                .iter()
                .filter_map(|(error, _)| code_of(payload(error))),
        );
        returned.extend(
            sections_refusals()
                .iter()
                .filter_map(|(error, _)| code_of(sections(error))),
        );
        returned.extend(receipt_refusals().iter().map(|(error, _)| receipt(error)));
        returned.extend(folder_refusals().iter().map(|(error, _)| folder(error)));
        returned.extend(lock_refusals().iter().map(|(error, _)| lock(error)));
        returned.extend(game_use_refusals().iter().map(|(error, _)| game_use(error)));
        returned.extend(
            game_start_refusals()
                .iter()
                .map(|(error, _)| game_start(error)),
        );
        returned.extend(
            STATES
                .into_iter()
                .filter_map(|(folder_state, _)| state(folder_state)),
        );
        returned.extend(REFUSALS.into_iter().map(|(refused, _)| refusal(refused)));
        assert_eq!(returned, listed);
    }
}
