//! The codes of the problems the launcher's page shows, which its string table turns into words.
//! The crates below report a refusal by its kind and its values; this is the one place that gives
//! each refusal its code, so every code the page needs words for is in [`CODES`]. A job asked to
//! stop has no code: it ends as cancelled rather than failed, so it is never shown as a problem or
//! retried as one.

use std::io;

use gdl_extract::ExtractError;
use gdl_files::FilesError;
use gdl_install::{
    FolderError, FolderState, GameStartError, GameUseError, LockError, PayloadError, ReceiptError,
    SectionsError,
};
use gdl_media::{DiscRule, DumpRefusal, MediaError, RomRule};

use crate::privilege::Refusal;

/// A runtime zip, or the launcher file that carries one, breaks a rule the launcher holds it to,
/// or does not hold the bytes its records give.
const PAYLOAD_DAMAGED: &str = "payload.damaged";

/// Looking at, reading or writing a file or folder failed.
const IO_FAILED: &str = "io.failed";

/// Writing a file found the disk full or the quota spent.
const SPACE_RAN_OUT: &str = "space.ranOut";

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

/// Something is already where the launcher was to put what it made, and the launcher never
/// replaces what it did not make.
const FOLDER_COLLISION: &str = "folder.collision";

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

/// The dump is nothing the launcher knows: its content is that of no container the launcher reads,
/// nor of one it knows to refuse.
const SOURCE_UNRECOGNIZED: &str = "source.unrecognized";

/// The dump is of another game.
const SOURCE_OTHER_GAME: &str = "source.otherGame";

/// The dump is of the game, but of another region or revision than the one the launcher installs
/// from.
const SOURCE_OTHER_REVISION: &str = "source.otherRevision";

/// The dump comes in a container the launcher does not read, such as a compressed disc image or a
/// cue sheet that keeps the whole disc in one file.
const SOURCE_UNSUPPORTED_CONTAINER: &str = "source.unsupportedContainer";

/// The dump is a DiscJuggler image, a copy rebuilt for a CD-R, rather than a dump of the GD-ROM.
const DUMP_DC_CDI: &str = "dump.dc.cdi";

/// The dump is an archive, to unpack first.
const SOURCE_ARCHIVE: &str = "source.archive";

/// Part of the dump is not there: a file it needs is missing, or ends before what it must hold.
const SOURCE_INCOMPLETE: &str = "source.incomplete";

/// The dump breaks a rule of its format, or holds files that would make a tree that breaks one.
const SOURCE_DAMAGED: &str = "source.damaged";

/// The dump's bytes, or a file's, are not the ones the launcher holds them to, or a file of the
/// dump changed while it was read.
const DUMP_HASH_MISMATCH: &str = "dump.hashMismatch";

/// Every code this module's functions give, each once: the codes the page's string table holds
/// words for.
pub const CODES: [&str; 27] = [
    PAYLOAD_DAMAGED,
    IO_FAILED,
    SPACE_RAN_OUT,
    FOLDER_LINKS,
    FOLDER_NOT_DIRECTORY,
    FOLDER_NOT_WRITABLE,
    FOLDER_ALPHA_INSTALL,
    FOLDER_COLLISION,
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
    SOURCE_UNRECOGNIZED,
    SOURCE_OTHER_GAME,
    SOURCE_OTHER_REVISION,
    SOURCE_UNSUPPORTED_CONTAINER,
    DUMP_DC_CDI,
    SOURCE_ARCHIVE,
    SOURCE_INCOMPLETE,
    SOURCE_DAMAGED,
    DUMP_HASH_MISMATCH,
];

/// What a refusal a job meets reports: that the job was asked to stop, or the problem it fails
/// with.
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

/// The code a failure to read a dump reports. A cancel has none, and the system's failure to read
/// the dump is `io.failed`. A disc file's copy whose writer would not take its bytes is
/// `space.ranOut` when the writer found no room, else `io.failed`. A dump that breaks a rule of
/// its format reports that rule's code: one with a part that is not there is `source.incomplete`,
/// one in a container the launcher does not read `source.unsupportedContainer`, a disc of another
/// game `source.otherGame`, and a file that is no dump the launcher knows `source.unrecognized`.
/// Any other broken rule is `source.damaged`.
pub fn media(error: &MediaError) -> Report {
    match error {
        MediaError::Cancelled => Report::Cancelled,
        MediaError::Io { .. } => Report::Error(IO_FAILED),
        MediaError::Write { source, .. } => Report::Error(written(source)),
        MediaError::Rom { rule, .. } => Report::Error(rom_rule(*rule)),
        MediaError::Disc { rule, .. } => Report::Error(disc_rule(*rule)),
    }
}

/// The code a dump refused for its container or its identity reports.
pub fn dump_refusal(refusal: DumpRefusal) -> &'static str {
    match refusal {
        DumpRefusal::OtherGame => SOURCE_OTHER_GAME,
        DumpRefusal::OtherRevision => SOURCE_OTHER_REVISION,
        DumpRefusal::HashMismatch => DUMP_HASH_MISMATCH,
        DumpRefusal::UnsupportedContainer => SOURCE_UNSUPPORTED_CONTAINER,
        DumpRefusal::Cdi => DUMP_DC_CDI,
        DumpRefusal::Archive => SOURCE_ARCHIVE,
        DumpRefusal::Unrecognized => SOURCE_UNRECOGNIZED,
    }
}

/// The code a refusal of one of the launcher's file operations reports, such as making a staging
/// and writing into it. A name taken where a staged root was to go is `folder.collision`, and a
/// write that found the disk full or the quota spent is `space.ranOut`. A link is `io.failed`:
/// one found among what the launcher made was put there by something else, and a job that checks
/// a path the player or a developer chose reports a link on it as `folder.links` itself. Any other
/// refusal is `io.failed`.
pub fn files(error: &FilesError) -> &'static str {
    match error {
        FilesError::Io { source, .. } => written(source),
        FilesError::Exists { .. } => FOLDER_COLLISION,
        FilesError::Link { .. }
        | FilesError::TooLarge { .. }
        | FilesError::Json { .. }
        | FilesError::NotRoot { .. } => IO_FAILED,
    }
}

/// The code an extraction's failure reports. A cancel has none. A reader's failure reports what
/// [`media`] gives it, the staging's what [`files`] gives it, and a dump an installation's trust
/// does not name what [`dump_refusal`] gives its refusal. Writing or reading back a staged file is
/// `space.ranOut` when the disk was full or the quota spent, else `io.failed`. A dump whose tree
/// would break a rule that keeps it one tree on every system, or whose native table is missing,
/// too large or cut short, is `source.damaged`; a disc whose files do not match the fingerprints
/// the trust holds, or that gave other bytes to be written than to be checked, is
/// `dump.hashMismatch`.
pub fn extract(error: &ExtractError) -> Report {
    match error {
        ExtractError::Cancelled => Report::Cancelled,
        ExtractError::Media(error) => media(error),
        ExtractError::Files(error) => Report::Error(files(error)),
        ExtractError::Io { source, .. } => Report::Error(written(source)),
        ExtractError::Tree { .. } | ExtractError::Native { .. } => Report::Error(SOURCE_DAMAGED),
        ExtractError::Fingerprint { .. } | ExtractError::Changed { .. } => {
            Report::Error(DUMP_HASH_MISMATCH)
        }
        ExtractError::Refused { refusal } => Report::Error(dump_refusal(*refusal)),
    }
}

/// The code of a rule of the N64 ROM's format: a ROM shorter than its header, or cut off within a
/// word, is `source.incomplete`, and a file whose first word is that of no byte order a cartridge
/// is kept in is no ROM the launcher knows, `source.unrecognized`. Every other rule is
/// `source.damaged`.
fn rom_rule(rule: RomRule) -> &'static str {
    match rule {
        RomRule::Header => SOURCE_INCOMPLETE,
        RomRule::ByteOrder => SOURCE_UNRECOGNIZED,
        RomRule::RomTooLarge
        | RomRule::RomdiskHeader
        | RomRule::OutsideData
        | RomRule::DirectoryCount
        | RomRule::DirectoryOverlap
        | RomRule::RecordPlace
        | RomRule::UnterminatedPath
        | RomRule::NonAsciiPath
        | RomRule::LeadingSlash
        | RomRule::UnsafePath
        | RomRule::DuplicatePath
        | RomRule::Collision
        | RomRule::Flags
        | RomRule::FileSize
        | RomRule::FileOverlap
        | RomRule::ExpandedSize
        | RomRule::Deflate
        | RomRule::DeflateEnd
        | RomRule::DeflateTrailer
        | RomRule::OverlayPadding
        | RomRule::OverlayMetadata
        | RomRule::OverlayName
        | RomRule::WadHeader
        | RomRule::WadDirectory
        | RomRule::WadSection => SOURCE_DAMAGED,
    }
}

/// The code of a rule of the Dreamcast disc's format, of the sheet that lists its track files, or
/// of the raw sectors they keep: a rule that tells of a part of the disc that is not there is
/// `source.incomplete`, one that tells of a container the launcher does not read
/// `source.unsupportedContainer`, the boot area of another game `source.otherGame`, and a file too
/// long to be a sheet `source.unrecognized`. Every other rule is `source.damaged`.
fn disc_rule(rule: DiscRule) -> &'static str {
    match rule {
        DiscRule::MissingTrack
        | DiscRule::TruncatedTrack
        | DiscRule::ShortRead
        | DiscRule::NoSheet => SOURCE_INCOMPLETE,
        DiscRule::TrackPerFile
        | DiscRule::TrackMode
        | DiscRule::GdiSectorSize
        | DiscRule::GdiOffset
        | DiscRule::HighDensityStart
        | DiscRule::SeveralSheets => SOURCE_UNSUPPORTED_CONTAINER,
        DiscRule::NotGauntletLegends => SOURCE_OTHER_GAME,
        DiscRule::SheetTooLarge => SOURCE_UNRECOGNIZED,
        DiscRule::SectorHeader
        | DiscRule::SectorBcd
        | DiscRule::SectorTime
        | DiscRule::SheetText
        | DiscRule::TrackName
        | DiscRule::MissingIndex
        | DiscRule::NumberTooLarge
        | DiscRule::CueTime
        | DiscRule::CueLine
        | DiscRule::IncompleteSheet
        | DiscRule::DuplicateTrack
        | DiscRule::GdiCount
        | DiscRule::GdiLine
        | DiscRule::GdiTrackNumber
        | DiscRule::GdiTrackType
        | DiscRule::GdiFirstSector
        | DiscRule::OverlappingTracks
        | DiscRule::ExtentOutside
        | DiscRule::SectorAddress
        | DiscRule::SectorEdc
        | DiscRule::TextField
        | DiscRule::VolumeDescriptor
        | DiscRule::RootNotDirectory
        | DiscRule::BothEndian
        | DiscRule::RecordTruncated
        | DiscRule::RecordUnsupported
        | DiscRule::FileIdentifier
        | DiscRule::DirectoryShape
        | DiscRule::Entries
        | DiscRule::EntryName
        | DiscRule::SizesHeader
        | DiscRule::SizesTable
        | DiscRule::SizesName
        | DiscRule::SizesNoName
        | DiscRule::SizesNameUnsafe
        | DiscRule::SizesDuplicate
        | DiscRule::SizesExtent
        | DiscRule::SizesConflict => SOURCE_DAMAGED,
    }
}

/// The code a failed write reports, by what the system answered: `space.ranOut` when the disk was
/// full or the quota spent, else `io.failed`.
fn written(error: &io::Error) -> &'static str {
    // The system's kinds of failure are an open set, so this match alone ends in a wildcard: every
    // kind but the two that mean no room is left is a write that failed.
    match error.kind() {
        io::ErrorKind::StorageFull | io::ErrorKind::QuotaExceeded => SPACE_RAN_OUT,
        _ => IO_FAILED,
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::io;
    use std::path::PathBuf;

    use gdl_extract::{ExtractError, FingerprintRule, NativeRule, TreeRule};
    use gdl_files::{DISC_NAMES, FilesError, RelPath};
    use gdl_install::{
        BuildInfo, FolderError, FolderState, GameStartError, GameUseError, LockError, PayloadError,
        ReceiptError, SectionsError,
    };
    use gdl_media::{DiscRule, DumpRefusal, MediaError, RomRule};
    use gdl_release::Platform;

    use super::{
        CODES, Report, dump_refusal, extract, files, folder, game_start, game_use, lock, media,
        payload, receipt, refusal, sections, state,
    };
    use crate::privilege::Refusal;

    /// What a broken rule of a runtime, or of the launcher file that carries it, reports.
    const DAMAGED: Report = Report::Error("payload.damaged");

    /// What a failure to read or copy a runtime, or to read the launcher file, reports.
    const FAILED: Report = Report::Error("io.failed");

    /// Every rule of the N64 ROM's format, with its code: only a ROM cut short and a file of no
    /// byte order are told apart from damage.
    const ROM_RULES: [(RomRule, &str); 27] = [
        (RomRule::Header, "source.incomplete"),
        (RomRule::ByteOrder, "source.unrecognized"),
        (RomRule::RomTooLarge, "source.damaged"),
        (RomRule::RomdiskHeader, "source.damaged"),
        (RomRule::OutsideData, "source.damaged"),
        (RomRule::DirectoryCount, "source.damaged"),
        (RomRule::DirectoryOverlap, "source.damaged"),
        (RomRule::RecordPlace, "source.damaged"),
        (RomRule::UnterminatedPath, "source.damaged"),
        (RomRule::NonAsciiPath, "source.damaged"),
        (RomRule::LeadingSlash, "source.damaged"),
        (RomRule::UnsafePath, "source.damaged"),
        (RomRule::DuplicatePath, "source.damaged"),
        (RomRule::Collision, "source.damaged"),
        (RomRule::Flags, "source.damaged"),
        (RomRule::FileSize, "source.damaged"),
        (RomRule::FileOverlap, "source.damaged"),
        (RomRule::ExpandedSize, "source.damaged"),
        (RomRule::Deflate, "source.damaged"),
        (RomRule::DeflateEnd, "source.damaged"),
        (RomRule::DeflateTrailer, "source.damaged"),
        (RomRule::OverlayPadding, "source.damaged"),
        (RomRule::OverlayMetadata, "source.damaged"),
        (RomRule::OverlayName, "source.damaged"),
        (RomRule::WadHeader, "source.damaged"),
        (RomRule::WadDirectory, "source.damaged"),
        (RomRule::WadSection, "source.damaged"),
    ];

    /// Every rule of the Dreamcast disc's format, its sheet's and its sectors', with its code.
    const DISC_RULES: [(DiscRule, &str); 50] = [
        (DiscRule::SectorHeader, "source.damaged"),
        (DiscRule::SectorBcd, "source.damaged"),
        (DiscRule::SectorTime, "source.damaged"),
        (DiscRule::SheetText, "source.damaged"),
        (DiscRule::SheetTooLarge, "source.unrecognized"),
        (DiscRule::TrackName, "source.damaged"),
        (DiscRule::MissingIndex, "source.damaged"),
        (DiscRule::TrackPerFile, "source.unsupportedContainer"),
        (DiscRule::NumberTooLarge, "source.damaged"),
        (DiscRule::TrackMode, "source.unsupportedContainer"),
        (DiscRule::CueTime, "source.damaged"),
        (DiscRule::CueLine, "source.damaged"),
        (DiscRule::IncompleteSheet, "source.damaged"),
        (DiscRule::DuplicateTrack, "source.damaged"),
        (DiscRule::GdiCount, "source.damaged"),
        (DiscRule::GdiLine, "source.damaged"),
        (DiscRule::GdiTrackNumber, "source.damaged"),
        (DiscRule::GdiTrackType, "source.damaged"),
        (DiscRule::GdiSectorSize, "source.unsupportedContainer"),
        (DiscRule::GdiOffset, "source.unsupportedContainer"),
        (DiscRule::MissingTrack, "source.incomplete"),
        (DiscRule::TruncatedTrack, "source.incomplete"),
        (DiscRule::GdiFirstSector, "source.damaged"),
        (DiscRule::OverlappingTracks, "source.damaged"),
        (DiscRule::HighDensityStart, "source.unsupportedContainer"),
        (DiscRule::ExtentOutside, "source.damaged"),
        (DiscRule::ShortRead, "source.incomplete"),
        (DiscRule::SectorAddress, "source.damaged"),
        (DiscRule::SectorEdc, "source.damaged"),
        (DiscRule::NotGauntletLegends, "source.otherGame"),
        (DiscRule::TextField, "source.damaged"),
        (DiscRule::VolumeDescriptor, "source.damaged"),
        (DiscRule::RootNotDirectory, "source.damaged"),
        (DiscRule::BothEndian, "source.damaged"),
        (DiscRule::RecordTruncated, "source.damaged"),
        (DiscRule::RecordUnsupported, "source.damaged"),
        (DiscRule::FileIdentifier, "source.damaged"),
        (DiscRule::DirectoryShape, "source.damaged"),
        (DiscRule::Entries, "source.damaged"),
        (DiscRule::EntryName, "source.damaged"),
        (DiscRule::SizesHeader, "source.damaged"),
        (DiscRule::SizesTable, "source.damaged"),
        (DiscRule::SizesName, "source.damaged"),
        (DiscRule::SizesNoName, "source.damaged"),
        (DiscRule::SizesNameUnsafe, "source.damaged"),
        (DiscRule::SizesDuplicate, "source.damaged"),
        (DiscRule::SizesExtent, "source.damaged"),
        (DiscRule::SizesConflict, "source.damaged"),
        (DiscRule::NoSheet, "source.incomplete"),
        (DiscRule::SeveralSheets, "source.unsupportedContainer"),
    ];

    /// Every reason a dump is refused for its container or its identity, with its code.
    const DUMP_REFUSALS: [(DumpRefusal, &str); 7] = [
        (DumpRefusal::OtherGame, "source.otherGame"),
        (DumpRefusal::OtherRevision, "source.otherRevision"),
        (DumpRefusal::HashMismatch, "dump.hashMismatch"),
        (
            DumpRefusal::UnsupportedContainer,
            "source.unsupportedContainer",
        ),
        (DumpRefusal::Cdi, "dump.dc.cdi"),
        (DumpRefusal::Archive, "source.archive"),
        (DumpRefusal::Unrecognized, "source.unrecognized"),
    ];

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

    /// What the system answers a write that found the disk full or the quota spent, or that it
    /// refused, with the code each reports.
    fn write_failures() -> [(io::Error, &'static str); 3] {
        [
            (io::ErrorKind::StorageFull.into(), "space.ranOut"),
            (io::ErrorKind::QuotaExceeded.into(), "space.ranOut"),
            (denied(), "io.failed"),
        ]
    }

    /// A disc's file, by its path in the disc's tree, as a refused copy of it names it.
    fn disc_file() -> RelPath {
        RelPath::parse("DISK.ROM", &DISC_NAMES).expect("a disc's file name")
    }

    /// What the JSON reader finds in a file cut short.
    fn json_refusal() -> serde_json::Error {
        serde_json::from_str::<serde_json::Value>("{").expect_err("an object cut short")
    }

    /// One failure of every kind a reader of a dump meets, with what each reports: a copy whose
    /// writer would not take it once for each of the system's answers, and a refusal by every
    /// rule of both formats.
    fn media_refusals() -> Vec<(MediaError, Report)> {
        let mut refusals = vec![
            (
                MediaError::Io {
                    path: path(),
                    source: denied(),
                },
                FAILED,
            ),
            // A dump that cannot be read is `io.failed` whatever the system answered: only a
            // write runs out of room.
            (
                MediaError::Io {
                    path: path(),
                    source: io::ErrorKind::StorageFull.into(),
                },
                FAILED,
            ),
            (MediaError::Cancelled, Report::Cancelled),
        ];
        refusals.extend(write_failures().into_iter().map(|(source, code)| {
            let file = disc_file();
            (MediaError::Write { file, source }, Report::Error(code))
        }));
        refusals.extend(ROM_RULES.into_iter().map(|(rule, code)| {
            let detail = detail();
            (MediaError::Rom { rule, detail }, Report::Error(code))
        }));
        refusals.extend(DISC_RULES.into_iter().map(|(rule, code)| {
            let detail = detail();
            (MediaError::Disc { rule, detail }, Report::Error(code))
        }));
        refusals
    }

    /// One refusal of every kind the launcher's file operations meet, with its code: a write once
    /// for each of the system's answers.
    fn files_refusals() -> Vec<(FilesError, &'static str)> {
        let mut refusals: Vec<(FilesError, &'static str)> = write_failures()
            .into_iter()
            .map(|(source, code)| {
                let path = path();
                (FilesError::Io { path, source }, code)
            })
            .collect();
        refusals.extend([
            (FilesError::Link { path: path() }, "io.failed"),
            (
                FilesError::TooLarge {
                    path: path(),
                    limit: 7,
                },
                "io.failed",
            ),
            (
                FilesError::Json {
                    path: path(),
                    source: json_refusal(),
                },
                "io.failed",
            ),
            (FilesError::Exists { path: path() }, "folder.collision"),
            (FilesError::NotRoot { name: name() }, "io.failed"),
        ]);
        refusals
    }

    /// One failure of every kind an extraction meets, with what each reports: a reader's, the
    /// staging's and a refused dump's once each, a staged file's write once for each of the
    /// system's answers, and the extraction's own by every rule.
    fn extract_refusals() -> Vec<(ExtractError, Report)> {
        let damaged = Report::Error("source.damaged");
        let mismatch = Report::Error("dump.hashMismatch");
        let mut refusals = vec![
            (
                ExtractError::Media(MediaError::Disc {
                    rule: DiscRule::SectorEdc,
                    detail: detail(),
                }),
                damaged,
            ),
            (
                ExtractError::Files(FilesError::Exists { path: path() }),
                Report::Error("folder.collision"),
            ),
            (
                ExtractError::Refused {
                    refusal: DumpRefusal::OtherRevision,
                },
                Report::Error("source.otherRevision"),
            ),
            (
                ExtractError::Changed {
                    path: "disc/SIZES.ROM".into(),
                },
                mismatch,
            ),
            (ExtractError::Cancelled, Report::Cancelled),
        ];
        refusals.extend(write_failures().into_iter().map(|(source, code)| {
            let path = path();
            (ExtractError::Io { path, source }, Report::Error(code))
        }));
        let trees = [
            TreeRule::Duplicate,
            TreeRule::FolderCase,
            TreeRule::Conflict,
        ];
        refusals.extend(trees.map(|rule| {
            let detail = detail();
            (ExtractError::Tree { rule, detail }, damaged)
        }));
        let natives = [
            NativeRule::TableMissing,
            NativeRule::TooLarge,
            NativeRule::DiskTruncated,
        ];
        refusals.extend(natives.map(|rule| {
            let detail = detail();
            (ExtractError::Native { rule, detail }, damaged)
        }));
        let fingerprints = [
            FingerprintRule::Missing,
            FingerprintRule::Unexpected,
            FingerprintRule::Digest,
        ];
        refusals.extend(fingerprints.map(|rule| {
            let path = "disc/DISK.ROM".into();
            (ExtractError::Fingerprint { path, rule }, mismatch)
        }));
        refusals
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
        for (error, expected) in media_refusals() {
            assert_eq!(media(&error), expected, "{}: {error}", error.rule());
        }
        for (refused, expected) in DUMP_REFUSALS {
            assert_eq!(dump_refusal(refused), expected, "{}", refused.as_str());
        }
        for (error, expected) in files_refusals() {
            assert_eq!(files(&error), expected, "{}: {error}", error.rule());
        }
        for (error, expected) in extract_refusals() {
            assert_eq!(extract(&error), expected, "{}: {error}", error.rule());
        }
    }

    #[test]
    fn an_extraction_refused_by_a_reader_or_the_staging_reports_what_they_do() {
        // A reader's failure, the staging's refusal and a dump the trust does not name are the
        // extraction's, so each reports as it does where it is met.
        for (error, _) in media_refusals() {
            let expected = media(&error);
            let error = ExtractError::Media(error);
            assert_eq!(extract(&error), expected, "{}: {error}", error.rule());
        }
        for (error, _) in files_refusals() {
            let expected = Report::Error(files(&error));
            let error = ExtractError::Files(error);
            assert_eq!(extract(&error), expected, "{}: {error}", error.rule());
        }
        for (refused, _) in DUMP_REFUSALS {
            let expected = Report::Error(dump_refusal(refused));
            let error = ExtractError::Refused { refusal: refused };
            assert_eq!(extract(&error), expected, "{}: {error}", error.rule());
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
        returned.extend(
            media_refusals()
                .iter()
                .filter_map(|(error, _)| code_of(media(error))),
        );
        returned.extend(
            DUMP_REFUSALS
                .into_iter()
                .map(|(refused, _)| dump_refusal(refused)),
        );
        returned.extend(files_refusals().iter().map(|(error, _)| files(error)));
        returned.extend(
            extract_refusals()
                .iter()
                .filter_map(|(error, _)| code_of(extract(error))),
        );
        assert_eq!(returned, listed);
    }
}
