//! The sheet that lists a dump's track files: a small file of UTF-8 text beside the files it
//! names, a byte order mark before its text dropped, that gives each track's number, its file,
//! what its sectors hold and where its first sector lies. It is a cue sheet, read as the Dreamcast
//! extractor reads one, or a GDI, which begins with the count of its tracks.

use std::io::Read;
use std::path::{Path, PathBuf};

use super::{cue, gdi, is_number, lines, strip};
use crate::dump_file::open_regular;
use crate::{DiscRule, MediaError};

/// The most bytes a sheet may hold: far more than any disc's list of tracks needs, so that a large
/// file given as a sheet is refused before it is read.
pub const MAX_SHEET_LEN: u64 = 64 << 10;

/// The byte order mark a sheet's text may begin with, which is no part of its first line.
const BYTE_ORDER_MARK: &str = "\u{feff}";

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
    let unmarked = bytes
        .strip_prefix(BYTE_ORDER_MARK.as_bytes())
        .unwrap_or(bytes);
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

/// What `sheet`, the file at `path`, holds: read into room for the `len` bytes its metadata gives,
/// but never for more than the bound and the byte past it, whatever `len` is, and refused once it
/// runs past [`MAX_SHEET_LEN`], as a sheet that has grown since its length was read does, with no
/// more than a byte past the bound held.
fn read_within_bound(sheet: impl Read, path: &Path, len: u64) -> Result<Vec<u8>, MediaError> {
    let room = len.min(MAX_SHEET_LEN + 1);
    let mut bytes = Vec::with_capacity(usize::try_from(room).unwrap_or_default());
    // One byte past the bound, so that a sheet that has grown is refused with no more than that
    // held.
    sheet
        .take(MAX_SHEET_LEN + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| MediaError::io(path, error))?;
    if bytes.len() as u64 > MAX_SHEET_LEN {
        return Err(too_large());
    }
    Ok(bytes)
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
    use std::path::Path;

    use gdl_fixtures::{DiscSet, Edc};
    use serde_json::{Value, json};

    use super::{MAX_SHEET_LEN, SheetKind, TrackMode, parse_sheet, read_sheet, read_within_bound};
    use crate::vectors::{dreamcast_vectors, listed, placed, text};
    use crate::{DiscRule, MediaError};

    /// A cue sheet of one raw data track, from its file's start.
    const ONE_TRACK: &str = "FILE \"t.bin\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n";

    /// The byte order mark a sheet may begin with, as UTF-8 writes it.
    const MARK: &[u8] = b"\xef\xbb\xbf";

    /// What the launcher answers for a sheet longer than the most a sheet may hold.
    const TOO_LARGE: &str = "Sheet is longer than 65536 bytes, the most a list of tracks may hold";

    /// The bytes `parts` hold, one after another.
    fn joined(parts: &[&[u8]]) -> Vec<u8> {
        parts.concat()
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
