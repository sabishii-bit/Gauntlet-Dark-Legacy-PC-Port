//! A GDI, the plain list of a GD-ROM dump's track files: a line that counts its tracks, then a
//! line for each track in turn, of six fields apart by runs of spaces and tabs: the track's
//! number, the LBA of its first sector, its type, the size of its sectors, the name of its file
//! beside the GDI, quoted where it holds spaces, and an offset. A track's file starts with its
//! first sector. Only raw sectors are read, since only they keep the headers every sector is
//! placed by, and only an offset of 0, since the format's readers disagree on what any other
//! means.

use gdl_files::{TRACK_NAMES, check_component};

use super::{RAW_SECTOR, SheetTrack, TrackMode, is_number, quoted, too_large, unsafe_name};
use crate::{DiscRule, MediaError};

/// The most tracks a GDI may count: as many as a disc's table of contents can number.
const MAX_GDI_TRACKS: u32 = 99;

/// How many fields a track's line holds.
const FIELDS: usize = 6;

/// What ends a GDI's line.
const LINE_FEED: char = '\n';

/// What may stand once before a line's line feed, and is no part of the line.
const CARRIAGE_RETURN: char = '\r';

/// What a line's fields stand apart by, a run of them at a time, and what a line is trimmed of:
/// the space and the tab.
const BLANKS: [char; 2] = [' ', '\t'];

/// What a field that holds spaces is quoted between.
const QUOTE: char = '"';

/// The type a GDI gives an audio track.
const AUDIO_TYPE: u32 = 0;

/// The type a GDI gives a data track: the bit of a track's control field that a disc's table of
/// contents marks data with.
const DATA_TYPE: u32 = 4;

/// The one offset a GDI may give a track.
const NO_OFFSET: u32 = 0;

/// Where a GDI's track starts in its file: at its first sector, the one at the LBA the GDI gives.
const FIRST_SECTOR: u64 = 0;

/// What the reader answers a first line that gives no count of tracks with, before the line.
const UNSUPPORTED_COUNT: &str = "Unsupported GDI track count";

/// What the reader answers a track's line it does not read with, before the line.
const UNSUPPORTED_LINE: &str = "Unsupported GDI line";

/// What a track's LBA past the most it may be is called in its refusal.
const TRACK_LBA: &str = "GDI track LBA";

/// The tracks `text`, a GDI's text without its byte order mark, lists, in its order, each at the
/// LBA the GDI gives it, or the first rule of the GDI's grammar it breaks. Its lines end at line
/// feeds, one carriage return before each dropped, and are trimmed of spaces and tabs, and blank
/// lines are passed over. The first gives the count of tracks, which is judged first, and each
/// line after it is judged in turn before the count is held to the tracks listed.
pub(super) fn read(text: &str) -> Result<Vec<SheetTrack>, MediaError> {
    let mut lines = text
        .split(LINE_FEED)
        .map(|line| {
            let ended = line.strip_suffix(CARRIAGE_RETURN).unwrap_or(line);
            ended.trim_matches(BLANKS)
        })
        .filter(|line| !line.is_empty());
    let count = track_count(lines.next().unwrap_or_default())?;
    let mut tracks = Vec::new();
    let mut listed = 0;
    for line in lines {
        if listed == count {
            let detail = format!("GDI's track count is {count}, but it lists more");
            return Err(MediaError::disc(DiscRule::GdiCount, detail));
        }
        listed += 1;
        tracks.push(track(line, listed)?);
    }
    if listed < count {
        let detail = format!("GDI's track count is {count}, but it lists {listed}");
        return Err(MediaError::disc(DiscRule::GdiCount, detail));
    }
    Ok(tracks)
}

/// The count of tracks `line`, a GDI's first, gives: a number in ASCII digits from 1 to
/// [`MAX_GDI_TRACKS`].
fn track_count(line: &str) -> Result<u32, MediaError> {
    match line.parse() {
        Ok(count) if is_number(line) && (1..=MAX_GDI_TRACKS).contains(&count) => Ok(count),
        _ => Err(MediaError::disc(
            DiscRule::GdiCount,
            quoted(UNSUPPORTED_COUNT, line),
        )),
    }
}

/// The track `line`, the GDI's line for track `number`, lists. Its form is judged first: six
/// fields, all but the file's name numbers in ASCII digits. Then its fields are, in their order:
/// the track's own number, an LBA that fits 32 bits, the type of audio or data, raw sectors, a
/// name the folder beside the GDI may hold, and no offset.
fn track(line: &str, number: u32) -> Result<SheetTrack, MediaError> {
    let form = fields(line).filter(|&[given, lba, kind, size, _, offset]| {
        [given, lba, kind, size, offset].into_iter().all(is_number)
    });
    let Some([given, lba, kind, size, file, offset]) = form else {
        return Err(MediaError::disc(
            DiscRule::GdiLine,
            quoted(UNSUPPORTED_LINE, line),
        ));
    };
    if given.parse::<u32>().ok() != Some(number) {
        let detail = format!("GDI track {number} is missing or out of order");
        return Err(MediaError::disc(DiscRule::GdiTrackNumber, detail));
    }
    let lba: u32 = lba
        .parse()
        .map_err(|_| too_large(TRACK_LBA, u64::from(u32::MAX)))?;
    let mode = match kind.parse::<u32>() {
        Ok(AUDIO_TYPE) => TrackMode::Audio,
        Ok(DATA_TYPE) => TrackMode::Mode1Raw,
        _ => {
            let words = format!(
                "GDI track {number} is of a type other than audio ({AUDIO_TYPE}) and data \
                 ({DATA_TYPE})"
            );
            return Err(MediaError::disc(
                DiscRule::GdiTrackType,
                quoted(&words, kind),
            ));
        }
    };
    if size.parse::<usize>().ok() != Some(RAW_SECTOR) {
        let words = format!("GDI track {number} keeps sectors of other than {RAW_SECTOR} bytes");
        return Err(MediaError::disc(
            DiscRule::GdiSectorSize,
            quoted(&words, size),
        ));
    }
    check_component(file, &TRACK_NAMES)
        .map_err(|refused| unsafe_name(DiscRule::TrackName, file, refused.rule))?;
    if offset.parse::<u32>().ok() != Some(NO_OFFSET) {
        let words = format!("GDI track {number} gives an offset other than {NO_OFFSET}");
        return Err(MediaError::disc(
            DiscRule::GdiOffset,
            quoted(&words, offset),
        ));
    }
    Ok(SheetTrack {
        number,
        file: file.to_owned(),
        mode,
        index: FIRST_SECTOR,
        lba: Some(lba),
    })
}

/// The six fields of `line`, a GDI's line trimmed of blanks, when it holds exactly six. None for
/// another count, or for a quote left open or closed within a field.
fn fields(line: &str) -> Option<[&str; FIELDS]> {
    let mut fields = [""; FIELDS];
    let mut rest = line;
    for field in &mut fields {
        (*field, rest) = next_field(rest)?;
    }
    rest.is_empty().then_some(fields)
}

/// The field `rest`, a line from a field's start, begins with, and the line from the next field's
/// start: the characters up to the next blank, or for a field that begins with a quote, those
/// between it and the next quote, which a blank or the line's end must follow. None at the line's
/// end, or for a quote left open or closed within a field.
fn next_field(rest: &str) -> Option<(&str, &str)> {
    if rest.is_empty() {
        return None;
    }
    let (field, after) = match rest.strip_prefix(QUOTE) {
        Some(opened) => opened
            .split_once(QUOTE)
            .filter(|(_, after)| after.is_empty() || after.starts_with(BLANKS))?,
        None => rest.split_once(BLANKS).unwrap_or((rest, "")),
    };
    Some((field, after.trim_start_matches(BLANKS)))
}

#[cfg(test)]
mod tests {
    use serde_json::{Value, json};

    use super::{MAX_GDI_TRACKS, read};
    use crate::gdrom::MAX_QUOTED_CHARS;
    use crate::vectors::{dreamcast_vectors, placed, text};
    use crate::{MediaError, RAW_SECTOR, SECTOR, Sha1Digest, SheetKind, parse_sheet};

    /// The GDI of the US disc's TOSEC set, byte for byte: five tracks, data in the low-density
    /// area, audio after it, the high-density area's data from LBA 45000, audio, and the last
    /// data track from its INDEX 01, with CRLF line ends.
    const TOSEC_GDI: &str = concat!(
        "5\r\n",
        "1 0 4 2352 track01.bin 0\r\n",
        "2 1000 0 2352 track02.raw 0\r\n",
        "3 45000 4 2352 track03.bin 0\r\n",
        "4 252586 0 2352 track04.raw 0\r\n",
        "5 253187 4 2352 track05.bin 0\r\n",
    );

    /// The length the TOSEC catalogue gives that GDI.
    const TOSEC_GDI_LEN: usize = 150;

    /// The SHA-1 the TOSEC catalogue gives that GDI.
    const TOSEC_GDI_SHA1: &str = "7469884452f804fad0952ecadddbab779bfd4146";

    /// What the reader gives a GDI's text: its tracks as the vectors list a sheet's, each with the
    /// LBA the GDI gives it, or the rule and the words it refuses the text with.
    type Outcome = Result<Value, (&'static str, String)>;

    /// What the GDI's grammar gives `text`: by a rule of the disc's format when it refuses it,
    /// never by a failure to read.
    fn outcome(text: &str) -> Outcome {
        match read(text) {
            Ok(tracks) => Ok(placed(&tracks)),
            Err(error) => {
                assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
                Err((error.rule(), error.to_string()))
            }
        }
    }

    /// The refusal by `rule` with `words`.
    fn refused(rule: &'static str, words: &str) -> Outcome {
        Err((rule, words.to_owned()))
    }

    /// A GDI of one track, whose line is `line`.
    fn one_track(line: &str) -> String {
        format!("1\n{line}\n")
    }

    /// What the reader answers a track's line it does not read.
    fn unread(line: &str) -> Outcome {
        refused("gdiLine", &format!("Unsupported GDI line: {line:?}"))
    }

    #[test]
    fn the_tosec_layout_reads() {
        // The sheet of the TOSEC set, as its catalogue gives it.
        assert_eq!(TOSEC_GDI.len(), TOSEC_GDI_LEN);
        assert_eq!(Sha1Digest::of(TOSEC_GDI.as_bytes()).hex(), TOSEC_GDI_SHA1);
        // Each track in a file of its own, from the file's first sector, at the LBA the GDI gives.
        let tracks = json!([
            [1, "track01.bin", "MODE1/2352", 0, 0],
            [2, "track02.raw", "AUDIO", 0, 1000],
            [3, "track03.bin", "MODE1/2352", 0, 45000],
            [4, "track04.raw", "AUDIO", 0, 252586],
            [5, "track05.bin", "MODE1/2352", 0, 253187]
        ]);
        let (kind, read_tracks) = parse_sheet(TOSEC_GDI.as_bytes()).unwrap();
        assert_eq!(kind, SheetKind::Gdi);
        assert_eq!(placed(&read_tracks), tracks);
        // With line feeds alone, it lists the same.
        assert_eq!(outcome(&TOSEC_GDI.replace("\r\n", "\n")), Ok(tracks));
    }

    #[test]
    fn quoted_names_and_crlf_read() {
        // Names quoted where they hold spaces, and where they need not be, fields apart by runs of
        // spaces and tabs, lines ended by CRLF or a line feed alone, a last line with no end, and
        // blank lines and the blanks at either end of a line passed over.
        let gdi = concat!(
            "\r\n",
            " \t3\t \r\n",
            "\r\n",
            "1 0 4 2352 \"Track 01 (data).bin\" 0\r\n",
            " \t \n",
            "\t2  600\t\t0 2352 \"trak\u{e9} 2.raw\"   0 \n",
            "3 45000 4 2352 \"track03.bin\" 0",
        );
        assert_eq!(
            outcome(gdi),
            Ok(json!([
                [1, "Track 01 (data).bin", "MODE1/2352", 0, 0],
                [2, "trak\u{e9} 2.raw", "AUDIO", 0, 600],
                [3, "track03.bin", "MODE1/2352", 0, 45000]
            ]))
        );
        // A quoted field is the text between its quotes, spaces at its ends and all, and any field
        // may be quoted.
        let quoted = one_track("\"1\" \"45000\" \"4\" \"2352\" \" a b.bin\" \"0\"");
        assert_eq!(
            outcome(&quoted),
            Ok(json!([[1, " a b.bin", "MODE1/2352", 0, 45000]]))
        );
        // A quote that is left open, or that closes where its field goes on, ends no field.
        for line in [
            "1 45000 4 2352 \"t.bin 0",
            "1 45000 4 2352 \"t.bin\"0",
            "1 45000 4 2352 \"t\"\".bin\" 0",
            "1 45000 4 2352 t.bin \"0",
            "\"1 45000 4 2352 t.bin 0",
        ] {
            assert_eq!(outcome(&one_track(line)), unread(line), "{line}");
        }
        // A field that does not begin with a quote runs to the next blank, quotes and all: no name
        // may hold one, and no number.
        let line = "1 45000 4 2352 t\"x\".bin 0";
        let said = format!("Unsafe disc name: {:?} (forbidden)", "t\"x\".bin");
        assert_eq!(outcome(&one_track(line)), refused("trackName", &said));
        let line = "1 45000 4 2352 t.bin 0\"";
        assert_eq!(outcome(&one_track(line)), unread(line));

        // Only one carriage return before a line feed is dropped: another is part of the line.
        let line = "1 45000 4 2352 t.bin 0\r";
        assert_eq!(outcome(&format!("1\n{line}\r\n")), unread(line));
        // And a lone carriage return ends no line, so the count's line runs on past it.
        let first = "1\r1 45000 4 2352 t.bin 0";
        let said = format!("Unsupported GDI track count: {first:?}");
        assert_eq!(outcome(&format!("{first}\r\n")), refused("gdiCount", &said));
    }

    #[test]
    fn cooked_tracks_and_offsets_are_unsupported() {
        // Sectors of any size but a raw sector's: cooked ones, which keep no header, mode 2's
        // user data, raw ones with their subcode after them, and the rest, in a data track or an
        // audio one.
        let cooked = SECTOR.to_string();
        let sizes = [
            cooked.as_str(),
            "2336",
            "2448",
            "0",
            "2351",
            "2353",
            "99999999999999999999",
        ];
        for size in sizes {
            for (kind, lba) in [(4, 45000), (0, 600)] {
                let gdi = one_track(&format!("1 {lba} {kind} {size} t.bin 0"));
                let words = format!("GDI track 1 keeps sectors of other than 2352 bytes: {size:?}");
                assert_eq!(
                    outcome(&gdi),
                    refused("gdiSectorSize", &words),
                    "{size} {kind}"
                );
            }
        }
        // Any offset but none.
        for offset in ["4", "1", "150", "2352", "99999999999999999999"] {
            let gdi = one_track(&format!("1 45000 4 2352 t.bin {offset}"));
            let words = format!("GDI track 1 gives an offset other than 0: {offset:?}");
            assert_eq!(outcome(&gdi), refused("gdiOffset", &words), "{offset}");
        }
        // Noughts before a number change nothing: a raw sector's size and no offset are read
        // however many lead them, as the count and every number of a line are.
        let zeros = "0".repeat(40);
        let gdi = format!("{zeros}1\n{zeros}1 {zeros}45000 {zeros}4 {zeros}2352 t.bin {zeros}\n");
        assert_eq!(
            outcome(&gdi),
            Ok(json!([[1, "t.bin", "MODE1/2352", 0, 45000]]))
        );
        assert_eq!(RAW_SECTOR, 2352);
    }

    #[test]
    fn malformed_lines_are_refused() {
        // A line of five fields, or of seven.
        for line in [
            "1 45000 4 2352 t.bin",
            "1 45000 4 2352 t.bin 0 0",
            "1 45000 4 2352 \"t .bin\"",
            "1 45000 4 2352 t .bin 0",
            "1",
        ] {
            assert_eq!(outcome(&one_track(line)), unread(line), "{line}");
        }
        // A field that should be a number, in anything but ASCII digits: signed, with a point, in
        // another base, in another script's digits, or quoted and empty.
        let numbers = [
            "+1", "-1", "1.0", "0x1", "1e3", "\u{661}", "\u{ff11}", "\"\"", "1a",
        ];
        let fields = ["1", "45000", "4", "2352", "t.bin", "0"];
        for number in numbers {
            for at in [0, 1, 2, 3, 5] {
                let mut line = fields;
                line[at] = number;
                let line = line.join(" ");
                assert_eq!(outcome(&one_track(&line)), unread(&line), "{line}");
            }
        }

        // A first line that counts no track, more than a disc numbers, or is no number in ASCII
        // digits, and a GDI of no line at all.
        let track = "1 45000 4 2352 t.bin 0";
        for count in [
            "0",
            "100",
            "4294967296",
            "x",
            "3 tracks",
            "+1",
            "\u{661}",
            "1.0",
        ] {
            let said = format!("Unsupported GDI track count: {count:?}");
            let gdi = format!("{count}\n{track}\n");
            assert_eq!(outcome(&gdi), refused("gdiCount", &said), "{count}");
        }
        for gdi in ["", "\n", " \t\r\n\n"] {
            let said = "Unsupported GDI track count: \"\"";
            assert_eq!(outcome(gdi), refused("gdiCount", said), "{gdi:?}");
        }
        // A count that differs from the tracks listed, fewer of them or more.
        let two = "1 0 4 2352 a.bin 0\n2 600 0 2352 b.raw 0\n";
        let cases = [
            ("3\n".to_owned(), "GDI's track count is 3, but it lists 0"),
            (
                "3\n1 0 4 2352 a.bin 0\n".to_owned(),
                "GDI's track count is 3, but it lists 1",
            ),
            (
                format!("3\n{two}"),
                "GDI's track count is 3, but it lists 2",
            ),
            (
                format!("1\n{two}"),
                "GDI's track count is 1, but it lists more",
            ),
        ];
        for (gdi, said) in cases {
            assert_eq!(outcome(&gdi), refused("gdiCount", said), "{gdi:?}");
        }

        // Numbers that do not run from 1 a step at a time: one skipped, one repeated, from 0 or 2,
        // and one past 32 bits.
        let cases = [
            ("2\n1 0 4 2352 a.bin 0\n3 600 0 2352 b.raw 0\n", 2),
            ("2\n1 0 4 2352 a.bin 0\n1 600 0 2352 b.raw 0\n", 2),
            ("1\n0 0 4 2352 a.bin 0\n", 1),
            ("1\n2 0 4 2352 a.bin 0\n", 1),
            ("1\n4294967297 0 4 2352 a.bin 0\n", 1),
        ];
        for (gdi, number) in cases {
            let said = format!("GDI track {number} is missing or out of order");
            assert_eq!(outcome(gdi), refused("gdiTrackNumber", &said), "{gdi:?}");
        }

        // A type other than audio's and data's, such as the other bits of a track's control field.
        for kind in [
            "1",
            "2",
            "3",
            "5",
            "6",
            "7",
            "8",
            "12",
            "40",
            "99999999999999999999",
        ] {
            let gdi = one_track(&format!("1 45000 {kind} 2352 t.bin 0"));
            let said =
                format!("GDI track 1 is of a type other than audio (0) and data (4): {kind:?}");
            assert_eq!(outcome(&gdi), refused("gdiTrackType", &said), "{kind}");
        }
    }

    #[test]
    fn lbas_are_kept_in_32_bits() {
        // The largest LBA 32 bits keep is read, and one more is not.
        let last = format!("1 {} 4 2352 t.bin 0", u32::MAX);
        assert_eq!(
            outcome(&one_track(&last)),
            Ok(json!([[1, "t.bin", "MODE1/2352", 0, u32::MAX]]))
        );
        let past = format!("1 {} 4 2352 t.bin 0", u64::from(u32::MAX) + 1);
        let said = "GDI track LBA is larger than 4294967295";
        assert_eq!(outcome(&one_track(&past)), refused("numberTooLarge", said));
    }

    #[test]
    fn a_gdi_counts_from_1_to_99_tracks() {
        // As many tracks as a disc's table of contents numbers, the count written with noughts
        // before it too.
        let lines: String = (1..=MAX_GDI_TRACKS)
            .map(|number| format!("{number} {} 0 2352 t{number}.raw 0\n", 1000 * number))
            .collect();
        for count in ["99", "099"] {
            let tracks = read(&format!("{count}\n{lines}")).unwrap();
            assert_eq!(tracks.len(), 99, "{count}");
            for (track, number) in tracks.iter().zip(1..) {
                assert_eq!(track.number, number);
                assert_eq!(track.file, format!("t{number}.raw"));
                assert_eq!(track.lba, Some(1000 * number));
            }
        }
        assert_eq!(MAX_GDI_TRACKS, 99);
    }

    #[test]
    fn track_names_follow_the_shared_vectors() {
        // A GDI's track names are held to the rules a cue sheet's are: each name of the shared
        // vectors, quoted, is taken or refused as the cue sheet's reader takes or refuses it, by
        // the same rule of names.
        let vectors = dreamcast_vectors();
        let cases = vectors["names"].as_array().unwrap();
        assert!(!cases.is_empty());
        let mut empty = 0;
        for case in cases {
            let name = text(&case["name"]);
            let track = &case["track"];
            let gdi = one_track(&format!("1 45000 4 2352 \"{name}\" 0"));
            match (outcome(&gdi), track["nameRule"].as_str()) {
                (Ok(tracks), None) => {
                    assert!(track["rule"].is_null(), "{case}");
                    assert_eq!(tracks, json!([[1, name, "MODE1/2352", 0, 45000]]), "{case}");
                }
                (Err((rule, detail)), Some(name_rule)) => {
                    assert_eq!(rule, text(&track["rule"]), "{case}");
                    let said = format!("Unsafe disc name: {name:?} ({name_rule})");
                    assert_eq!(detail, said, "{case}");
                }
                // The one name a cue sheet's FILE line cannot hold: the empty name, which a GDI
                // holds between its quotes and refuses by the rules of names.
                (Err((rule, detail)), None) => {
                    assert!(name.is_empty(), "{case}");
                    assert_eq!(text(&track["rule"]), "cueLine", "{case}");
                    assert_eq!(rule, "trackName");
                    assert_eq!(detail, "Unsafe disc name: \"\" (empty)");
                    empty += 1;
                }
                (given, _) => panic!("{case}: {given:?}"),
            }
            // Unquoted, a name without blanks is the same field.
            if !name.contains([' ', '\t']) && !name.is_empty() {
                let bare = one_track(&format!("1 45000 4 2352 {name} 0"));
                assert_eq!(outcome(&bare), outcome(&gdi), "{case}");
            }
        }
        assert_eq!(empty, 1);
    }

    #[test]
    fn each_line_is_judged_in_a_fixed_order() {
        // Each GDI breaks two rules, and the one judged first refuses it: the count's line before
        // any track's, a line past the count before its own form, a line's form before its
        // fields, its fields in their order, and an earlier line before a later one, which comes
        // before the count is held to the tracks listed.
        let cases = [
            ("0\nx\n", "gdiCount"),
            ("1\n1 0 4 2352 a.bin 0\nx\n", "gdiCount"),
            ("1\n2 45000 4 2352 t.bin x\n", "gdiLine"),
            ("1\n2 45000 4 2352 t.bin 0 0\n", "gdiLine"),
            ("1\n2 4294967296 4 2352 t.bin 0\n", "gdiTrackNumber"),
            ("1\n1 4294967296 1 2352 t.bin 0\n", "numberTooLarge"),
            ("1\n1 45000 1 2048 t.bin 0\n", "gdiTrackType"),
            ("1\n1 45000 4 2048 ../t.bin 0\n", "gdiSectorSize"),
            ("1\n1 45000 4 2352 ../t.bin 4\n", "trackName"),
            (
                "2\n1 0 4 2048 a.bin 0\n3 600 1 2352 b.raw 4\n",
                "gdiSectorSize",
            ),
            ("3\n1 0 4 2352 a.bin 0\nx\n", "gdiLine"),
        ];
        for (gdi, rule) in cases {
            let given = outcome(gdi);
            assert!(
                matches!(&given, Err((found, _)) if *found == rule),
                "{gdi:?}: {given:?}"
            );
        }
    }

    #[test]
    fn a_long_line_or_field_is_told_by_its_start_alone() {
        // A line as long as the most a refusal quotes is quoted whole.
        let line = "x".repeat(MAX_QUOTED_CHARS);
        assert_eq!(outcome(&one_track(&line)), unread(&line));
        // A longer one is told by its length and its start, counted in characters, however far it
        // runs on and however its characters are escaped.
        for (filler, len) in [
            ("x", MAX_QUOTED_CHARS + 1),
            ("\u{e9}", 300),
            ("\u{1b}", 60_000),
        ] {
            let line = filler.repeat(len);
            let start = filler.repeat(MAX_QUOTED_CHARS);
            let said = format!("Unsupported GDI line of {len} characters beginning {start:?}");
            assert_eq!(
                outcome(&one_track(&line)),
                refused("gdiLine", &said),
                "{len}"
            );
            assert!(said.len() < 8 * MAX_QUOTED_CHARS, "{}", said.len());
        }
        // So are a count, a field and a name.
        let count = "9".repeat(300);
        let start = "9".repeat(MAX_QUOTED_CHARS);
        let said = format!("Unsupported GDI track count of 300 characters beginning {start:?}");
        assert_eq!(outcome(&format!("{count}\n")), refused("gdiCount", &said));
        let kind = "1".repeat(300);
        let start = "1".repeat(MAX_QUOTED_CHARS);
        let said = format!(
            "GDI track 1 is of a type other than audio (0) and data (4) of 300 characters \
             beginning {start:?}"
        );
        let gdi = one_track(&format!("1 45000 {kind} 2352 t.bin 0"));
        assert_eq!(outcome(&gdi), refused("gdiTrackType", &said));
        let name = "a".repeat(300);
        let start = "a".repeat(MAX_QUOTED_CHARS);
        let said =
            format!("Unsafe disc name of 300 characters beginning {start:?} (componentTooLong)");
        let gdi = one_track(&format!("1 45000 4 2352 {name} 0"));
        assert_eq!(outcome(&gdi), refused("trackName", &said));
    }

    #[test]
    fn a_gdi_is_read_from_its_text_alone() {
        // The reader takes the text without its byte order mark, as the sheet's reader gives it:
        // a mark is part of the count's line.
        let gdi = "1\n1 45000 4 2352 t.bin 0\n";
        assert_eq!(
            outcome(gdi),
            Ok(json!([[1, "t.bin", "MODE1/2352", 0, 45000]]))
        );
        let marked = format!("\u{feff}{gdi}");
        let said = format!("Unsupported GDI track count: {:?}", "\u{feff}1");
        assert_eq!(outcome(&marked), refused("gdiCount", &said));
    }
}
