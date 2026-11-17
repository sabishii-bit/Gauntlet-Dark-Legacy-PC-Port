//! A cue sheet as the Dreamcast extractor reads one: each line stripped, empty lines and remarks
//! passed over, and then only the three commands a dump of raw track files, a file a track, needs,
//! each in the one spelling the extractor takes. `FILE "<name>" BINARY` names the file beside the
//! sheet that the next track lies in, `TRACK <number> <mode>` opens a track of audio or of raw
//! mode 1 data in it, and `INDEX 01 <minute>:<second>:<frame>` closes the track at the time it
//! starts in its file, after the `INDEX 00` of a pregap, which is passed over. Every number is
//! written in ASCII digits, and none may pass the width it is kept in.

use std::collections::BTreeSet;

use gdl_files::{TRACK_NAMES, check_component};

use super::{
    FRAMES_PER_SECOND, SECONDS_PER_MINUTE, SheetTrack, TrackMode, is_number, lines, strip,
    too_large, unsafe_name,
};
use crate::error::long_start;
use crate::{DiscRule, MediaError};

/// What begins a remark, which the reader passes over: the word and one space.
const REMARK: &str = "REM ";

/// What a FILE line begins with, up to the quote that opens the file's name.
const FILE_START: &str = "FILE \"";

/// What a FILE line ends with, from the quote that closes the file's name: the one type of file a
/// dump of raw track files holds.
const FILE_END: &str = "\" BINARY";

/// The quote a file's name lies between, and which the name may not hold.
const QUOTE: char = '"';

/// What a TRACK line begins with, up to the track's number.
const TRACK_START: &str = "TRACK ";

/// What an INDEX line begins with, up to the index's point.
const INDEX_START: &str = "INDEX ";

/// What stands between a TRACK or INDEX line's fields: one space.
const FIELD_SEPARATOR: char = ' ';

/// What stands between the minute, the second and the frame of a time.
const TIME_SEPARATOR: char = ':';

/// How many numbers a time gives: its minute, second and frame.
const TIME_PARTS: usize = 3;

/// The mode of an audio track.
const AUDIO: &str = "AUDIO";

/// The mode of a data track kept as raw sectors.
const MODE1_RAW: &str = "MODE1/2352";

/// The other modes a cue sheet may give a track: data kept as cooked sectors, mode 2, CD+G and
/// CD-i, none of which a dump of a GD-ROM's tracks keeps.
const OTHER_MODES: [&str; 6] = [
    "CDG",
    "MODE1/2048",
    "MODE2/2336",
    "MODE2/2352",
    "CDI/2336",
    "CDI/2352",
];

/// The INDEX point where a track's pregap begins, which the reader passes over.
const PREGAP_POINT: &str = "00";

/// The INDEX point where a track begins, which closes the track.
const START_POINT: &str = "01";

/// The seconds of a minute of a cue sheet's times, which count the disc's time as a raw sector's
/// address does.
const SECONDS: u64 = SECONDS_PER_MINUTE.unsigned_abs();

/// The frames of a second of a cue sheet's times, a sector each.
const FRAMES: u64 = FRAMES_PER_SECOND.unsigned_abs();

/// What the Dreamcast extractor answers for a FILE line while a track has no INDEX 01 yet.
const MISSING_INDEX: &str = "Track is missing INDEX 01";

/// What it answers for a track opened while another is open, where no file is named, or in a file
/// a track was read from already.
const NOT_ONE_TRACK_PER_FILE: &str = "Expected one complete track per BIN file";

/// What it answers for a time whose second or frame is past the last.
const INVALID_TIME: &str = "Invalid CUE time";

/// What it answers a line it does not read with, before the line.
const UNSUPPORTED_LINE: &str = "Unsupported CUE line";

/// What it answers for a sheet of no track, or one that ends with a track open.
const INCOMPLETE: &str = "Incomplete CUE";

/// What it answers for two tracks of one number.
const DUPLICATE_TRACK: &str = "Duplicate track number";

/// What the reader answers a track of another mode with, before the mode.
const UNSUPPORTED_MODE: &str = "Unsupported CUE track mode";

/// What a track number past the most it may be is called in its refusal.
const TRACK_NUMBER: &str = "CUE track number";

/// What a number of a time past the most it may be is called in its refusal.
const TIME_NUMBER: &str = "CUE time number";

/// What a time whose frames come to more than the most they may is called in its refusal.
const TIME_FRAMES: &str = "CUE time in frames";

/// The tracks `text`, a cue sheet's text without its byte order mark, lists, in its order, read
/// as the Dreamcast extractor reads them, or the first rule of the sheet's grammar it breaks.
pub(super) fn read(text: &str) -> Result<Vec<SheetTrack>, MediaError> {
    let mut reader = CueReader::default();
    for line in lines(text).map(strip) {
        reader.read_line(line)?;
    }
    reader.finish()
}

/// The name every FILE line of `text`, a cue sheet's text without its byte order mark, gives, in
/// its order, whatever else the sheet holds: the files beside it that a sheet the reader refuses
/// still names.
pub(super) fn named_files(text: &str) -> Vec<String> {
    let named = lines(text)
        .map(strip)
        .filter_map(|line| match line_of(line) {
            Line::File(name) => Some(name.to_owned()),
            _ => None,
        });
    named.collect()
}

/// What a stripped line of a cue sheet is, by its form alone.
enum Line<'a> {
    /// An empty line or a remark, which the reader passes over.
    Passed,
    /// A FILE line, and the name it gives: one or more characters, none of them a quote.
    File(&'a str),
    /// A TRACK line of a mode the reader reads, and its number, one or more ASCII digits.
    Track { number: &'a str, mode: TrackMode },
    /// A TRACK line of another mode a cue sheet may give, and the mode.
    OtherMode(&'a str),
    /// An INDEX line of point 00 or 01, whether it is 01, and its time's minute, second and frame,
    /// each one or more ASCII digits.
    Index {
        start: bool,
        time: [&'a str; TIME_PARTS],
    },
    /// Any other line.
    Other,
}

/// A track a TRACK line has opened and no INDEX 01 has closed yet.
#[derive(Clone, Copy)]
struct OpenTrack<'a> {
    /// Its number.
    number: u32,
    /// What its sectors hold.
    mode: TrackMode,
    /// The file it lies in, as the sheet names it.
    file: &'a str,
}

/// What a cue sheet has given so far, as its lines are read in turn.
#[derive(Default)]
struct CueReader<'a> {
    /// The file the last FILE line named, which the next track lies in.
    file: Option<&'a str>,
    /// The track that is open, until its INDEX 01 closes it.
    open: Option<OpenTrack<'a>>,
    /// The files tracks were read from, in lower case: whatever the letter case a sheet names a
    /// file in, each track must lie in a file of its own.
    used: BTreeSet<String>,
    /// The tracks closed so far, in the sheet's order.
    tracks: Vec<SheetTrack>,
}

impl<'a> CueReader<'a> {
    /// Reads `line`, stripped: passed over, taken, or refused by the first rule it breaks.
    fn read_line(&mut self, line: &'a str) -> Result<(), MediaError> {
        match line_of(line) {
            Line::Passed => Ok(()),
            Line::File(name) => self.name_file(name),
            Line::Track { number, mode } => self.open_track(number, mode),
            Line::OtherMode(mode) => {
                let detail = format!("{UNSUPPORTED_MODE}: {mode}");
                Err(MediaError::disc(DiscRule::TrackMode, detail))
            }
            Line::Index { start, time } => match self.open {
                Some(track) => self.index(track, start, time),
                // An INDEX line where no track is open is no line the extractor reads.
                None => Err(unsupported_line(line)),
            },
            Line::Other => Err(unsupported_line(line)),
        }
    }

    /// Takes `name` for the file the next track lies in, once the track before has its INDEX 01:
    /// a name the folder beside the sheet may hold under the rules of a track file's name.
    fn name_file(&mut self, name: &'a str) -> Result<(), MediaError> {
        if self.open.is_some() {
            return Err(MediaError::disc(DiscRule::MissingIndex, MISSING_INDEX));
        }
        check_component(name, &TRACK_NAMES)
            .map_err(|refused| unsafe_name(DiscRule::TrackName, name, refused.rule))?;
        self.file = Some(name);
        Ok(())
    }

    /// Opens the track of `number` and `mode` in the file the last FILE line named, which no track
    /// may lie in already, while no other track is open. Its number must fit 32 bits.
    fn open_track(&mut self, number: &str, mode: TrackMode) -> Result<(), MediaError> {
        let unused = self
            .file
            .filter(|file| !self.used.contains(&file.to_lowercase()));
        let (None, Some(file)) = (self.open, unused) else {
            return Err(MediaError::disc(
                DiscRule::TrackPerFile,
                NOT_ONE_TRACK_PER_FILE,
            ));
        };
        let number = number
            .parse()
            .map_err(|_| too_large(TRACK_NUMBER, u64::from(u32::MAX)))?;
        self.open = Some(OpenTrack { number, mode, file });
        Ok(())
    }

    /// Reads the time of an INDEX line of `track`: of point 01 when `start`, which closes the
    /// track where it starts in its file, and otherwise of point 00, which is passed over once its
    /// time is read. The time's numbers must fit 64 bits, its second be under a minute's and its
    /// frame under a second's, and the frames it comes to must fit 64 bits too.
    fn index(
        &mut self,
        track: OpenTrack<'a>,
        start: bool,
        time: [&str; TIME_PARTS],
    ) -> Result<(), MediaError> {
        let [Some(minute), Some(second), Some(frame)] =
            time.map(|number| number.parse::<u64>().ok())
        else {
            return Err(too_large(TIME_NUMBER, u64::MAX));
        };
        if second >= SECONDS || frame >= FRAMES {
            return Err(MediaError::disc(DiscRule::CueTime, INVALID_TIME));
        }
        if !start {
            return Ok(());
        }
        let index = minute
            .checked_mul(SECONDS)
            .and_then(|seconds| seconds.checked_add(second))
            .and_then(|seconds| seconds.checked_mul(FRAMES))
            .and_then(|frames| frames.checked_add(frame))
            .ok_or_else(|| too_large(TIME_FRAMES, u64::MAX))?;
        self.used.insert(track.file.to_lowercase());
        self.tracks.push(SheetTrack {
            number: track.number,
            file: track.file.to_owned(),
            mode: track.mode,
            index,
            lba: None,
        });
        self.open = None;
        Ok(())
    }

    /// The tracks read, once every line is: refused when there are none, when the last is still
    /// open, or when two share a number.
    fn finish(self) -> Result<Vec<SheetTrack>, MediaError> {
        if self.open.is_some() || self.tracks.is_empty() {
            return Err(MediaError::disc(DiscRule::IncompleteSheet, INCOMPLETE));
        }
        let numbers: BTreeSet<u32> = self.tracks.iter().map(|track| track.number).collect();
        if numbers.len() != self.tracks.len() {
            return Err(MediaError::disc(DiscRule::DuplicateTrack, DUPLICATE_TRACK));
        }
        Ok(self.tracks)
    }
}

/// What `line`, stripped, is by its form alone, each command in the one spelling the extractor
/// takes.
fn line_of(line: &str) -> Line<'_> {
    if line.is_empty() || line.starts_with(REMARK) {
        Line::Passed
    } else if let Some(name) = file_name(line) {
        Line::File(name)
    } else if let Some((number, mode)) = track_fields(line) {
        match mode {
            AUDIO => Line::Track {
                number,
                mode: TrackMode::Audio,
            },
            MODE1_RAW => Line::Track {
                number,
                mode: TrackMode::Mode1Raw,
            },
            mode if OTHER_MODES.contains(&mode) => Line::OtherMode(mode),
            _ => Line::Other,
        }
    } else if let Some((point, time)) = index_fields(line) {
        match point {
            PREGAP_POINT => Line::Index { start: false, time },
            START_POINT => Line::Index { start: true, time },
            _ => Line::Other,
        }
    } else {
        Line::Other
    }
}

/// The name a `FILE "<name>" BINARY` line gives: one or more characters, none of them a quote.
fn file_name(line: &str) -> Option<&str> {
    let name = line.strip_prefix(FILE_START)?.strip_suffix(FILE_END)?;
    (!name.is_empty() && !name.contains(QUOTE)).then_some(name)
}

/// The number and the mode a `TRACK <number> <mode>` line gives, its number in ASCII digits.
fn track_fields(line: &str) -> Option<(&str, &str)> {
    let (number, mode) = line
        .strip_prefix(TRACK_START)?
        .split_once(FIELD_SEPARATOR)?;
    is_number(number).then_some((number, mode))
}

/// The point and the time an `INDEX <point> <minute>:<second>:<frame>` line gives, each number of
/// the time in ASCII digits.
fn index_fields(line: &str) -> Option<(&str, [&str; TIME_PARTS])> {
    let (point, time) = line
        .strip_prefix(INDEX_START)?
        .split_once(FIELD_SEPARATOR)?;
    let mut numbers = time.split(TIME_SEPARATOR);
    let time = [numbers.next()?, numbers.next()?, numbers.next()?];
    let whole = numbers.next().is_none() && time.iter().all(|number| is_number(number));
    whole.then_some((point, time))
}

/// The refusal of `line`, which the extractor does not read: the line after its words for one,
/// or past [`MAX_QUOTED_CHARS`](crate::error::MAX_QUOTED_CHARS) characters its length and its
/// start, quoted.
fn unsupported_line(line: &str) -> MediaError {
    let detail = match long_start(line) {
        Some((len, start)) => {
            format!("{UNSUPPORTED_LINE} of {len} characters beginning {start:?}")
        }
        None => format!("{UNSUPPORTED_LINE}: {line}"),
    };
    MediaError::disc(DiscRule::CueLine, detail)
}

#[cfg(test)]
mod tests {
    use gdl_fixtures::unhex;
    use serde_json::{Value, json};

    use super::{OTHER_MODES, named_files, read};
    use crate::error::MAX_QUOTED_CHARS;
    use crate::vectors::{dreamcast_vectors, listed, text};
    use crate::{MediaError, SheetKind, parse_sheet};

    /// The system the tests run on, as the vectors key a value where the extractor's outcome
    /// differs between systems.
    const THIS_SYSTEM: &str = if cfg!(windows) { "windows" } else { "linux" };

    /// The rules whose refusals keep the extractor's own words, which are fixed, or a line after
    /// them.
    const SCRIPT_WORDS: [&str; 6] = [
        "cueLine",
        "cueTime",
        "duplicateTrack",
        "incompleteSheet",
        "missingIndex",
        "trackPerFile",
    ];

    /// What the extractor answers for a track opened in a file a track was read from already.
    const NOT_ONE_TRACK_PER_FILE: &str = "Expected one complete track per BIN file";

    /// What the reader gives a sheet: its tracks as the vectors list them, or the rule and the
    /// words it refuses the sheet with.
    type Outcome = Result<Value, (&'static str, String)>;

    /// What the reader gives the bytes of a sheet: by a rule of the disc's format when it refuses
    /// them, never by a failure to read.
    fn outcome(sheet: &[u8]) -> Outcome {
        match parse_sheet(sheet) {
            Ok((kind, tracks)) => {
                assert_eq!(kind, SheetKind::Cue);
                Ok(listed(&tracks))
            }
            Err(error) => {
                assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
                Err((error.rule(), error.to_string()))
            }
        }
    }

    /// What the reader gives `text`.
    fn outcome_of(text: &str) -> Outcome {
        outcome(text.as_bytes())
    }

    /// The refusal by `rule` with `words`.
    fn refused(rule: &'static str, words: &str) -> Outcome {
        Err((rule, words.to_owned()))
    }

    /// A cue sheet of one raw data track, from the start of the file `name`, as the extractor's
    /// tests write one for a name.
    fn one_track(name: &str) -> String {
        format!("FILE \"{name}\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n")
    }

    /// What the file of a sheet case holds: its text in UTF-8, or the bytes it gives where it is
    /// no UTF-8.
    fn sheet_bytes(case: &Value) -> Vec<u8> {
        match case.get("bytes") {
            Some(bytes) => unhex(bytes),
            None => text(&case["text"]).as_bytes().to_vec(),
        }
    }

    /// A case's value on the system the tests run on: the value itself, or where the extractor's
    /// outcome differs between systems, this system's.
    fn on_this_system(value: &Value) -> &Value {
        if value.is_object() {
            &value[THIS_SYSTEM]
        } else {
            value
        }
    }

    /// Every character Unicode counts as a number: the decimal digits of every script, numerals
    /// that are letters, and the rest, such as superscripts and fractions.
    fn numeric_chars() -> impl Iterator<Item = char> {
        (0..=u32::from(char::MAX))
            .filter_map(char::from_u32)
            .filter(|character| character.is_numeric())
    }

    #[test]
    fn sheets_follow_the_shared_vectors() {
        let vectors = dreamcast_vectors();
        let cases = vectors["sheets"].as_array().unwrap();
        assert!(!cases.is_empty());
        let (mut rules, mut deviations) = (Vec::new(), Vec::new());
        for case in cases {
            let python = on_this_system(&case["python"]);
            match (outcome(&sheet_bytes(case)), case["rule"].as_str()) {
                (Ok(tracks), None) => {
                    assert!(python.is_null(), "{case}");
                    assert_eq!(&tracks, on_this_system(&case["tracks"]), "{case}");
                }
                (Err((rule, detail)), Some(expected)) => {
                    assert_eq!(rule, expected, "{case}");
                    if let Some(name_rule) = case["nameRule"].as_str() {
                        // The launcher's own words, which name the rule of names the file's name
                        // breaks.
                        assert!(detail.starts_with("Unsafe disc name: "), "{detail}");
                        assert!(detail.ends_with(&format!(" ({name_rule})")), "{detail}");
                    } else if let Some(words) = python.as_str()
                        && SCRIPT_WORDS.contains(&rule)
                    {
                        assert_eq!(detail, words, "{case}");
                    }
                    // Where the extractor takes a sheet the launcher refuses, a deviation says why.
                    assert!(!python.is_null() || case["deviation"].is_u64(), "{case}");
                    rules.push(rule);
                }
                (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
            }
            deviations.extend(case["deviation"].as_u64());
        }
        rules.sort_unstable();
        rules.dedup();
        let every_rule = [
            "cueLine",
            "cueTime",
            "duplicateTrack",
            "incompleteSheet",
            "missingIndex",
            "numberTooLarge",
            "sheetText",
            "trackMode",
            "trackName",
            "trackPerFile",
        ];
        assert_eq!(rules, every_rule);
        // The sheets the extractor takes and the launcher refuses: numbers in digits other than
        // ASCII's, numbers past the widths they are kept in, and one file named in two letter
        // cases, which the extractor refuses on Windows alone.
        deviations.sort_unstable();
        deviations.dedup();
        assert_eq!(deviations, [7, 8, 9]);
    }

    #[test]
    fn track_names_follow_the_shared_vectors() {
        let vectors = dreamcast_vectors();
        let cases = vectors["names"].as_array().unwrap();
        assert!(!cases.is_empty());
        let mut deviations = Vec::new();
        for case in cases {
            let name = text(&case["name"]);
            let track = &case["track"];
            match (outcome_of(&one_track(name)), track["rule"].as_str()) {
                (Ok(tracks), None) => {
                    assert!(track["python"].is_null(), "{case}");
                    assert_eq!(tracks, json!([[3, name, "MODE1/2352", 0]]), "{case}");
                }
                (Err((rule, detail)), Some(expected)) => {
                    assert_eq!(rule, expected, "{case}");
                    match track["nameRule"].as_str() {
                        // The launcher's own words, which quote the name and name the rule of
                        // names it breaks.
                        Some(name_rule) => {
                            let said = format!("Unsafe disc name: {name:?} ({name_rule})");
                            assert_eq!(detail, said, "{case}");
                        }
                        // A FILE line holds no empty name: the extractor's own words for a line it
                        // does not read.
                        None => assert_eq!(detail, text(&track["python"]), "{case}"),
                    }
                }
                (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
            }
            deviations.extend(track["deviation"].as_u64());
        }
        // The names the extractor takes and the launcher refuses: a delete character, a name of
        // more than 200 characters, the alpha installer's names, a device name followed by spaces,
        // and one with a superscript digit.
        deviations.sort_unstable();
        deviations.dedup();
        assert_eq!(deviations, [1, 2, 3, 4, 5]);
    }

    #[test]
    fn numbers_are_ascii_digits_only() {
        // A number in the track's place and in each of its time's: alone, and after a digit.
        let places: [fn(&str) -> String; 4] = [
            |number| format!("FILE \"t.bin\" BINARY\nTRACK {number} MODE1/2352\nINDEX 01 0:0:0\n"),
            |minute| format!("FILE \"t.bin\" BINARY\nTRACK 3 MODE1/2352\nINDEX 01 {minute}:0:0\n"),
            |second| format!("FILE \"t.bin\" BINARY\nTRACK 3 MODE1/2352\nINDEX 01 0:{second}:0\n"),
            |frame| format!("FILE \"t.bin\" BINARY\nTRACK 3 MODE1/2352\nINDEX 01 0:0:{frame}\n"),
        ];
        // Every character Unicode counts as a number is read only when it is an ASCII digit: the
        // decimal digits of other scripts, which the extractor reads too, make a line the reader
        // does not read.
        let mut digits = 0;
        for numeral in numeric_chars() {
            let ascii = numeral.is_ascii_digit();
            digits += usize::from(ascii);
            for (place, sheet) in places.iter().enumerate() {
                for number in [numeral.to_string(), format!("1{numeral}")] {
                    let given = outcome_of(&sheet(&number));
                    if ascii {
                        assert!(given.is_ok(), "{place} {number:?}: {given:?}");
                    } else {
                        assert!(
                            matches!(&given, Err(("cueLine", _))),
                            "{place} {number:?}: {given:?}"
                        );
                    }
                }
            }
        }
        assert_eq!(digits, 10);
        // Nor is a number signed, empty, spaced from its separator, written with a point or in
        // another base.
        for number in ["+3", "-3", "", " 3", "3.0", "0x3", "3e1", "\u{663}"] {
            for (place, sheet) in places.iter().enumerate() {
                let given = outcome_of(&sheet(number));
                assert!(
                    matches!(&given, Err(("cueLine", _))),
                    "{place} {number:?}: {given:?}"
                );
            }
        }
        // An INDEX line's point is 00 or 01 in ASCII digits.
        for point in ["\u{660}\u{661}", "0\u{661}", "1", "001", "02"] {
            let sheet = format!("FILE \"t.bin\" BINARY\nTRACK 03 AUDIO\nINDEX {point} 0:0:0\n");
            let given = outcome_of(&sheet);
            assert!(
                matches!(&given, Err(("cueLine", _))),
                "{point:?}: {given:?}"
            );
        }
        // Leading noughts, however many, are read as the number they lead.
        let zeros = "0".repeat(40);
        let sheet = format!(
            "FILE \"t.bin\" BINARY\nTRACK {zeros}7 AUDIO\nINDEX 01 {zeros}1:{zeros}2:{zeros}3\n"
        );
        assert_eq!(
            outcome_of(&sheet),
            Ok(json!([[7, "t.bin", "AUDIO", 75 * 62 + 3]]))
        );
    }

    #[test]
    fn one_track_per_file_ignores_letter_case_on_both_systems() {
        let two_tracks = |first: &str, second: &str| {
            format!(
                "FILE \"{first}\" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n\
                 FILE \"{second}\" BINARY\nTRACK 04 AUDIO\nINDEX 01 00:00:00\n"
            )
        };
        // A file named again, in its own letter case or another, outside ASCII's too, holds a
        // track already, on every system.
        let same = [
            ("t.bin", "t.bin"),
            ("t.bin", "T.BIN"),
            ("Track 3.Bin", "tRACK 3.bIN"),
            ("\u{e9}.bin", "\u{c9}.BIN"),
            ("\u{3a3}.bin", "\u{3c3}.bin"),
        ];
        for (first, second) in same {
            assert_eq!(
                outcome_of(&two_tracks(first, second)),
                refused("trackPerFile", NOT_ONE_TRACK_PER_FILE),
                "{first} {second}"
            );
        }
        // Names that differ by more than letter case are two files, each read as it is spelled:
        // a sharp s lowers to itself, not to the two letters it raises to.
        let other = [
            ("t.bin", "u.bin"),
            ("t.bin", "t.bin2"),
            ("\u{df}.bin", "SS.bin"),
        ];
        for (first, second) in other {
            assert_eq!(
                outcome_of(&two_tracks(first, second)),
                Ok(json!([
                    [3, first, "MODE1/2352", 0],
                    [4, second, "AUDIO", 0]
                ])),
                "{first} {second}"
            );
        }
        // A file named twice before its one track is read is one file.
        let named_twice = "FILE \"t.bin\" BINARY\nFILE \"T.BIN\" BINARY\n\
                           TRACK 03 MODE1/2352\nINDEX 01 00:00:00\n";
        assert_eq!(
            outcome_of(named_twice),
            Ok(json!([[3, "T.BIN", "MODE1/2352", 0]]))
        );
    }

    #[test]
    fn each_command_is_read_in_its_one_spelling() {
        let file = "FILE \"t.bin\" BINARY";
        let track = "TRACK 03 MODE1/2352";
        let index = "INDEX 01 00:00:00";
        let unread = [
            // FILE with another type, without quotes, or with more after it.
            (
                format!("FILE \"t.bin\" WAVE\n{track}\n{index}\n"),
                "FILE \"t.bin\" WAVE",
            ),
            (
                format!("FILE t.bin BINARY\n{track}\n{index}\n"),
                "FILE t.bin BINARY",
            ),
            (
                format!("{file} x\n{track}\n{index}\n"),
                "FILE \"t.bin\" BINARY x",
            ),
            // TRACK without a mode, with two spaces before it, or with more after it.
            (format!("{file}\nTRACK 03\n{index}\n"), "TRACK 03"),
            (
                format!("{file}\nTRACK 03  AUDIO\n{index}\n"),
                "TRACK 03  AUDIO",
            ),
            (
                format!("{file}\nTRACK 03 AUDIO x\n{index}\n"),
                "TRACK 03 AUDIO x",
            ),
            (
                format!("{file}\nTRACK 03 audio\n{index}\n"),
                "TRACK 03 audio",
            ),
            // INDEX with two numbers in its time or four, or a point of one digit.
            (
                format!("{file}\n{track}\nINDEX 01 00:00\n"),
                "INDEX 01 00:00",
            ),
            (
                format!("{file}\n{track}\nINDEX 01 0:0:0:0\n"),
                "INDEX 01 0:0:0:0",
            ),
            (
                format!("{file}\n{track}\nINDEX 1 00:00:00\n"),
                "INDEX 1 00:00:00",
            ),
            (
                format!("{file}\n{track}\nINDEX 01 00:00:\n"),
                "INDEX 01 00:00:",
            ),
            // A remark is the word and a space, in capitals.
            (format!("rem x\n{file}\n{track}\n{index}\n"), "rem x"),
            (format!("REMARK x\n{file}\n{track}\n{index}\n"), "REMARK x"),
        ];
        for (sheet, line) in unread {
            let words = format!("Unsupported CUE line: {line}");
            assert_eq!(outcome_of(&sheet), refused("cueLine", &words), "{sheet:?}");
        }

        // Every other mode a cue sheet may give a track is one the reader knows and refuses.
        for mode in OTHER_MODES {
            let sheet = format!("{file}\nTRACK 03 {mode}\n{index}\n");
            let words = format!("Unsupported CUE track mode: {mode}");
            assert_eq!(outcome_of(&sheet), refused("trackMode", &words), "{mode}");
        }

        // Remarks and empty lines are passed over wherever they stand, between a track and its
        // INDEX too, and an INDEX 00 of a pregap is read and passed over.
        let sheet = format!(
            "REM a\n\n{file}\nREM b\n  \t\nTRACK 03 MODE1/2352\nREM c\nINDEX 00 00:00:00\n\
             \u{3000}\nINDEX 01 00:02:00\nREM d"
        );
        assert_eq!(
            outcome_of(&sheet),
            Ok(json!([[3, "t.bin", "MODE1/2352", 150]]))
        );
    }

    #[test]
    fn each_line_is_judged_in_a_fixed_order() {
        // Each sheet breaks two rules. Where both are the extractor's, the one it judges first
        // refuses the sheet. The rules of the launcher's own, a mode it knows and a number past
        // the width it is kept in, are judged first, on sheets the extractor refuses too, for a
        // line it does not read or a time past the last.
        let open = "FILE \"t.bin\" BINARY\nTRACK 03 AUDIO\n";
        let cases = [
            // A FILE line while a track is open, before its name is judged.
            (format!("{open}FILE \"../u.bin\" BINARY\n"), "missingIndex"),
            // A TRACK line while another is open, or with no file named, before its number.
            (format!("{open}TRACK 4294967296 AUDIO\n"), "trackPerFile"),
            ("TRACK 4294967296 AUDIO\n".to_owned(), "trackPerFile"),
            ("TRACK 03 AUDIO\n".to_owned(), "trackPerFile"),
            // A TRACK line of another mode, whatever is open and whatever its number.
            (format!("{open}TRACK 4 MODE2/2352\n"), "trackMode"),
            ("TRACK 4294967296 CDG\n".to_owned(), "trackMode"),
            // A number past 64 bits in an INDEX line's time, before its second and frame.
            (
                format!("{open}INDEX 01 0:99999999999999999999:0\n"),
                "numberTooLarge",
            ),
            (
                format!("{open}INDEX 00 99999999999999999999:60:75\n"),
                "numberTooLarge",
            ),
            // An INDEX 00's second and frame are judged too, though nothing else of it is kept.
            (format!("{open}INDEX 00 00:60:00\n"), "cueTime"),
            (format!("{open}INDEX 00 00:00:75\n"), "cueTime"),
            // An INDEX line where no track is open, whatever its time.
            (
                "FILE \"t.bin\" BINARY\nINDEX 01 00:60:00\n".to_owned(),
                "cueLine",
            ),
            (
                "FILE \"t.bin\" BINARY\nINDEX 01 99999999999999999999:00:00\n".to_owned(),
                "cueLine",
            ),
            // A sheet that ends with a track open, before two tracks of one number.
            (
                format!("{open}INDEX 01 00:00:00\nFILE \"u.bin\" BINARY\nTRACK 03 AUDIO\n"),
                "incompleteSheet",
            ),
        ];
        for (sheet, rule) in cases {
            let given = outcome_of(&sheet);
            assert!(
                matches!(&given, Err((found, _)) if *found == rule),
                "{sheet:?}: {given:?}"
            );
        }

        // Only an INDEX 01's time is counted in frames: an INDEX 00's minute may be one whose
        // frames would not fit 64 bits.
        let sheet = format!("{open}INDEX 00 4099276460824345:00:00\nINDEX 01 00:00:01\n");
        assert_eq!(outcome_of(&sheet), Ok(json!([[3, "t.bin", "AUDIO", 1]])));
        // The largest time whose frames fit 64 bits is read, and a frame more is not.
        let most = u64::MAX;
        let minutes = most / (60 * 75);
        let rest = most - minutes * 60 * 75;
        let (second, frame) = (rest / 75, rest % 75);
        let last = format!("{open}INDEX 01 {minutes}:{second}:{frame}\n");
        assert_eq!(outcome_of(&last), Ok(json!([[3, "t.bin", "AUDIO", most]])));
        let past = format!("{open}INDEX 01 {minutes}:{second}:{}\n", frame + 1);
        assert_eq!(
            outcome_of(&past),
            refused(
                "numberTooLarge",
                "CUE time in frames is larger than 18446744073709551615"
            )
        );
        // The largest track number that fits 32 bits is read, and one more is not.
        let last = "FILE \"t.bin\" BINARY\nTRACK 4294967295 AUDIO\nINDEX 01 0:0:0\n";
        assert_eq!(
            outcome_of(last),
            Ok(json!([[u32::MAX, "t.bin", "AUDIO", 0]]))
        );
        let past = last.replace("4294967295", "4294967296");
        assert_eq!(
            outcome_of(&past),
            refused(
                "numberTooLarge",
                "CUE track number is larger than 4294967295"
            )
        );
    }

    #[test]
    fn a_long_name_or_line_is_told_by_its_start_alone() {
        // A name as long as the most a refusal quotes is quoted whole.
        let whole = "a".repeat(MAX_QUOTED_CHARS);
        let said = format!("Unsafe disc name: {whole:?} (componentTooLong)");
        assert_eq!(outcome_of(&one_track(&whole)), refused("trackName", &said));
        // A longer one is told by its length and its start, counted in characters, however far it
        // runs on and however its characters are escaped.
        for (filler, len) in [
            ("a", MAX_QUOTED_CHARS + 1),
            ("\u{e9}", 300),
            ("\u{7f}", 60_000),
        ] {
            let name = filler.repeat(len);
            let start = filler.repeat(MAX_QUOTED_CHARS);
            let rule = if filler == "\u{7f}" {
                "control"
            } else {
                "componentTooLong"
            };
            let said = format!("Unsafe disc name of {len} characters beginning {start:?} ({rule})");
            let given = outcome_of(&one_track(&name));
            assert_eq!(given, refused("trackName", &said), "{len}");
            assert!(said.len() < 8 * MAX_QUOTED_CHARS, "{}", said.len());
        }

        // A line the reader does not read is quoted whole up to the same length.
        let line = "X".repeat(MAX_QUOTED_CHARS);
        let said = format!("Unsupported CUE line: {line}");
        assert_eq!(outcome_of(&line), refused("cueLine", &said));
        for (filler, len) in [("X", MAX_QUOTED_CHARS + 1), ("\u{1b}", 60_000)] {
            let line = filler.repeat(len);
            let start = filler.repeat(MAX_QUOTED_CHARS);
            let said = format!("Unsupported CUE line of {len} characters beginning {start:?}");
            assert_eq!(outcome_of(&line), refused("cueLine", &said), "{len}");
        }
    }

    #[test]
    fn a_sheet_is_read_from_its_text_alone() {
        // The reader takes the text without its byte order mark, as the sheet's reader gives it.
        let tracks = read(&one_track("t.bin")).unwrap();
        assert_eq!(listed(&tracks), json!([[3, "t.bin", "MODE1/2352", 0]]));
        assert_eq!(read("").unwrap_err().rule(), "incompleteSheet");
    }

    #[test]
    fn a_refused_sheet_still_names_its_files() {
        // Every FILE line's name, in the sheet's order, whatever the sheet breaks before or after
        // it: a track of a mode the reader refuses, a second track in a file used already, and a
        // line it does not read.
        let sheet = concat!(
            "REM ONE FILE\n",
            "FILE \"disc.bin\" BINARY\n",
            "TRACK 01 MODE1/2048\n",
            "INDEX 01 00:00:00\n",
            "  FILE \"Track (2).bin\" BINARY\r\n",
            "PREGAP 00:02:00\n",
            "FILE \"../outside.bin\" BINARY\n",
        );
        assert_eq!(read(sheet).unwrap_err().rule(), "trackMode");
        let named = ["disc.bin", "Track (2).bin", "../outside.bin"];
        assert_eq!(named_files(sheet), named);
        // A FILE line in another spelling names nothing, as the reader reads none.
        assert!(named_files("file \"disc.bin\" BINARY\nFILE \"a.bin\" WAVE\n").is_empty());
        assert!(named_files("").is_empty());
    }
}
