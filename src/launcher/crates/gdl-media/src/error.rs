//! What the readers report when a dump cannot be read or is refused: the path a read failed on and
//! what the system answered, that the job reading the dump was asked to stop, or the rule of its
//! format the dump breaks, as technical detail for logs. What a player reads is chosen from the
//! failure elsewhere.

use std::io;
use std::path::{Path, PathBuf};

use gdl_files::Cancelled;

/// What a reader answers for a dump path that names no file, such as a root.
const NAMES_NO_FILE: &str = "the path names no file";

/// Why a dump could not be read or was refused.
#[derive(Debug, thiserror::Error)]
pub enum MediaError {
    /// Reading `path` failed: the system refused it or could not give its bytes.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The file or folder that was being read.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The job reading the dump was asked to stop before the read was done.
    #[error("cancelled")]
    Cancelled,
    /// The N64 ROM, or a native file it holds, breaks a rule of its format.
    #[error("{detail}")]
    Rom {
        /// The rule it breaks.
        rule: RomRule,
        /// What is wrong, in English, in the N64 extractor's own words where it has fixed words
        /// for it.
        detail: Box<str>,
    },
    /// The Dreamcast disc, as its sheet lists its track files and they keep its sectors, breaks a
    /// rule of its format.
    #[error("{detail}")]
    Disc {
        /// The rule it breaks.
        rule: DiscRule,
        /// What is wrong, in English, in the Dreamcast extractor's own words where it has fixed
        /// words for it.
        detail: Box<str>,
    },
}

impl MediaError {
    /// The failure's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        // Every variant is named, with no wildcard, so that a failure added later is given its
        // rule on purpose.
        match self {
            MediaError::Io { .. } => "io",
            MediaError::Cancelled => "cancelled",
            MediaError::Rom { rule, .. } => rule.as_str(),
            MediaError::Disc { rule, .. } => rule.as_str(),
        }
    }

    /// Reading `path` failed with `source`.
    pub(crate) fn io(path: &Path, source: io::Error) -> MediaError {
        MediaError::Io {
            path: path.to_path_buf(),
            source,
        }
    }

    /// The refusal of a dump path that names no file, such as a root or a path that ends in
    /// `..`, which a reader gives before it reads anything.
    pub(crate) fn names_no_file(path: &Path) -> MediaError {
        let unnamed = io::Error::new(io::ErrorKind::InvalidInput, NAMES_NO_FILE);
        MediaError::io(path, unnamed)
    }

    /// An N64 ROM refused by `rule`, for the reason `detail` gives.
    pub(crate) fn rom(rule: RomRule, detail: impl Into<Box<str>>) -> MediaError {
        MediaError::Rom {
            rule,
            detail: detail.into(),
        }
    }

    /// A Dreamcast disc refused by `rule`, for the reason `detail` gives.
    pub(crate) fn disc(rule: DiscRule, detail: impl Into<Box<str>>) -> MediaError {
        MediaError::Disc {
            rule,
            detail: detail.into(),
        }
    }
}

impl From<Cancelled> for MediaError {
    /// A reader stops where its job's cancel tells it to, between two blocks of its work.
    fn from(_: Cancelled) -> MediaError {
        MediaError::Cancelled
    }
}

/// The rules of the N64 ROM's format, and of the native files it holds, that a ROM is refused by.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RomRule {
    /// The ROM is shorter than its header, or does not end on a whole word.
    Header,
    /// The ROM's first word is that of none of the byte orders dumps keep a cartridge in.
    ByteOrder,
    /// The ROM is longer than the largest cartridge.
    RomTooLarge,
    /// The ROM holds no ROMDISK header, or more than one.
    RomdiskHeader,
    /// A part the ROM describes reaches past the data that holds it, as the ROMDISK's directory
    /// past the ROM's end, a file's bytes into the directory, or an executable overlay's record or
    /// extent past the ROM's end.
    OutsideData,
    /// The ROMDISK header's reserved word is not zero, or it lists no file or more than the most a
    /// ROMDISK may list.
    DirectoryCount,
    /// The ROMDISK's pointer table starts inside its header.
    DirectoryOverlap,
    /// A ROMDISK record starts before the one listed before it ends, or off a whole word.
    RecordPlace,
    /// A ROMDISK record's path has no end within the directory.
    UnterminatedPath,
    /// A ROMDISK record's path holds a byte outside ASCII.
    NonAsciiPath,
    /// A ROMDISK record's path does not begin with exactly one `/`.
    LeadingSlash,
    /// A ROMDISK record's path is no name the extracted tree may hold.
    UnsafePath,
    /// Two ROMDISK paths are the same, or differ only in letter case.
    DuplicatePath,
    /// A ROMDISK path is also the folder above another.
    Collision,
    /// A ROMDISK record's flags are neither none nor compressed.
    Flags,
    /// A ROMDISK file expands past the most a file may, or one kept as it is stores other than
    /// its size.
    FileSize,
    /// A ROMDISK file's bytes start inside its header.
    FileOverlap,
    /// The ROMDISK's files together expand past the most they may.
    ExpandedSize,
    /// A raw DEFLATE stream holds what the format does not allow, such as a block of no type or a
    /// distance back past the start of its output.
    Deflate,
    /// A raw DEFLATE stream reaches no end marker within a byte past the size its record gives, or
    /// decodes to other than exactly that size.
    DeflateEnd,
    /// A ROMDISK file's bytes go on past the end of its raw DEFLATE stream.
    DeflateTrailer,
    /// An executable overlay's extent holds more bytes past the end of its stream than pad it.
    OverlayPadding,
    /// An executable overlay's record keeps a reserved word that is not zero, or places or sizes
    /// the zeroed memory after the overlay so that it disagrees with the overlay's length.
    OverlayMetadata,
    /// An executable overlay's record names it with a byte outside ASCII, or with a name whose
    /// file the extracted tree may not hold.
    OverlayName,
    /// A native WAD is shorter than its header.
    WadHeader,
    /// A native WAD's header describes a directory that ends where the WAD does in neither byte
    /// order, or in both.
    WadDirectory,
    /// A row of a native WAD's directory tags its section with a byte other than printable ASCII
    /// or NUL, or starts it before the header's end or past the directory's start.
    WadSection,
}

impl RomRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            RomRule::Header => "header",
            RomRule::ByteOrder => "byteOrder",
            RomRule::RomTooLarge => "romTooLarge",
            RomRule::RomdiskHeader => "romdiskHeader",
            RomRule::OutsideData => "outsideData",
            RomRule::DirectoryCount => "directoryCount",
            RomRule::DirectoryOverlap => "directoryOverlap",
            RomRule::RecordPlace => "recordPlace",
            RomRule::UnterminatedPath => "unterminatedPath",
            RomRule::NonAsciiPath => "nonAsciiPath",
            RomRule::LeadingSlash => "leadingSlash",
            RomRule::UnsafePath => "unsafePath",
            RomRule::DuplicatePath => "duplicatePath",
            RomRule::Collision => "collision",
            RomRule::Flags => "flags",
            RomRule::FileSize => "fileSize",
            RomRule::FileOverlap => "fileOverlap",
            RomRule::ExpandedSize => "expandedSize",
            RomRule::Deflate => "deflate",
            RomRule::DeflateEnd => "deflateEnd",
            RomRule::DeflateTrailer => "deflateTrailer",
            RomRule::OverlayPadding => "overlayPadding",
            RomRule::OverlayMetadata => "overlayMetadata",
            RomRule::OverlayName => "overlayName",
            RomRule::WadHeader => "wadHeader",
            RomRule::WadDirectory => "wadDirectory",
            RomRule::WadSection => "wadSection",
        }
    }
}

/// The rules of the Dreamcast GD-ROM's format, of the sheet that lists its track files, and of the
/// raw sectors those files keep, that a disc is refused by.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DiscRule {
    /// A raw sector's first bytes are not the sync and a header of mode 1: too few for a header,
    /// another sync, or another mode.
    SectorHeader,
    /// A raw sector's address is not BCD: a digit of it is past nine, but for the minute's tens,
    /// which count past nine on a disc of more than 100 minutes.
    SectorBcd,
    /// A raw sector's address gives a second past 59 or a frame past 74.
    SectorTime,
    /// A sheet's bytes are not UTF-8 text, after the byte order mark they may begin with.
    SheetText,
    /// A sheet is longer than [`MAX_SHEET_LEN`](crate::MAX_SHEET_LEN), far more than any disc's
    /// list of tracks needs.
    SheetTooLarge,
    /// A sheet names a track's file by a name the folder beside it may not hold: one that leaves
    /// the folder, names a device, or breaks another rule of names.
    TrackName,
    /// A cue sheet names a file while the track it opened last has no INDEX 01 yet.
    MissingIndex,
    /// A cue sheet opens a track while another is open, before it names a file, or in a file a
    /// track was read from already, whatever the letter case it names the file in: each track
    /// must lie in a file of its own, so a dump that keeps the whole disc in one file is refused.
    TrackPerFile,
    /// A cue sheet's track number is past 32 bits, a number of a time, or the frames the time
    /// comes to, past 64, or a GDI's LBA past 32.
    NumberTooLarge,
    /// A cue sheet gives a track a mode other than audio and raw mode 1 data, as a dump of cooked
    /// or mode 2 sectors does.
    TrackMode,
    /// A cue sheet's time gives a second past 59 or a frame past 74.
    CueTime,
    /// A cue sheet holds a line its reader does not read: another command, another spelling of
    /// one, a number in digits other than ASCII's, or an INDEX where no track is open.
    CueLine,
    /// A cue sheet lists no track, or ends with one still open.
    IncompleteSheet,
    /// A cue sheet lists two tracks of one number.
    DuplicateTrack,
    /// A GDI's first line is no count of tracks from 1 to 99, or the GDI lists another number of
    /// tracks than that line gives.
    GdiCount,
    /// A GDI's track line holds other than six fields, leaves a quote open or closes one within a
    /// field, or gives a field that should be a number in other than ASCII digits.
    GdiLine,
    /// A GDI's tracks are not numbered from 1 up, one at a time, in its order.
    GdiTrackNumber,
    /// A GDI gives a track a type other than 0, audio, and 4, data.
    GdiTrackType,
    /// A GDI gives a track sectors of other than 2352 bytes, as a dump of cooked 2048-byte
    /// sectors does, whose sectors keep no header to place them by.
    GdiSectorSize,
    /// A GDI gives a track an offset other than 0, which the format's readers do not agree on
    /// the meaning of.
    GdiOffset,
    /// A track's file is not there, or is no regular file, such as a folder: a data track's
    /// always, and under an installation's rules an audio track's too.
    MissingTrack,
    /// A track's file ends partway through a raw sector, or a data track's file holds no sector
    /// where its sheet places the track's first.
    TruncatedTrack,
    /// A GDI gives a data track an LBA other than the one its first sector's header gives.
    GdiFirstSector,
    /// Two data tracks, each placed by its first sector's header, hold sectors of one LBA.
    OverlappingTracks,
    /// No data track holds LBA 45000, where a GD-ROM's high-density area and its boot area begin,
    /// or under an installation's rules, none starts there.
    HighDensityStart,
    /// An extent of the disc reaches a sector no data track holds, as one in the gap an audio
    /// track leaves between two data tracks does.
    ExtentOutside,
    /// A data track's file ends before the sectors a read takes from it, as a file cut short
    /// while it is read does.
    ShortRead,
    /// A raw sector's header gives another address than the place its track puts it at.
    SectorAddress,
    /// A raw sector's error detection code is not the one of the bytes it covers. Only an
    /// installation's rules hold the sectors read to it.
    SectorEdc,
    /// The boot area is not Gauntlet Legends' on the Dreamcast: it does not begin with the
    /// console's hardware ID, or its title does not begin with the game's name.
    NotGauntletLegends,
    /// A text field the extraction records holds a byte outside ASCII: the boot area's product,
    /// title or executable, or the volume's name its volume descriptor gives.
    TextField,
    /// The volume descriptor after the boot area is no primary one of ISO 9660, or gives its
    /// logical blocks another size than a sector's user data.
    VolumeDescriptor,
    /// The volume descriptor's record of the root directory is not a directory's.
    RootNotDirectory,
    /// A number ISO 9660 keeps both-endian, little-endian and then big-endian, has halves that
    /// differ.
    BothEndian,
    /// A directory record is shorter than its fixed fields and an identifier of a byte, or runs
    /// past its directory's end or into the next sector.
    RecordTruncated,
    /// A directory record has an extended attribute record, interleaves its file, or carries a
    /// flag other than hidden and directory, such as the one a file continued in another record
    /// carries.
    RecordUnsupported,
    /// A directory record places its extent on another volume than the disc's one, or gives its
    /// identifier a length that runs past the record's end.
    FileIdentifier,
    /// A directory lies deeper below the root than
    /// [`MAX_DIRECTORY_DEPTH`](crate::MAX_DIRECTORY_DEPTH), holds more than
    /// [`MAX_DIRECTORY_LEN`](crate::MAX_DIRECTORY_LEN) bytes, or is one the walk has read
    /// already, as a folder that holds itself or its parent is.
    DirectoryShape,
    /// Two entries of the volume's tree have one path, in this or another letter case, as two
    /// versions of one file do, or the tree holds more than [`MAX_ENTRIES`](crate::MAX_ENTRIES).
    Entries,
    /// An entry's identifier holds a byte outside ASCII or, its version dropped, is no name the
    /// extracted tree may hold by itself, or makes with its folder's path a path longer than the
    /// tree may hold.
    EntryName,
}

impl DiscRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            DiscRule::SectorHeader => "sectorHeader",
            DiscRule::SectorBcd => "sectorBcd",
            DiscRule::SectorTime => "sectorTime",
            DiscRule::SheetText => "sheetText",
            DiscRule::SheetTooLarge => "sheetTooLarge",
            DiscRule::TrackName => "trackName",
            DiscRule::MissingIndex => "missingIndex",
            DiscRule::TrackPerFile => "trackPerFile",
            DiscRule::NumberTooLarge => "numberTooLarge",
            DiscRule::TrackMode => "trackMode",
            DiscRule::CueTime => "cueTime",
            DiscRule::CueLine => "cueLine",
            DiscRule::IncompleteSheet => "incompleteSheet",
            DiscRule::DuplicateTrack => "duplicateTrack",
            DiscRule::GdiCount => "gdiCount",
            DiscRule::GdiLine => "gdiLine",
            DiscRule::GdiTrackNumber => "gdiTrackNumber",
            DiscRule::GdiTrackType => "gdiTrackType",
            DiscRule::GdiSectorSize => "gdiSectorSize",
            DiscRule::GdiOffset => "gdiOffset",
            DiscRule::MissingTrack => "missingTrack",
            DiscRule::TruncatedTrack => "truncatedTrack",
            DiscRule::GdiFirstSector => "gdiFirstSector",
            DiscRule::OverlappingTracks => "overlappingTracks",
            DiscRule::HighDensityStart => "highDensityStart",
            DiscRule::ExtentOutside => "extentOutside",
            DiscRule::ShortRead => "shortRead",
            DiscRule::SectorAddress => "sectorAddress",
            DiscRule::SectorEdc => "sectorEdc",
            DiscRule::NotGauntletLegends => "notGauntletLegends",
            DiscRule::TextField => "textField",
            DiscRule::VolumeDescriptor => "volumeDescriptor",
            DiscRule::RootNotDirectory => "rootNotDirectory",
            DiscRule::BothEndian => "bothEndian",
            DiscRule::RecordTruncated => "recordTruncated",
            DiscRule::RecordUnsupported => "recordUnsupported",
            DiscRule::FileIdentifier => "fileIdentifier",
            DiscRule::DirectoryShape => "directoryShape",
            DiscRule::Entries => "entries",
            DiscRule::EntryName => "entryName",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::io;
    use std::path::Path;

    use gdl_files::Cancel;

    use super::{DiscRule, MediaError, RomRule};

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// Reads nothing, but stops as a reader does before each block when its job is asked to.
    fn read_until_cancelled(cancel: &Cancel) -> Result<(), MediaError> {
        cancel.check()?;
        Ok(())
    }

    #[test]
    fn failures_name_their_path_and_say_what_failed() {
        let path = Path::new("dumps").join("dump.z64");
        let missing = io::Error::new(io::ErrorKind::NotFound, "missing");
        let error = MediaError::io(&path, missing);
        assert_eq!(error.to_string(), format!("{}: missing", path.display()));
        assert_eq!(error.source().unwrap().to_string(), "missing");
        assert!(matches!(&error, MediaError::Io { path: at, .. } if *at == path));

        assert_eq!(MediaError::Cancelled.to_string(), "cancelled");
        assert!(MediaError::Cancelled.source().is_none());

        // A path that names no file is refused as an argument no file could answer, before the
        // system is asked anything.
        let unnamed = Path::new("dumps").join("..");
        let error = MediaError::names_no_file(&unnamed);
        assert_eq!(
            error.to_string(),
            format!("{}: the path names no file", unnamed.display())
        );
        let MediaError::Io { path: at, source } = &error else {
            panic!("{error:?}");
        };
        assert_eq!(*at, unnamed);
        assert_eq!(source.kind(), io::ErrorKind::InvalidInput);
        assert_eq!(error.rule(), "io");

        // A refusal says what breaks the rule, and nothing the system answered caused it.
        let detail = "unrecognized Nintendo 64 ROM byte order";
        let refused = MediaError::rom(RomRule::ByteOrder, detail);
        assert_eq!(refused.to_string(), detail);
        assert!(refused.source().is_none());
        assert!(matches!(
            &refused,
            MediaError::Rom { rule: RomRule::ByteOrder, detail: said } if &**said == detail
        ));
        let worded = MediaError::rom(RomRule::RomTooLarge, format!("{} bytes", 1 << 26));
        assert_eq!(worded.to_string(), "67108864 bytes");

        // A disc's refusal is said the same way, by its own rules.
        let detail = "Invalid BCD sector address";
        let refused = MediaError::disc(DiscRule::SectorBcd, detail);
        assert_eq!(refused.to_string(), detail);
        assert!(refused.source().is_none());
        assert!(matches!(
            &refused,
            MediaError::Disc { rule: DiscRule::SectorBcd, detail: said } if &**said == detail
        ));
        let worded = MediaError::disc(DiscRule::SectorTime, format!("at LBA {}", -150));
        assert_eq!(worded.to_string(), "at LBA -150");

        assert!(size_of::<MediaError>() <= MOST_ERROR_BYTES);
    }

    #[test]
    fn every_failure_names_its_rule() {
        let path = Path::new("dumps").join("dump.z64");
        let failures = [
            // The rule is the variant's, whatever the system answered.
            (
                MediaError::io(&path, io::ErrorKind::PermissionDenied.into()),
                "io",
            ),
            (
                MediaError::io(&path, io::ErrorKind::UnexpectedEof.into()),
                "io",
            ),
            (MediaError::Cancelled, "cancelled"),
            // A refusal is named by the rule it breaks, whatever its words.
            (MediaError::rom(RomRule::Header, "short"), "header"),
            (MediaError::rom(RomRule::ByteOrder, "order"), "byteOrder"),
            (MediaError::rom(RomRule::RomTooLarge, "long"), "romTooLarge"),
            (
                MediaError::disc(DiscRule::SectorHeader, "header"),
                "sectorHeader",
            ),
        ];
        for (failure, rule) in failures {
            assert_eq!(failure.rule(), rule, "{failure:?}");
        }
        // Every rule of the ROM's format keeps the name the shared vectors give it.
        let rules = [
            (RomRule::Header, "header"),
            (RomRule::ByteOrder, "byteOrder"),
            (RomRule::RomTooLarge, "romTooLarge"),
            (RomRule::RomdiskHeader, "romdiskHeader"),
            (RomRule::OutsideData, "outsideData"),
            (RomRule::DirectoryCount, "directoryCount"),
            (RomRule::DirectoryOverlap, "directoryOverlap"),
            (RomRule::RecordPlace, "recordPlace"),
            (RomRule::UnterminatedPath, "unterminatedPath"),
            (RomRule::NonAsciiPath, "nonAsciiPath"),
            (RomRule::LeadingSlash, "leadingSlash"),
            (RomRule::UnsafePath, "unsafePath"),
            (RomRule::DuplicatePath, "duplicatePath"),
            (RomRule::Collision, "collision"),
            (RomRule::Flags, "flags"),
            (RomRule::FileSize, "fileSize"),
            (RomRule::FileOverlap, "fileOverlap"),
            (RomRule::ExpandedSize, "expandedSize"),
            (RomRule::Deflate, "deflate"),
            (RomRule::DeflateEnd, "deflateEnd"),
            (RomRule::DeflateTrailer, "deflateTrailer"),
            (RomRule::OverlayPadding, "overlayPadding"),
            (RomRule::OverlayMetadata, "overlayMetadata"),
            (RomRule::OverlayName, "overlayName"),
            (RomRule::WadHeader, "wadHeader"),
            (RomRule::WadDirectory, "wadDirectory"),
            (RomRule::WadSection, "wadSection"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(MediaError::rom(rule, "").rule(), name);
        }
        // Every rule of the disc's format keeps the name the shared vectors give it.
        let rules = [
            (DiscRule::SectorHeader, "sectorHeader"),
            (DiscRule::SectorBcd, "sectorBcd"),
            (DiscRule::SectorTime, "sectorTime"),
            (DiscRule::SheetText, "sheetText"),
            (DiscRule::SheetTooLarge, "sheetTooLarge"),
            (DiscRule::TrackName, "trackName"),
            (DiscRule::MissingIndex, "missingIndex"),
            (DiscRule::TrackPerFile, "trackPerFile"),
            (DiscRule::NumberTooLarge, "numberTooLarge"),
            (DiscRule::TrackMode, "trackMode"),
            (DiscRule::CueTime, "cueTime"),
            (DiscRule::CueLine, "cueLine"),
            (DiscRule::IncompleteSheet, "incompleteSheet"),
            (DiscRule::DuplicateTrack, "duplicateTrack"),
            (DiscRule::GdiCount, "gdiCount"),
            (DiscRule::GdiLine, "gdiLine"),
            (DiscRule::GdiTrackNumber, "gdiTrackNumber"),
            (DiscRule::GdiTrackType, "gdiTrackType"),
            (DiscRule::GdiSectorSize, "gdiSectorSize"),
            (DiscRule::GdiOffset, "gdiOffset"),
            (DiscRule::MissingTrack, "missingTrack"),
            (DiscRule::TruncatedTrack, "truncatedTrack"),
            (DiscRule::GdiFirstSector, "gdiFirstSector"),
            (DiscRule::OverlappingTracks, "overlappingTracks"),
            (DiscRule::HighDensityStart, "highDensityStart"),
            (DiscRule::ExtentOutside, "extentOutside"),
            (DiscRule::ShortRead, "shortRead"),
            (DiscRule::SectorAddress, "sectorAddress"),
            (DiscRule::SectorEdc, "sectorEdc"),
            (DiscRule::NotGauntletLegends, "notGauntletLegends"),
            (DiscRule::TextField, "textField"),
            (DiscRule::VolumeDescriptor, "volumeDescriptor"),
            (DiscRule::RootNotDirectory, "rootNotDirectory"),
            (DiscRule::BothEndian, "bothEndian"),
            (DiscRule::RecordTruncated, "recordTruncated"),
            (DiscRule::RecordUnsupported, "recordUnsupported"),
            (DiscRule::FileIdentifier, "fileIdentifier"),
            (DiscRule::DirectoryShape, "directoryShape"),
            (DiscRule::Entries, "entries"),
            (DiscRule::EntryName, "entryName"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(MediaError::disc(rule, "").rule(), name);
        }
    }

    #[test]
    fn a_cancelled_job_stops_the_read() {
        let cancel = Cancel::default();
        assert!(read_until_cancelled(&cancel).is_ok());

        cancel.cancel();
        let error = read_until_cancelled(&cancel).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        assert_eq!(error.rule(), "cancelled");
    }
}
