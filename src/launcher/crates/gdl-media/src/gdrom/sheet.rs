//! The sheet that lists a dump's track files: a small file of UTF-8 text beside the files it
//! names, a byte order mark before its text dropped, that gives each track's number, its file,
//! what its sectors hold and where its first sector lies. It is a cue sheet, read as the Dreamcast
//! extractor reads one, or a GDI, which begins with the count of its tracks. A dump given as a
//! folder or as one of its track files is read from the one sheet found by content there.

use std::ffi::OsStr;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use super::{cue, gdi, is_number, lines, strip};
use crate::dump_file::{open_regular, open_regular_within};
use crate::{DiscRule, MediaError};

/// The most bytes a sheet may hold: far more than any disc's list of tracks needs, so that a large
/// file given as a sheet is refused before it is read.
pub const MAX_SHEET_LEN: u64 = 64 << 10;

/// The byte order mark a sheet's text may begin with, which is no part of its first line.
const BYTE_ORDER_MARK: &str = "\u{feff}";

/// The folder a relative path's file lies in when the path names no folder before it.
const CURRENT_FOLDER: &str = ".";

/// What the reader answers for a folder given as a dump that holds no sheet.
const NO_SHEET_IN_FOLDER: &str = "No sheet in the folder lists a disc's tracks";

/// What it answers for a file given as a dump that is no sheet, and that no sheet beside it names.
const NO_SHEET_BESIDE: &str = "No sheet beside the file names it as a track's file";

/// What it answers, after their count, for a folder given as a dump that holds several sheets.
const SHEETS_IN_FOLDER: &str = "sheets in the folder list a disc's tracks";

/// What it answers, after their count, for a file given as a dump that several sheets beside it
/// name.
const SHEETS_BESIDE: &str = "sheets beside the file name it as a track's file";

/// Where sheets were looked for, which a refusal of none or of several names.
#[derive(Clone, Copy)]
enum Search {
    /// In the folder given as the dump.
    Folder,
    /// Beside the file given as the dump, among those that name it as a track's file.
    Beside,
}

/// A sheet found in a folder, and the files beside it that it names.
struct FoundSheet {
    /// Its path in the folder.
    path: PathBuf,
    /// Whether the readers take it.
    taken: bool,
    /// The files it names: those of the tracks the readers read from it or, for a sheet they
    /// refuse, those its text gives.
    files: Vec<String>,
}

/// What kind of sheet lists a dump's tracks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SheetKind {
    /// A cue sheet, which names each track's file before the track.
    Cue,
    /// A GDI, which gives the count of its tracks, then each track's number, the LBA of its first
    /// sector, its type, the size of its sectors, its file and an offset, a line each.
    Gdi,
}

/// What a track's sectors hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TrackMode {
    /// Audio, which holds none of the disc's files.
    Audio,
    /// Data, kept as raw MODE1/2352 sectors, each with its sync, header and codes.
    Mode1Raw,
}

/// A track as a sheet lists it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SheetTrack {
    /// Its number, which no other track of the sheet has.
    pub number: u32,
    /// The name of the file that holds it, beside the sheet, as the sheet spells it.
    pub file: String,
    /// What its sectors hold.
    pub mode: TrackMode,
    /// How many sectors of its file come before its first: in a cue sheet, the frames of the time
    /// its INDEX 01 gives, a sector each; a GDI's track starts at its file's first sector.
    pub index: u64,
    /// The LBA a GDI gives the track's first sector, which that sector's own header must agree
    /// with. A cue sheet gives none: its tracks are placed by their sectors' headers alone.
    pub lba: Option<u32>,
}

/// A sheet as it was read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Sheet {
    /// What kind of sheet it is.
    pub kind: SheetKind,
    /// The path it was read from, as it was given: the folder it names holds the track files.
    pub path: PathBuf,
    /// Its tracks, in its order.
    pub tracks: Vec<SheetTrack>,
}

/// Reads the sheet at `path` whole, and then as [`parse_sheet`] reads its bytes. What is no
/// regular file, such as a folder or a FIFO, is refused before it is opened. A file longer than
/// [`MAX_SHEET_LEN`] is refused by its length before a byte of it is read, and one that grows past
/// the bound as it is read is refused once it has, with no more than a byte past the bound held.
/// The file is only ever read.
pub fn read_sheet(path: &Path) -> Result<Sheet, MediaError> {
    let (file, len) = open_regular(path)
        .map_err(|error| MediaError::io(path, error))?
        .ok_or_else(|| MediaError::names_no_regular_file(path))?;
    if len > MAX_SHEET_LEN {
        return Err(too_large());
    }
    let bytes = read_within_bound(file, path, len)?;
    let (kind, tracks) = parse_sheet(&bytes)?;
    Ok(Sheet {
        kind,
        path: path.to_path_buf(),
        tracks,
    })
}

/// The kind of sheet `bytes` hold and the tracks it lists, in its order: one byte order mark at
/// the start is dropped, and the rest must be UTF-8 text. A text whose first line is the count of
/// tracks a GDI begins with is read by the GDI's grammar, and any other as the Dreamcast extractor
/// reads a cue sheet. Refused by the first rule the text breaks.
pub fn parse_sheet(bytes: &[u8]) -> Result<(SheetKind, Vec<SheetTrack>), MediaError> {
    let unmarked = without_mark(bytes);
    let marked = bytes.len() - unmarked.len();
    let text = str::from_utf8(unmarked).map_err(|error| {
        let at = marked + error.valid_up_to();
        let detail = format!("Sheet is not UTF-8 text from byte {at}");
        MediaError::disc(DiscRule::SheetText, detail)
    })?;
    if begins_with_count(text) {
        Ok((SheetKind::Gdi, gdi::read(text)?))
    } else {
        Ok((SheetKind::Cue, cue::read(text)?))
    }
}

/// Whether the first line of `text` that is not empty once stripped, as the Dreamcast extractor
/// ends and strips a cue sheet's lines, is a number in ASCII digits: the count of tracks a GDI
/// begins with, and a line no cue sheet the extractor reads holds.
fn begins_with_count(text: &str) -> bool {
    lines(text)
        .map(strip)
        .find(|line| !line.is_empty())
        .is_some_and(is_number)
}

/// `bytes` without the one byte order mark a sheet's text may begin with.
pub(crate) fn without_mark(bytes: &[u8]) -> &[u8] {
    bytes
        .strip_prefix(BYTE_ORDER_MARK.as_bytes())
        .unwrap_or(bytes)
}

/// What `bytes`, all a file holds, are as a sheet: the kind and tracks [`parse_sheet`] reads from
/// them, or the rule it refuses them by when only a sheet's text can break that rule, as a cue
/// sheet that keeps every track in one file and a GDI of cooked sectors do, so that such a dump
/// is told by its own refusal. `None` when they are no sheet: bytes that are no UTF-8 text, text
/// that holds a line the cue sheet's reader does not read, as most text does at its first, and
/// text that lists no whole track, as an empty file does.
pub(crate) fn sheet_of(bytes: &[u8]) -> Option<Result<(SheetKind, Vec<SheetTrack>), MediaError>> {
    match parse_sheet(bytes) {
        Err(MediaError::Disc {
            rule: DiscRule::SheetText | DiscRule::CueLine | DiscRule::IncompleteSheet,
            ..
        }) => None,
        read => Some(read),
    }
}

/// The sheet the dump at `path` is read from, found by content, never by name: the one sheet in
/// the folder `path` names; else the file at `path` itself, when it is a sheet; else the one sheet
/// beside it in its folder that names it as a track's file, by its name as the system opens the
/// file a sheet names: whatever the letter case on Windows, and exactly on Linux. A sheet is a
/// regular file of at most [`MAX_SHEET_LEN`] bytes whose text [`parse_sheet`] reads, or refuses
/// by a rule only a sheet's text breaks, as it refuses a cue sheet that keeps every track in one
/// file, so that such a dump is told by its own refusal however it is given. A sheet the readers
/// take names the files of the tracks they read from it, and one they refuse the files its text
/// gives; one they refuse is found only where no sheet they take is, in the folder or naming the
/// file, so that a stray file beside a dump's sheet never hides it. Refused when there is no such
/// sheet, or more than one, and when what is at `path` is neither a folder nor a regular file, or
/// when a file short enough to be a sheet cannot be read. Every file is only ever read, and only
/// once seen to be a regular file no longer than a sheet may be, so that no FIFO or device is
/// opened, nor a track's file, which another program may hold open.
pub fn find_sheet(path: &Path) -> Result<PathBuf, MediaError> {
    let metadata = fs::metadata(path).map_err(|error| MediaError::io(path, error))?;
    if metadata.is_dir() {
        let sheets = preferred(sheets_in(path)?);
        return one_of(sheets, Search::Folder)?.ok_or_else(|| no_sheet(Search::Folder));
    }
    if !metadata.is_file() {
        return Err(MediaError::names_no_regular_file(path));
    }
    if sheet_bytes(path)?.as_deref().and_then(sheet_of).is_some() {
        return Ok(path.to_path_buf());
    }
    sheet_naming(path)?.ok_or_else(|| no_sheet(Search::Beside))
}

/// The one sheet beside the file at `path`, in its folder, that names the file as a track's, as
/// [`find_sheet`] finds it, or `None` when no sheet there names it. Refused when more than one
/// does.
pub(crate) fn sheet_naming(path: &Path) -> Result<Option<PathBuf>, MediaError> {
    // A name that is no Unicode is none a sheet's text can give.
    let Some(name) = path.file_name().and_then(OsStr::to_str) else {
        return Ok(None);
    };
    let folder = path.parent().unwrap_or(Path::new(""));
    let naming = sheets_in(folder)?.into_iter().filter(|sheet| {
        let mut files = sheet.files.iter();
        files.any(|file| names_file(file, name))
    });
    one_of(preferred(naming), Search::Beside)
}

/// The paths of `sheets`: of those the readers take, or of those they refuse when they take none,
/// so that a sheet the readers refuse tells a dump only where no sheet they take does.
fn preferred(sheets: impl IntoIterator<Item = FoundSheet>) -> Vec<PathBuf> {
    let (taken, refused): (Vec<FoundSheet>, Vec<FoundSheet>) =
        sheets.into_iter().partition(|sheet| sheet.taken);
    let chosen = if taken.is_empty() { refused } else { taken };
    chosen.into_iter().map(|sheet| sheet.path).collect()
}

/// Every sheet in `folder`, which is empty when a relative path names no folder before its file,
/// by its path in the folder: each regular file there as it is listed, opened in turn, one gone
/// since the folder was listed passed over.
fn sheets_in(folder: &Path) -> Result<Vec<FoundSheet>, MediaError> {
    let listed = if folder.as_os_str().is_empty() {
        Path::new(CURRENT_FOLDER)
    } else {
        folder
    };
    let entries = fs::read_dir(listed).map_err(|error| MediaError::io(listed, error))?;
    let mut sheets = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|error| MediaError::io(listed, error))?;
        let path = folder.join(entry.file_name());
        let Some(bytes) = sheet_bytes(&path)? else {
            continue;
        };
        let found = match sheet_of(&bytes) {
            None => continue,
            Some(Ok((_, tracks))) => FoundSheet {
                path,
                taken: true,
                files: tracks.into_iter().map(|track| track.file).collect(),
            },
            Some(Err(_)) => FoundSheet {
                path,
                taken: false,
                files: named_files(&bytes),
            },
        };
        sheets.push(found);
    }
    Ok(sheets)
}

/// The files that `bytes`, a sheet the readers refuse by a rule only a sheet's text breaks, names
/// beside it, whatever rule it breaks: those a GDI's track lines give, or a cue sheet's FILE
/// lines.
fn named_files(bytes: &[u8]) -> Vec<String> {
    // Such a sheet is UTF-8 text, since text that is none is no sheet at all.
    let text = str::from_utf8(without_mark(bytes)).unwrap_or_default();
    if begins_with_count(text) {
        gdi::named_files(text)
    } else {
        cue::named_files(text)
    }
}

/// What the file at `path` holds, when it is a regular file of at most [`MAX_SHEET_LEN`] bytes,
/// opened only once seen to be one of that length at most. `None`, unread, when it is gone, is no
/// regular file or is longer, neither of which is opened, and when it grows past the bound as it
/// is read.
fn sheet_bytes(path: &Path) -> Result<Option<Vec<u8>>, MediaError> {
    let (file, len) = match open_regular_within(path, MAX_SHEET_LEN) {
        Ok(Some(opened)) => opened,
        Ok(None) => return Ok(None),
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(MediaError::io(path, error)),
    };
    read_up_to_bound(file, path, len)
}

/// Whether `listed`, a track's file as a sheet names it, is the file named `name` beside the
/// sheet, as Windows opens a file: whatever the letter case of either.
#[cfg(windows)]
fn names_file(listed: &str, name: &str) -> bool {
    listed.to_lowercase() == name.to_lowercase()
}

/// Whether `listed`, a track's file as a sheet names it, is the file named `name` beside the
/// sheet, as Linux opens a file: exactly as it is spelled.
#[cfg(unix)]
fn names_file(listed: &str, name: &str) -> bool {
    listed == name
}

/// The one path of `found`, the sheets found where `search` says, or `None` when there are
/// none. Refused when there are more.
fn one_of(mut found: Vec<PathBuf>, search: Search) -> Result<Option<PathBuf>, MediaError> {
    if found.len() > 1 {
        let words = match search {
            Search::Folder => SHEETS_IN_FOLDER,
            Search::Beside => SHEETS_BESIDE,
        };
        let detail = format!("{} {words}", found.len());
        return Err(MediaError::disc(DiscRule::SeveralSheets, detail));
    }
    Ok(found.pop())
}

/// The refusal of a dump for which no sheet was found where `search` says.
fn no_sheet(search: Search) -> MediaError {
    let detail = match search {
        Search::Folder => NO_SHEET_IN_FOLDER,
        Search::Beside => NO_SHEET_BESIDE,
    };
    MediaError::disc(DiscRule::NoSheet, detail)
}

/// What `sheet`, the file at `path`, holds, read as [`read_up_to_bound`] reads it, and refused
/// once it runs past [`MAX_SHEET_LEN`], as a sheet that has grown since its length was read does.
fn read_within_bound(sheet: impl Read, path: &Path, len: u64) -> Result<Vec<u8>, MediaError> {
    read_up_to_bound(sheet, path, len)?.ok_or_else(too_large)
}

/// What `sheet`, the file at `path`, holds: read into room for the `len` bytes its metadata gives,
/// but never for more than the bound and the byte past it, whatever `len` is, or `None` once it
/// runs past [`MAX_SHEET_LEN`], with no more than a byte past the bound held.
fn read_up_to_bound(
    sheet: impl Read,
    path: &Path,
    len: u64,
) -> Result<Option<Vec<u8>>, MediaError> {
    let room = len.min(MAX_SHEET_LEN + 1);
    let mut bytes = Vec::with_capacity(usize::try_from(room).unwrap_or_default());
    // One byte past the bound, so that a sheet that has grown is told with no more than that
    // held.
    sheet
        .take(MAX_SHEET_LEN + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| MediaError::io(path, error))?;
    Ok((bytes.len() as u64 <= MAX_SHEET_LEN).then_some(bytes))
}

/// The refusal of a sheet longer than the most a sheet may hold.
fn too_large() -> MediaError {
    let detail =
        format!("Sheet is longer than {MAX_SHEET_LEN} bytes, the most a list of tracks may hold");
    MediaError::disc(DiscRule::SheetTooLarge, detail)
}

#[cfg(test)]
mod tests {
    use std::fs::{self, File};
    use std::io::{self, ErrorKind, Read};
    use std::path::{Path, PathBuf};

    use gdl_fixtures::{DiscSet, Edc, hold_closed, holds_bind, snapshot};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::{
        MAX_SHEET_LEN, SheetKind, TrackMode, find_sheet, parse_sheet, read_sheet,
        read_within_bound, sheet_of,
    };
    use crate::vectors::{dreamcast_vectors, listed, placed, text};
    use crate::{DiscRule, MediaError};

    /// A cue sheet of one raw data track, from its file's start.
    const ONE_TRACK: &str = "FILE \"t.bin\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n";

    /// The byte order mark a sheet may begin with, as UTF-8 writes it.
    const MARK: &[u8] = b"\xef\xbb\xbf";

    /// What the launcher answers for a sheet longer than the most a sheet may hold.
    const TOO_LARGE: &str = "Sheet is longer than 65536 bytes, the most a list of tracks may hold";

    /// A cue sheet that keeps a whole disc in one file: its second track lies in the file its
    /// first was read from.
    const ONE_FILE_DISC: &str = concat!(
        "FILE \"disc.bin\" BINARY\n",
        "TRACK 01 MODE1/2352\n",
        "INDEX 01 00:00:00\n",
        "TRACK 02 AUDIO\n",
        "INDEX 01 03:00:00\n",
    );

    /// A GDI of one track of cooked 2048-byte sectors.
    const COOKED_GDI: &str = "1\r\n1 0 4 2048 track01.iso 0\r\n";

    /// What the launcher answers for a folder given as a dump that holds no sheet.
    const NO_SHEET_IN_FOLDER: &str = "No sheet in the folder lists a disc's tracks";

    /// What it answers for a file given as a dump that is no sheet, and that no sheet beside it
    /// names.
    const NO_SHEET_BESIDE: &str = "No sheet beside the file names it as a track's file";

    /// The bytes `parts` hold, one after another.
    fn joined(parts: &[&[u8]]) -> Vec<u8> {
        parts.concat()
    }

    /// `set` written into a folder of its own.
    fn written(set: &DiscSet) -> TempDir {
        let dir = tempfile::tempdir().unwrap();
        set.write(dir.path()).unwrap();
        dir
    }

    /// The sheet [`find_sheet`] finds for `path`, which must be found.
    fn found(path: &Path) -> PathBuf {
        find_sheet(path).unwrap_or_else(|error| panic!("{}: {error:?}", path.display()))
    }

    /// The rule and the words [`find_sheet`] refuses `path` with, a rule of the disc's format.
    fn refused(path: &Path) -> (&'static str, String) {
        let error = find_sheet(path).unwrap_err();
        assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
        (error.rule(), error.to_string())
    }

    /// The refusal by `rule` with `words`.
    fn said(rule: &'static str, words: &str) -> (&'static str, String) {
        (rule, words.to_owned())
    }

    #[test]
    fn sheets_are_told_from_other_files_by_their_text() {
        // What the readers take is a sheet, with the tracks they read.
        let (kind, tracks) = sheet_of(ONE_TRACK.as_bytes()).unwrap().unwrap();
        assert_eq!(kind, SheetKind::Cue);
        assert_eq!(listed(&tracks), json!([[3, "t.bin", "MODE1/2352", 0]]));
        let (kind, _) = sheet_of(b"1\r\n1 45000 4 2352 t.bin 0\r\n")
            .unwrap()
            .unwrap();
        assert_eq!(kind, SheetKind::Gdi);
        // So is text the readers refuse by a rule only a sheet's text breaks, keeping the refusal
        // reading it gives: a cue sheet that keeps a whole disc in one file, one of cooked
        // sectors, one that names a file outside its folder or a time past a second's frames,
        // and a GDI of cooked sectors, of another offset, or that lists fewer tracks than it
        // counts.
        let refusing = [
            ONE_FILE_DISC,
            "FILE \"t.iso\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n",
            "FILE \"../t.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n",
            "FILE \"t.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:75\n",
            COOKED_GDI,
            "1\r\n1 45000 4 2352 t.bin 4\r\n",
            "2\r\n1 45000 4 2352 t.bin 0\r\n",
        ];
        for text in refusing {
            let given = sheet_of(text.as_bytes()).unwrap().unwrap_err();
            let read = parse_sheet(text.as_bytes()).unwrap_err();
            let refusal = (given.rule(), given.to_string());
            assert_eq!(refusal, (read.rule(), read.to_string()), "{text:?}");
        }
        // Anything else is no sheet: bytes that are no UTF-8 text, text with a line the cue
        // sheet's reader does not read wherever it stands, and text that lists no whole track,
        // as an empty file, a file of remarks and a sheet cut short in a track do. A raw sector
        // of zeros, as an audio track's file holds, is text of one such line.
        let others: [&[u8]; 9] = [
            b"\xff\xfe binary",
            b"Gauntlet Legends (USA)\n",
            b"[package]\nname = \"gdl-media\"\n",
            b"",
            b"REM HIGH-DENSITY AREA\n",
            b"FILE \"t.bin\" BINARY\nTRACK 01 MODE1/2352\n",
            b"FILE \"t.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\nPREGAP 00:02:00\n",
            &[0; 2352],
            MARK,
        ];
        for bytes in others {
            assert!(
                sheet_of(bytes).is_none(),
                "{:?}",
                String::from_utf8_lossy(bytes)
            );
        }
    }

    #[test]
    fn a_sheet_is_found_in_its_folder_as_itself_or_beside_a_track() {
        let full = written(&DiscSet::full(Edc::Zero));
        let sheet = full.path().join("full.cue");
        // Beside it, files that are no sheet, whatever their names: text, an empty file, a sheet's
        // text past the most a sheet may hold, and a folder.
        fs::write(full.path().join("readme.cue"), "Gauntlet Legends (USA)\n").unwrap();
        fs::write(full.path().join("empty.gdi"), "").unwrap();
        let mut long = ONE_TRACK.as_bytes().to_vec();
        long.resize(MAX_SHEET_LEN as usize + 1, b' ');
        fs::write(full.path().join("long.cue"), &long).unwrap();
        fs::create_dir(full.path().join("folder.cue")).unwrap();
        let before = snapshot(full.path()).unwrap();
        // From the folder, from the sheet itself, and from each data track's file the sheet
        // names.
        let paths = [
            full.path().to_path_buf(),
            sheet.clone(),
            full.path().join("track3.bin"),
            full.path().join("track5.bin"),
        ];
        for path in paths {
            assert_eq!(found(&path), sheet, "{}", path.display());
        }
        // Every file is only read.
        assert_eq!(snapshot(full.path()).unwrap(), before);

        // An audio track's file, which the sheet names too.
        let with_audio = written(&DiscSet::full(Edc::Zero).with_audio_files());
        let sheet = with_audio.path().join("full.cue");
        assert_eq!(found(&with_audio.path().join("track4.bin")), sheet);

        // A GDI is a sheet too, found the same ways.
        let gdi = written(&DiscSet::full_gdi(Edc::Zero));
        let sheet = gdi.path().join("full.gdi");
        let paths = [
            gdi.path().to_path_buf(),
            sheet.clone(),
            gdi.path().join("track03.bin"),
            gdi.path().join("track05.bin"),
        ];
        for path in paths {
            assert_eq!(found(&path), sheet, "{}", path.display());
        }
    }

    #[test]
    fn no_sheet_or_several_are_refused() {
        // An empty folder, and one holding only a disc's track files, which no sheet names.
        let empty = tempfile::tempdir().unwrap();
        assert_eq!(refused(empty.path()), said("noSheet", NO_SHEET_IN_FOLDER));
        let mut tracks_only = DiscSet::full(Edc::Zero);
        tracks_only.files.retain(|(name, _)| name != "full.cue");
        let tracks = written(&tracks_only);
        assert_eq!(refused(tracks.path()), said("noSheet", NO_SHEET_IN_FOLDER));
        let track = tracks.path().join("track3.bin");
        assert_eq!(refused(&track), said("noSheet", NO_SHEET_BESIDE));

        // Two sheets of one disc: the folder holds several, and its tracks' files are named by
        // both. Each sheet is its own.
        let twice = written(&DiscSet::full(Edc::Zero));
        let copy = twice.path().join("copy.cue");
        fs::copy(twice.path().join("full.cue"), &copy).unwrap();
        let several = "2 sheets in the folder list a disc's tracks";
        assert_eq!(refused(twice.path()), said("severalSheets", several));
        let several = "2 sheets beside the file name it as a track's file";
        let track = twice.path().join("track3.bin");
        assert_eq!(refused(&track), said("severalSheets", several));
        assert_eq!(found(&copy), copy);

        // Sheets of two discs in one folder: the folder holds several, but each track's file is
        // named by one.
        let both = written(&DiscSet::full(Edc::Zero));
        DiscSet::full_gdi(Edc::Zero).write(both.path()).unwrap();
        let several = "2 sheets in the folder list a disc's tracks";
        assert_eq!(refused(both.path()), said("severalSheets", several));
        let cue = both.path().join("full.cue");
        assert_eq!(found(&both.path().join("track3.bin")), cue);
        let gdi = both.path().join("full.gdi");
        assert_eq!(found(&both.path().join("track03.bin")), gdi);

        // What is not there is a failure to look at it, which names its path.
        let missing = empty.path().join("missing.bin");
        let error = find_sheet(&missing).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path, source }
                if *path == missing && source.kind() == ErrorKind::NotFound),
            "{error:?}"
        );
    }

    #[test]
    fn a_sheet_the_readers_refuse_is_still_a_sheet() {
        // Alone in its folder, it is the folder's sheet; given itself it is its own; and it names
        // the file its text gives as a track's, which is told by it too. Reading it then refuses
        // it by its own rule.
        let sheets = [
            ("disc.cue", ONE_FILE_DISC, "disc.bin"),
            ("disc.gdi", COOKED_GDI, "track01.iso"),
        ];
        for (name, text, track) in sheets {
            let dir = tempfile::tempdir().unwrap();
            let path = dir.path().join(name);
            fs::write(&path, text).unwrap();
            fs::write(dir.path().join(track), b"track").unwrap();
            fs::write(dir.path().join("other.bin"), b"other").unwrap();
            assert_eq!(found(dir.path()), path);
            assert_eq!(found(&path), path);
            assert_eq!(found(&dir.path().join(track)), path, "{name}");
            // A file its text does not name is no track of it.
            let other = dir.path().join("other.bin");
            assert_eq!(refused(&other), said("noSheet", NO_SHEET_BESIDE));
            let rule = parse_sheet(text.as_bytes()).unwrap_err().rule();
            assert_eq!(read_sheet(&path).unwrap_err().rule(), rule, "{name}");
        }
        // Two of them that name one file: the folder holds several, and so does the file.
        let dir = tempfile::tempdir().unwrap();
        for name in ["one.cue", "two.cue"] {
            fs::write(dir.path().join(name), ONE_FILE_DISC).unwrap();
        }
        fs::write(dir.path().join("disc.bin"), b"track").unwrap();
        let several = "2 sheets in the folder list a disc's tracks";
        assert_eq!(refused(dir.path()), said("severalSheets", several));
        let several = "2 sheets beside the file name it as a track's file";
        let track = dir.path().join("disc.bin");
        assert_eq!(refused(&track), said("severalSheets", several));

        // Beside a sheet the readers take it is passed over, and so is a file of notes whose
        // first line is a number, which reads as a GDI's count: the folder's sheet is the one the
        // readers take, and so is the sheet of the track's file both name.
        let full = written(&DiscSet::full(Edc::Zero));
        let refused_sheet = ONE_FILE_DISC.replace("disc.bin", "track3.bin");
        fs::write(full.path().join("one-file.cue"), refused_sheet).unwrap();
        let notes = "2024\nDumped from my own disc\n";
        assert_eq!(
            parse_sheet(notes.as_bytes()).unwrap_err().rule(),
            "gdiCount"
        );
        fs::write(full.path().join("notes.txt"), notes).unwrap();
        let sheet = full.path().join("full.cue");
        assert_eq!(found(full.path()), sheet);
        assert_eq!(found(&full.path().join("track3.bin")), sheet);
    }

    #[test]
    fn a_file_too_long_to_be_a_sheet_is_never_opened() {
        // Asked apart from the holds, so that a hold which kept nothing out fails the test.
        let probe = tempfile::tempdir().unwrap();
        let binds = holds_bind(probe.path()).unwrap();
        // Beside the full set's sheet, a file one byte longer than a sheet may be, which no open
        // reaches while it is held, as a file another program holds open sharing nothing: the
        // folder's sheet is found all the same, and so is a track's file's.
        let full = written(&DiscSet::full(Edc::Zero));
        let sheet = full.path().join("full.cue");
        let long = full.path().join("dump.7z");
        File::create(&long)
            .unwrap()
            .set_len(MAX_SHEET_LEN + 1)
            .unwrap();
        let held = hold_closed(&[&long]).unwrap();
        assert_eq!(held.proven(), binds);
        assert_eq!(found(full.path()), sheet);
        assert_eq!(found(&full.path().join("track3.bin")), sheet);
        drop(held);
        if !binds {
            eprintln!(
                "only the search was checked: the proof that no long file is opened needs a user \
                 other than root, who opens entries of mode 0o000"
            );
            return;
        }
        // A file short enough to be a sheet, which might be the dump's, fails the search when it
        // cannot be read, naming it.
        let short = full.path().join("other.cue");
        fs::write(&short, ONE_TRACK).unwrap();
        let held = hold_closed(&[&short]).unwrap();
        let error = find_sheet(full.path()).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path, .. } if *path == short),
            "{error:?}"
        );
        drop(held);
    }

    #[test]
    fn a_relative_path_is_looked_for_beside_what_it_names() {
        // The tests run in the crate's folder, which holds its manifest and no sheet: a relative
        // path that names no folder before its file is looked for there.
        let manifest = Path::new("Cargo.toml");
        assert_eq!(refused(manifest), said("noSheet", NO_SHEET_BESIDE));
    }

    #[cfg(windows)]
    #[test]
    fn a_track_is_named_in_any_letter_case_on_windows() {
        // Windows opens the file a sheet names whatever the letter case of either, so a file is
        // the track the sheet names by any.
        let full = written(&DiscSet::full(Edc::Zero));
        let sheet = full.path().join("full.cue");
        assert_eq!(found(&full.path().join("TRACK3.BIN")), sheet);
        // Letters outside ASCII too.
        let dir = tempfile::tempdir().unwrap();
        let sheet = dir.path().join("names.cue");
        let text = "FILE \"Trak\u{e9}.bin\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n";
        fs::write(&sheet, text).unwrap();
        fs::write(dir.path().join("trak\u{e9}.bin"), b"track").unwrap();
        assert_eq!(found(&dir.path().join("TRAK\u{c9}.BIN")), sheet);
    }

    #[cfg(unix)]
    #[test]
    fn a_track_is_named_exactly_on_linux() {
        // Linux opens the file a sheet names exactly as it is spelled, so a file whose name
        // differs from it in letter case alone is another file, which the sheet does not name.
        let full = written(&DiscSet::full(Edc::Zero));
        let sheet = full.path().join("full.cue");
        let other = full.path().join("TRACK3.BIN");
        fs::copy(full.path().join("track3.bin"), &other).unwrap();
        assert_eq!(refused(&other), said("noSheet", NO_SHEET_BESIDE));
        assert_eq!(found(&full.path().join("track3.bin")), sheet);
        // Letters outside ASCII too.
        let dir = tempfile::tempdir().unwrap();
        let sheet = dir.path().join("names.cue");
        let text = "FILE \"Trak\u{e9}.bin\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n";
        fs::write(&sheet, text).unwrap();
        for name in ["Trak\u{e9}.bin", "trak\u{e9}.bin", "TRAK\u{c9}.BIN"] {
            fs::write(dir.path().join(name), b"track").unwrap();
        }
        assert_eq!(found(&dir.path().join("Trak\u{e9}.bin")), sheet);
        for name in ["trak\u{e9}.bin", "TRAK\u{c9}.BIN"] {
            assert_eq!(
                refused(&dir.path().join(name)),
                said("noSheet", NO_SHEET_BESIDE)
            );
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_fifo_is_never_opened_as_a_sheet_is_looked_for() {
        use crate::dump_file::fifo;

        // A FIFO in the folder, which a plain open would wait on until a writer came, is passed
        // over as the folder's sheet is looked for, and as a track's file's is.
        let full = written(&DiscSet::full(Edc::Zero));
        let sheet = full.path().join("full.cue");
        let fifo_path = full.path().join("waiting.cue");
        fifo::make(&fifo_path);
        let folder = full.path().to_path_buf();
        let (from_folder, from_track) = fifo::without_waiting(&fifo_path, move || {
            (find_sheet(&folder), find_sheet(&folder.join("track3.bin")))
        });
        assert_eq!(from_folder.unwrap(), sheet);
        assert_eq!(from_track.unwrap(), sheet);
        // Given itself, it is refused before it is opened.
        let reading = fifo_path.clone();
        let error = fifo::without_waiting(&fifo_path, move || find_sheet(&reading)).unwrap_err();
        assert!(names_no_regular_file(&error, &fifo_path), "{error:?}");
    }

    /// A sheet whose read fails, as a drive's that goes away does.
    struct FailingSheet;

    impl Read for FailingSheet {
        fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
            Err(io::Error::other("the drive went away"))
        }
    }

    #[test]
    fn the_sets_sheets_list_their_tracks() {
        let sets = [
            // LF line ends and a remark, and a second data track that keeps three seconds of
            // pregap in its file.
            (
                DiscSet::small(Edc::Zero),
                SheetKind::Cue,
                json!([
                    [3, "track3.bin", "MODE1/2352", 0, null],
                    [5, "track5.bin", "MODE1/2352", 225, null]
                ]),
            ),
            // A byte order mark, CRLF line ends and indents, and an audio track in a file the set
            // leaves out, which reading the sheet does not look for.
            (
                DiscSet::full(Edc::Zero),
                SheetKind::Cue,
                json!([
                    [3, "track3.bin", "MODE1/2352", 0, null],
                    [4, "track4.bin", "AUDIO", 150, null],
                    [5, "track5.bin", "MODE1/2352", 225, null]
                ]),
            ),
            (
                DiscSet::one_file(b"HELLO.BIN;1", &[], Edc::Zero),
                SheetKind::Cue,
                json!([[3, "track3.bin", "MODE1/2352", 0, null]]),
            ),
            // The full set's sectors behind a GDI with CRLF line ends, each track from its file's
            // first sector at the LBA the GDI gives, the audio track's file left out too.
            (
                DiscSet::full_gdi(Edc::Zero),
                SheetKind::Gdi,
                json!([
                    [1, "track03.bin", "MODE1/2352", 0, 45000],
                    [2, "track04.raw", "AUDIO", 0, 45050],
                    [3, "track05.bin", "MODE1/2352", 0, 45100]
                ]),
            ),
        ];
        for (set, kind, tracks) in sets {
            let dir = tempfile::tempdir().unwrap();
            let path = set.write(dir.path()).unwrap();
            let sheet = read_sheet(&path).unwrap();
            assert_eq!(sheet.kind, kind, "{}", set.sheet);
            assert_eq!(sheet.path, path);
            assert_eq!(placed(&sheet.tracks), tracks, "{}", set.sheet);
        }
    }

    #[test]
    fn the_gdi_set_lists_its_data_tracks_where_the_vectors_place_them() {
        // The shared vectors give the GDI set's sheet and, for each data track, its file, where
        // in the file it starts, and the LBA of its first sector.
        let vectors = dreamcast_vectors();
        let set = &vectors["sets"]["fullGdi"];
        let built = DiscSet::full_gdi(Edc::Zero);
        assert_eq!(built.sheet, text(&set["sheet"]));
        let dir = tempfile::tempdir().unwrap();
        let sheet = read_sheet(&built.write(dir.path()).unwrap()).unwrap();
        assert_eq!(sheet.kind, SheetKind::Gdi);
        let data: Vec<Value> = sheet
            .tracks
            .iter()
            .filter(|track| track.mode == TrackMode::Mode1Raw)
            .map(|track| json!([track.file, track.index, track.lba]))
            .collect();
        let expected: Vec<Value> = set["dataTracks"]
            .as_array()
            .unwrap()
            .iter()
            .map(|track| json!([track[0], track[1], track[2]]))
            .collect();
        assert_eq!(data.len(), 2);
        assert_eq!(data, expected);
    }

    #[test]
    fn a_sheet_whose_first_line_is_a_count_is_a_gdi() {
        let gdi = b"1\r\n1 45000 4 2352 t.bin 0\r\n";
        let tracks = json!([[1, "t.bin", "MODE1/2352", 0, 45000]]);
        // Whatever stands before the count that both grammars pass over: a byte order mark, empty
        // lines, and spaces and tabs, at the count's ends too. Noughts may lead the count.
        let sheets = [
            gdi.to_vec(),
            joined(&[MARK, gdi]),
            b"\r\n\n \t\n 1 \t\r\n1 45000 4 2352 t.bin 0".to_vec(),
            b"001\n1 45000 4 2352 t.bin 0\n".to_vec(),
        ];
        for bytes in sheets {
            let (kind, read) = parse_sheet(&bytes).unwrap();
            assert_eq!(kind, SheetKind::Gdi, "{bytes:?}");
            assert_eq!(placed(&read), tracks, "{bytes:?}");
        }
        // A count that a GDI may not give still makes a GDI, which is refused by it: no track, or
        // more than a disc numbers.
        for count in ["0", "100"] {
            let error = parse_sheet(format!("{count}\r\n").as_bytes()).unwrap_err();
            assert_eq!(error.rule(), "gdiCount", "{count}");
        }
        // White space and line breaks a cue sheet's reader takes but a GDI's grammar does not, on
        // the count's line or a line before it, make a first line that gives no count.
        let kept = [
            ("\u{3000}\n1\n1 45000 4 2352 t.bin 0\n", "\u{3000}"),
            ("\x0c\n1\n1 45000 4 2352 t.bin 0\n", "\x0c"),
            ("1\x1f\n1 45000 4 2352 t.bin 0\n", "1\x1f"),
            (
                "1\u{2028}1 45000 4 2352 t.bin 0\n",
                "1\u{2028}1 45000 4 2352 t.bin 0",
            ),
        ];
        for (sheet, first) in kept {
            let error = parse_sheet(sheet.as_bytes()).unwrap_err();
            assert_eq!(error.rule(), "gdiCount", "{sheet:?}");
            let said = format!("Unsupported GDI track count: {first:?}");
            assert_eq!(error.to_string(), said);
        }

        // Any other first line makes a cue sheet, none of whose lines is a number alone: a number
        // with more on its line, signed, in another script's digits, or after a remark.
        let cues = [
            ("3 tracks\n", "3 tracks"),
            ("+3\n", "+3"),
            ("\u{663}\n", "\u{663}"),
            ("REM GDI\n3\n", "3"),
        ];
        for (sheet, line) in cues {
            let error = parse_sheet(sheet.as_bytes()).unwrap_err();
            assert_eq!(error.rule(), "cueLine", "{sheet:?}");
            assert_eq!(error.to_string(), format!("Unsupported CUE line: {line}"));
        }
        // And a sheet that begins with a cue sheet's command is read as a cue sheet.
        let (kind, read) = parse_sheet(ONE_TRACK.as_bytes()).unwrap();
        assert_eq!(kind, SheetKind::Cue);
        assert_eq!(listed(&read), json!([[3, "t.bin", "MODE1/2352", 0]]));
    }

    #[test]
    fn a_sheet_over_64_kib_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("long.cue");
        // A sheet of one track, then spaces up to the bound, a line the reader passes over: the
        // bound itself is read.
        let mut text = ONE_TRACK.as_bytes().to_vec();
        text.resize(MAX_SHEET_LEN as usize, b' ');
        fs::write(&path, &text).unwrap();
        let sheet = read_sheet(&path).unwrap();
        assert_eq!(
            listed(&sheet.tracks),
            json!([[3, "t.bin", "MODE1/2352", 0]])
        );

        // A byte more, a NUL that would end the sheet on a line the reader does not read, is
        // refused for the sheet's length.
        let file = File::options().write(true).open(&path).unwrap();
        file.set_len(MAX_SHEET_LEN + 1).unwrap();
        drop(file);
        let error = read_sheet(&path).unwrap_err();
        assert!(
            matches!(
                &error,
                MediaError::Disc {
                    rule: DiscRule::SheetTooLarge,
                    ..
                }
            ),
            "{error:?}"
        );
        assert_eq!(error.rule(), "sheetTooLarge");
        assert_eq!(error.to_string(), TOO_LARGE);
        assert_eq!(MAX_SHEET_LEN, 64 * 1024);
        // Read past the bound, the same bytes would be refused for that line instead, and the
        // file is as it was.
        let bytes = fs::read(&path).unwrap();
        assert_eq!(bytes.len() as u64, MAX_SHEET_LEN + 1);
        assert_eq!(bytes.last(), Some(&0));
        assert_eq!(parse_sheet(&bytes).unwrap_err().rule(), "cueLine");
    }

    #[test]
    fn a_sheet_that_grows_as_it_is_read_is_refused() {
        let path = Path::new("grown.cue");
        // A sheet whose length was read as the bound, but which runs on as it is read: refused once
        // a byte past the bound is held, however much more it would give.
        let error = read_within_bound(io::repeat(b' '), path, MAX_SHEET_LEN).unwrap_err();
        assert_eq!(error.rule(), "sheetTooLarge");
        assert_eq!(error.to_string(), TOO_LARGE);
        // As many bytes as the bound are taken, whatever length was read before: none, or more
        // than memory could hold, which no room is made for.
        let bound = vec![b' '; MAX_SHEET_LEN as usize];
        for len in [0, u64::MAX] {
            let held = read_within_bound(&bound[..], path, len).unwrap();
            assert_eq!(held, bound, "{len}");
        }

        // A read that fails is a failure to read the sheet, which names its path.
        let error = read_within_bound(FailingSheet, path, 0).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path: at, source }
                if at == path && source.kind() == ErrorKind::Other),
            "{error:?}"
        );
        assert_eq!(error.rule(), "io");
    }

    #[test]
    fn a_sheet_is_utf_8_text_after_one_byte_order_mark() {
        // With a byte order mark or without, the sheet's text is the same.
        let sheet = ONE_TRACK.as_bytes();
        for bytes in [sheet.to_vec(), joined(&[MARK, sheet])] {
            let (kind, tracks) = parse_sheet(&bytes).unwrap();
            assert_eq!(kind, SheetKind::Cue);
            assert_eq!(listed(&tracks), json!([[3, "t.bin", "MODE1/2352", 0]]));
        }
        // Only one is dropped: a second begins the first line, which no command then begins.
        let error = parse_sheet(&joined(&[MARK, MARK, sheet])).unwrap_err();
        assert_eq!(error.rule(), "cueLine");
        assert_eq!(
            error.to_string(),
            "Unsupported CUE line: \u{feff}FILE \"t.bin\" BINARY"
        );
        // A mark alone, like no bytes, is a sheet of no track.
        for bytes in [MARK.to_vec(), Vec::new()] {
            assert_eq!(parse_sheet(&bytes).unwrap_err().rule(), "incompleteSheet");
        }

        // Bytes that are no UTF-8, wherever they stand, refuse the sheet, which says where they
        // begin in its bytes, the mark's counted: a byte that begins no character, a character cut
        // short at the end, a surrogate's code point, a NUL written in two bytes, and an é written
        // in Latin-1.
        let end = sheet.len();
        let cases = [
            (joined(&[b"\xff", sheet]), 0),
            (joined(&[MARK, b"\xff", sheet]), MARK.len()),
            (joined(&[sheet, b"\xc3"]), end),
            (joined(&[sheet, b"\xed\xa0\x80"]), end),
            (joined(&[sheet, b"\xc0\x80"]), end),
            (b"FILE \"t\xe9.bin\" BINARY\n".to_vec(), 7),
        ];
        for (bytes, at) in cases {
            let error = parse_sheet(&bytes).unwrap_err();
            assert_eq!(error.rule(), "sheetText", "{bytes:?}");
            let said = format!("Sheet is not UTF-8 text from byte {at}");
            assert_eq!(error.to_string(), said);
        }
    }

    #[test]
    fn a_sheet_that_cannot_be_read_names_its_path() {
        let dir = tempfile::tempdir().unwrap();
        let missing = dir.path().join("missing.cue");
        let error = read_sheet(&missing).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path, source }
                if *path == missing && source.kind() == ErrorKind::NotFound),
            "{error:?}"
        );
        assert_eq!(error.rule(), "io");
        // A folder is no file to read a sheet from, and is refused alike on both systems before
        // the system is asked to open it, and left as it was.
        let error = read_sheet(dir.path()).unwrap_err();
        assert!(names_no_regular_file(&error, dir.path()), "{error:?}");
        assert_eq!(error.rule(), "io");
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 0);
    }

    /// Whether `error` is the refusal of `path`, where what is there is no regular file.
    fn names_no_regular_file(error: &MediaError, path: &Path) -> bool {
        matches!(error, MediaError::Io { path: at, source }
            if at == path
                && source.kind() == ErrorKind::InvalidInput
                && source.to_string() == "the path names no regular file")
    }

    #[cfg(unix)]
    #[test]
    fn a_fifo_given_as_a_sheet_is_refused_without_waiting_for_a_writer() {
        use crate::dump_file::fifo;

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("disc.cue");
        fifo::make(&path);
        let reading = path.clone();
        let error =
            fifo::without_waiting(&path, move || read_sheet(&reading).map(drop)).unwrap_err();
        assert!(names_no_regular_file(&error, &path), "{error:?}");
    }
}
