//! What the readers report when a dump cannot be read or is refused: the path a read failed on,
//! with what the system answered or why a reader refused the path before reading it; the disc
//! file whose copy its writer would not take; that the job reading the dump was asked to stop; or
//! the rule of its format the dump breaks; as technical detail for logs. What a player reads is
//! chosen from the failure elsewhere.

use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{Cancelled, RelPath};

/// What a reader answers for a dump path that names no file, such as a root.
const NAMES_NO_FILE: &str = "the path names no file";

/// What a reader answers for a dump path where what is there is no regular file, such as a folder
/// or a FIFO.
const NAMES_NO_REGULAR_FILE: &str = "the path names no regular file";

/// The most characters of a name or a line taken from a dump that a refusal quotes, whichever
/// reader refuses it: more than the longest path the extracted tree may hold, so that a name
/// refused for its length is shown whole well past that bound, while text that runs on is shown
/// by its length and its start alone.
pub(crate) const MAX_QUOTED_CHARS: usize = 256;

/// Why a dump could not be read, was refused, or could not be copied where a caller asked.
#[derive(Debug, thiserror::Error)]
pub enum MediaError {
    /// Reading `path` failed: the system refused it or could not give its bytes, or the reader
    /// refused the path before opening anything there, since it names no file, as a root does, or
    /// what is there is no regular file, such as a folder, a FIFO or a device.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The file or folder that was being read.
        path: PathBuf,
        /// What the system answered, or the reader's own refusal of the path.
        #[source]
        source: io::Error,
    },
    /// The writer a disc file's copy was given refused its bytes, as one on a full disk does.
    /// Nothing of the dump failed to read, and what the writer wrote to is the caller's to name.
    #[error("writing the copy of the disc's {} failed: {source}", .file.as_str())]
    Write {
        /// The disc file whose copy was being written, by its path in the disc's tree.
        file: RelPath,
        /// What the writer answered.
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
    /// The GameCube disc image, its container or the header of the disc it holds, breaks a rule
    /// of its format, or is not the disc the launcher installs from.
    #[error("{detail}")]
    GameCube {
        /// The rule it breaks.
        rule: GameCubeRule,
        /// What is wrong, in English, in the installer's own words where it has fixed words for
        /// it.
        detail: Box<str>,
    },
    /// The disc's icon, a TPL texture, breaks a rule of its format, or keeps its first image in
    /// an encoding the launcher does not decode.
    #[error("{detail}")]
    Tpl {
        /// The rule it breaks.
        rule: TplRule,
        /// What is wrong, in English, in the installer's own words.
        detail: Box<str>,
    },
}

impl MediaError {
    /// The failure's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        // Every variant is named, with no wildcard, so that a failure added later is given its
        // rule on purpose.
        match self {
            MediaError::Io { .. } | MediaError::Write { .. } => "io",
            MediaError::Cancelled => "cancelled",
            MediaError::Rom { rule, .. } => rule.as_str(),
            MediaError::Disc { rule, .. } => rule.as_str(),
            MediaError::GameCube { rule, .. } => rule.as_str(),
            MediaError::Tpl { rule, .. } => rule.as_str(),
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

    /// The refusal of a dump path where what is there is no regular file, such as a folder or a
    /// FIFO, which a reader gives before it opens anything.
    pub(crate) fn names_no_regular_file(path: &Path) -> MediaError {
        let unopened = io::Error::new(io::ErrorKind::InvalidInput, NAMES_NO_REGULAR_FILE);
        MediaError::io(path, unopened)
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

    /// A GameCube disc image refused by `rule`, for the reason `detail` gives.
    pub(crate) fn gamecube(rule: GameCubeRule, detail: impl Into<Box<str>>) -> MediaError {
        MediaError::GameCube {
            rule,
            detail: detail.into(),
        }
    }

    /// A disc's icon refused by `rule`, for the reason `detail` gives.
    pub(crate) fn tpl(rule: TplRule, detail: impl Into<Box<str>>) -> MediaError {
        MediaError::Tpl {
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

/// `words`, then `text`, taken from the dump: quoted whole, or past [`MAX_QUOTED_CHARS`]
/// characters by its length and its quoted start, so that a refusal never runs on with what it
/// quotes.
pub(crate) fn quoted(words: &str, text: &str) -> String {
    match long_start(text) {
        Some((len, start)) => format!("{words} of {len} characters beginning {start:?}"),
        None => format!("{words}: {text:?}"),
    }
}

/// The length of `text` in characters and its first [`MAX_QUOTED_CHARS`] characters, when it has
/// more than that.
pub(crate) fn long_start(text: &str) -> Option<(usize, &str)> {
    let (end, _) = text.char_indices().nth(MAX_QUOTED_CHARS)?;
    Some((text.chars().count(), &text[..end]))
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
    /// SIZES.ROM, the table of the files DISK.ROM packs, is shorter than the count of rows it
    /// begins with.
    SizesHeader,
    /// SIZES.ROM counts no row, or more rows than it holds after its count.
    SizesTable,
    /// A component of a path SIZES.ROM gives starts at or past the end of the pool of names after
    /// its rows, has no NUL to end it within the pool, or holds a byte outside ASCII.
    SizesName,
    /// A row of SIZES.ROM gives its path no component: its first place in the pool is negative.
    SizesNoName,
    /// A component of a path SIZES.ROM gives is no name the extracted tree may hold by itself, or
    /// the components together make a path longer than the tree may hold.
    SizesNameUnsafe,
    /// Two rows of SIZES.ROM give one path, in this or another letter case.
    SizesDuplicate,
    /// A row of SIZES.ROM places its file's bytes past DISK.ROM's end, or at a negative place
    /// other than the one that marks a file kept outside DISK.ROM.
    SizesExtent,
    /// A path SIZES.ROM gives is also the folder above another it gives.
    SizesConflict,
    /// No sheet lies in the folder given as the dump, or none beside the file given names it as
    /// a track's file, so there is no disc to read.
    NoSheet,
    /// More than one sheet lies in the folder given as the dump, or more than one beside the file
    /// given names it as a track's file, so which disc to read cannot be told.
    SeveralSheets,
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
            DiscRule::SizesHeader => "sizesHeader",
            DiscRule::SizesTable => "sizesTable",
            DiscRule::SizesName => "sizesName",
            DiscRule::SizesNoName => "sizesNoName",
            DiscRule::SizesNameUnsafe => "sizesNameUnsafe",
            DiscRule::SizesDuplicate => "sizesDuplicate",
            DiscRule::SizesExtent => "sizesExtent",
            DiscRule::SizesConflict => "sizesConflict",
            DiscRule::NoSheet => "noSheet",
            DiscRule::SeveralSheets => "severalSheets",
        }
    }
}

/// The rules of a GameCube disc image's format, of the CISO container it may come in, of the
/// header of the disc it holds and of the disc's file table, that an image is refused by.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum GameCubeRule {
    /// The image begins as a CISO but is shorter than a CISO's header.
    CisoHeader,
    /// The CISO's block size is outside what a GameCube CISO keeps, as a PSP's CSO, which begins
    /// with the same mark, gives.
    CisoBlockSize,
    /// The CISO's map flags a block with other than 0, left out, and 1, kept.
    CisoMap,
    /// The CISO ends before the blocks its map keeps, or before one of them as it is read.
    CisoData,
    /// A read reaches past the disc the image holds: its header, as in an image shorter than
    /// that.
    OutsideImage,
    /// The ISO ends before a read it is asked for, as one cut short while it is read does.
    TruncatedIso,
    /// The disc's header carries no GameCube disc's mark.
    NotGameCube,
    /// The disc's game ID is another game's.
    OtherGame,
    /// The disc's game ID is the game's, of another region than North America.
    OtherRegion,
    /// The disc is another of the game's discs than its one.
    DiscNumber,
    /// The disc's revision is later than any the US disc was pressed as.
    Revision,
    /// The image is an NKit image, a disc rebuilt without its padding.
    Nkit,
    /// The header places the file table inside the header, or gives it a length shorter than its
    /// root's row or longer than [`MAX_FST_LEN`](crate::MAX_FST_LEN).
    FstExtent,
    /// The file table's first row is not a root folder's, or counts no row or more rows than the
    /// table holds.
    FstRoot,
    /// A row of the file table is of a kind other than a file's and a folder's, or names its entry
    /// from a place outside the table or with no NUL after it.
    FstEntry,
    /// An entry's name holds a byte outside ASCII or is no name the installed tree may hold by
    /// itself.
    UnsafeName,
    /// Two entries have one path, in this or another ASCII letter case.
    DuplicatePath,
    /// An entry's path is longer than [`MAX_DISC_PATH_CHARS`](gdl_files::MAX_DISC_PATH_CHARS)
    /// characters.
    PathTooLong,
    /// A folder's row names another folder as its parent than the one it lies in, or ends before
    /// itself or past the end of that folder.
    Nesting,
    /// A file's bytes run past the end of the disc the image holds.
    TruncatedFile,
    /// The `Gauntlet` or `carddemo` at the top of the disc is a file, not a folder.
    RootNotFolder,
    /// The `Gauntlet` and `carddemo` folders hold more than
    /// [`MAX_DISC_ENTRIES`](crate::MAX_DISC_ENTRIES) entries together.
    TooManyEntries,
    /// The files of the `Gauntlet` and `carddemo` folders hold more bytes together than a whole
    /// disc does.
    TotalSize,
    /// The top of the disc has no `Gauntlet` folder or no `carddemo` folder.
    MissingRoots,
    /// One of the files the game cannot start without is not on the disc: the war's and the
    /// tower's WADs and the disc's icon.
    MissingData,
}

impl GameCubeRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            GameCubeRule::CisoHeader => "cisoHeader",
            GameCubeRule::CisoBlockSize => "cisoBlockSize",
            GameCubeRule::CisoMap => "cisoMap",
            GameCubeRule::CisoData => "cisoData",
            GameCubeRule::OutsideImage => "outsideImage",
            GameCubeRule::TruncatedIso => "truncatedIso",
            GameCubeRule::NotGameCube => "notGameCube",
            GameCubeRule::OtherGame => "otherGame",
            GameCubeRule::OtherRegion => "otherRegion",
            GameCubeRule::DiscNumber => "discNumber",
            GameCubeRule::Revision => "revision",
            GameCubeRule::Nkit => "nkit",
            GameCubeRule::FstExtent => "fstExtent",
            GameCubeRule::FstRoot => "fstRoot",
            GameCubeRule::FstEntry => "fstEntry",
            GameCubeRule::UnsafeName => "unsafeName",
            GameCubeRule::DuplicatePath => "duplicatePath",
            GameCubeRule::PathTooLong => "pathTooLong",
            GameCubeRule::Nesting => "nesting",
            GameCubeRule::TruncatedFile => "truncatedFile",
            GameCubeRule::RootNotFolder => "rootNotFolder",
            GameCubeRule::TooManyEntries => "tooManyEntries",
            GameCubeRule::TotalSize => "totalSize",
            GameCubeRule::MissingRoots => "missingRoots",
            GameCubeRule::MissingData => "missingData",
        }
    }
}

/// The rules of a TPL texture's format, as the installer holds a disc's icon to them, that an
/// icon is refused by.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TplRule {
    /// The TPL is shorter than its directory or longer than
    /// [`MAX_TPL_LEN`](crate::MAX_TPL_LEN).
    Size,
    /// The directory does not begin with a TPL's magic, counts no image or more than a TPL may
    /// list, or places the table of images inside itself or past the TPL's end.
    Directory,
    /// The first image's header runs past the TPL's end.
    Header,
    /// The first image has a side of no pixels or of more than 256.
    Dimensions,
    /// The first image keeps its pixels in another encoding than RGB565, RGB5A3 and RGBA8, or
    /// with a palette.
    Format,
    /// The first image's pixels run past the TPL's end.
    Pixels,
}

impl TplRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            TplRule::Size => "size",
            TplRule::Directory => "directory",
            TplRule::Header => "header",
            TplRule::Dimensions => "dimensions",
            TplRule::Format => "format",
            TplRule::Pixels => "pixels",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::io;
    use std::path::Path;

    use gdl_files::{Cancel, DISC_NAMES, MAX_DISC_PATH_CHARS, RelPath};

    use super::{
        DiscRule, GameCubeRule, MAX_QUOTED_CHARS, MediaError, RomRule, TplRule, long_start, quoted,
    };

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The most characters a refusal quotes of what it takes from a dump.
    const QUOTED_CHARS: usize = 256;

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

        // A copy's writer that refuses its bytes names the disc file by its place in the disc's
        // tree, never as a path that was read, and keeps what the writer answered.
        let file = RelPath::parse("AUDIO/X.STR", &DISC_NAMES).unwrap();
        let full = io::Error::new(io::ErrorKind::StorageFull, "the disk is full");
        let error = MediaError::Write { file, source: full };
        let said = "writing the copy of the disc's AUDIO/X.STR failed: the disk is full";
        assert_eq!(error.to_string(), said);
        assert_eq!(error.source().unwrap().to_string(), "the disk is full");
        assert_eq!(error.rule(), "io");

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
        // So is a path where what is there is no regular file, before it is opened.
        let error = MediaError::names_no_regular_file(&path);
        assert_eq!(
            error.to_string(),
            format!("{}: the path names no regular file", path.display())
        );
        let MediaError::Io { path: at, source } = &error else {
            panic!("{error:?}");
        };
        assert_eq!(*at, path);
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

        // So is a GameCube image's, by its own rules.
        let detail = "Invalid CISO block map";
        let refused = MediaError::gamecube(GameCubeRule::CisoMap, detail);
        assert_eq!(refused.to_string(), detail);
        assert!(refused.source().is_none());
        assert!(matches!(
            &refused,
            MediaError::GameCube { rule: GameCubeRule::CisoMap, detail: said } if &**said == detail
        ));
        let worded = MediaError::gamecube(GameCubeRule::Revision, format!("revision: {}", 2));
        assert_eq!(worded.to_string(), "revision: 2");

        // And a disc icon's, by the rules of its TPL.
        let detail = "Truncated disc icon TPL pixels";
        let refused = MediaError::tpl(TplRule::Pixels, detail);
        assert_eq!(refused.to_string(), detail);
        assert!(refused.source().is_none());
        assert!(matches!(
            &refused,
            MediaError::Tpl { rule: TplRule::Pixels, detail: said } if &**said == detail
        ));

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
            (DiscRule::SizesHeader, "sizesHeader"),
            (DiscRule::SizesTable, "sizesTable"),
            (DiscRule::SizesName, "sizesName"),
            (DiscRule::SizesNoName, "sizesNoName"),
            (DiscRule::SizesNameUnsafe, "sizesNameUnsafe"),
            (DiscRule::SizesDuplicate, "sizesDuplicate"),
            (DiscRule::SizesExtent, "sizesExtent"),
            (DiscRule::SizesConflict, "sizesConflict"),
            (DiscRule::NoSheet, "noSheet"),
            (DiscRule::SeveralSheets, "severalSheets"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(MediaError::disc(rule, "").rule(), name);
        }
        // Every rule of a GameCube image's format keeps the name the shared vectors give it.
        let rules = [
            (GameCubeRule::CisoHeader, "cisoHeader"),
            (GameCubeRule::CisoBlockSize, "cisoBlockSize"),
            (GameCubeRule::CisoMap, "cisoMap"),
            (GameCubeRule::CisoData, "cisoData"),
            (GameCubeRule::OutsideImage, "outsideImage"),
            (GameCubeRule::TruncatedIso, "truncatedIso"),
            (GameCubeRule::NotGameCube, "notGameCube"),
            (GameCubeRule::OtherGame, "otherGame"),
            (GameCubeRule::OtherRegion, "otherRegion"),
            (GameCubeRule::DiscNumber, "discNumber"),
            (GameCubeRule::Revision, "revision"),
            (GameCubeRule::Nkit, "nkit"),
            (GameCubeRule::FstExtent, "fstExtent"),
            (GameCubeRule::FstRoot, "fstRoot"),
            (GameCubeRule::FstEntry, "fstEntry"),
            (GameCubeRule::UnsafeName, "unsafeName"),
            (GameCubeRule::DuplicatePath, "duplicatePath"),
            (GameCubeRule::PathTooLong, "pathTooLong"),
            (GameCubeRule::Nesting, "nesting"),
            (GameCubeRule::TruncatedFile, "truncatedFile"),
            (GameCubeRule::RootNotFolder, "rootNotFolder"),
            (GameCubeRule::TooManyEntries, "tooManyEntries"),
            (GameCubeRule::TotalSize, "totalSize"),
            (GameCubeRule::MissingRoots, "missingRoots"),
            (GameCubeRule::MissingData, "missingData"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(MediaError::gamecube(rule, "").rule(), name);
        }
        // Every rule of a disc icon's TPL keeps the name the shared vectors give it.
        let rules = [
            (TplRule::Size, "size"),
            (TplRule::Directory, "directory"),
            (TplRule::Header, "header"),
            (TplRule::Dimensions, "dimensions"),
            (TplRule::Format, "format"),
            (TplRule::Pixels, "pixels"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(MediaError::tpl(rule, "").rule(), name);
        }
    }

    #[test]
    fn text_from_a_dump_is_quoted_up_to_one_bound() {
        // Every reader quotes up to one bound, past the longest path the tree may hold, so that a
        // path refused for its length is quoted whole.
        assert_eq!(MAX_QUOTED_CHARS, QUOTED_CHARS);
        const { assert!(MAX_DISC_PATH_CHARS < MAX_QUOTED_CHARS) };
        // Text as long as the bound is quoted whole after the words, escaped as Debug escapes it.
        let whole = "\u{e9}".repeat(QUOTED_CHARS);
        assert_eq!(long_start(&whole), None);
        assert_eq!(quoted("Words", &whole), format!("Words: {whole:?}"));
        assert_eq!(quoted("Words", "a\"b"), "Words: \"a\\\"b\"");
        assert_eq!(quoted("Words", ""), "Words: \"\"");
        // Longer text is told by its length and its start, counted in characters, not bytes.
        for len in [QUOTED_CHARS + 1, 60_000] {
            let text = "\u{e9}".repeat(len);
            let start = "\u{e9}".repeat(QUOTED_CHARS);
            assert_eq!(long_start(&text), Some((len, start.as_str())), "{len}");
            let said = format!("Words of {len} characters beginning {start:?}");
            assert_eq!(quoted("Words", &text), said, "{len}");
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
