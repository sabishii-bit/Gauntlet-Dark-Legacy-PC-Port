//! The sheet that lists a dump's track files, read as the Dreamcast extractor reads its cue sheet:
//! a small file of UTF-8 text beside the files it names, a byte order mark before its text
//! dropped, that gives each track's number, its file, what its sectors hold, and how many of its
//! file's sectors come before its first.

use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};

use super::cue;
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
    /// How many sectors of its file come before its first: the frames of the time its INDEX 01
    /// gives, a sector each.
    pub index: u64,
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

/// Reads the sheet at `path` whole, and then as [`parse_sheet`] reads its bytes. A file longer
/// than [`MAX_SHEET_LEN`] is refused by its length before a byte of it is read, and one that
/// grows past the bound as it is read is refused once it has, with no more than a byte past the
/// bound held. The file is only ever read.
pub fn read_sheet(path: &Path) -> Result<Sheet, MediaError> {
    let file = File::open(path).map_err(|error| MediaError::io(path, error))?;
    let len = file
        .metadata()
        .map_err(|error| MediaError::io(path, error))?
        .len();
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

/// The kind of sheet `bytes` hold and the tracks it lists, in its order, read as the Dreamcast
/// extractor reads a cue sheet: one byte order mark at the start is dropped, the rest must be
/// UTF-8 text, and its lines are read by the cue sheet's grammar. Refused by the first rule the
/// text breaks.
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
    Ok((SheetKind::Cue, cue::read(text)?))
}

/// What `sheet`, the file at `path`, holds: read into room for the `len` bytes its metadata gives,
/// and refused once it runs past [`MAX_SHEET_LEN`], as a sheet that has grown since its length was
/// read does, with no more than a byte past the bound held.
fn read_within_bound(sheet: impl Read, path: &Path, len: u64) -> Result<Vec<u8>, MediaError> {
    let mut bytes = Vec::with_capacity(usize::try_from(len).unwrap_or_default());
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
    use serde_json::json;

    use super::{MAX_SHEET_LEN, SheetKind, parse_sheet, read_sheet, read_within_bound};
    use crate::vectors::listed;
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
                json!([
                    [3, "track3.bin", "MODE1/2352", 0],
                    [5, "track5.bin", "MODE1/2352", 225]
                ]),
            ),
            // A byte order mark, CRLF line ends and indents, and an audio track in a file the set
            // leaves out, which reading the sheet does not look for.
            (
                DiscSet::full(Edc::Zero),
                json!([
                    [3, "track3.bin", "MODE1/2352", 0],
                    [4, "track4.bin", "AUDIO", 150],
                    [5, "track5.bin", "MODE1/2352", 225]
                ]),
            ),
            (
                DiscSet::one_file(b"HELLO.BIN;1", &[], Edc::Zero),
                json!([[3, "track3.bin", "MODE1/2352", 0]]),
            ),
        ];
        for (set, tracks) in sets {
            let dir = tempfile::tempdir().unwrap();
            let path = set.write(dir.path()).unwrap();
            let sheet = read_sheet(&path).unwrap();
            assert_eq!(sheet.kind, SheetKind::Cue, "{}", set.sheet);
            assert_eq!(sheet.path, path);
            assert_eq!(listed(&sheet.tracks), tracks, "{}", set.sheet);
        }
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
        // refused by the file's length, before a byte of it is read.
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
        // As many bytes as the bound are taken, whatever length was read before.
        let bound = vec![b' '; MAX_SHEET_LEN as usize];
        let held = read_within_bound(&bound[..], path, 0).unwrap();
        assert_eq!(held, bound);

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
        // A folder is no file to read a sheet from, on either system, and is left as it was.
        let error = read_sheet(dir.path()).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path, .. } if path == dir.path()),
            "{error:?}"
        );
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 0);
    }
}
