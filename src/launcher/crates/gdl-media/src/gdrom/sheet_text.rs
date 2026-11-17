//! The text of a sheet that lists a dump's track files, cut into lines and stripped as the
//! Dreamcast extractor cuts and strips it, and the text fields the disc's boot area and volume
//! descriptor keep, stripped the same way. The extractor ends a line at more characters than a
//! line feed and a carriage return, and strips more than Unicode's white space, so neither std's
//! lines nor its trim gives its text.

use std::{iter, mem};

/// The characters the Dreamcast extractor strips from both ends of a sheet's line and of a text
/// field: Unicode's white space, and the four ASCII information separators from U+001C to U+001F.
const STRIPPED: [char; 29] = [
    '\t', '\n', '\u{b}', '\u{c}', '\r', '\u{1c}', '\u{1d}', '\u{1e}', '\u{1f}', ' ', '\u{85}',
    '\u{a0}', '\u{1680}', '\u{2000}', '\u{2001}', '\u{2002}', '\u{2003}', '\u{2004}', '\u{2005}',
    '\u{2006}', '\u{2007}', '\u{2008}', '\u{2009}', '\u{200a}', '\u{2028}', '\u{2029}', '\u{202f}',
    '\u{205f}', '\u{3000}',
];

/// The characters it ends a sheet's lines at: the line feed, the carriage return, the vertical tab
/// and the form feed, the first three information separators, the next line control, and
/// Unicode's line and paragraph separators. The unit separator U+001F is stripped but ends no
/// line.
const LINE_BREAKS: [char; 10] = [
    '\n', '\r', '\u{b}', '\u{c}', '\u{1c}', '\u{1d}', '\u{1e}', '\u{85}', '\u{2028}', '\u{2029}',
];

/// A carriage return and the line feed after it, which end one line together.
const CRLF: &str = "\r\n";

/// `text` without the white space the Dreamcast extractor strips from either end of a sheet's line
/// and of a text field. What lies within it is kept.
pub fn strip(text: &str) -> &str {
    text.trim_matches(STRIPPED)
}

/// The lines of `text`, each without the break that ends it, ended where the Dreamcast extractor
/// ends them: at any of its line breaks, a carriage return followed by a line feed ending one line.
/// A break at the end of the text ends the last line and begins none, so an empty text has no
/// lines, and a text that is one break has one empty line.
pub fn lines(text: &str) -> impl Iterator<Item = &str> {
    let mut rest = text;
    iter::from_fn(move || {
        if rest.is_empty() {
            return None;
        }
        let Some(at) = rest.find(LINE_BREAKS) else {
            return Some(mem::take(&mut rest));
        };
        let (line, after) = rest.split_at(at);
        rest = after.strip_prefix(CRLF).unwrap_or_else(|| {
            let mut broken = after.chars();
            broken.next();
            broken.as_str()
        });
        Some(line)
    })
}

/// A text field of the disc's boot area or volume descriptor, read as the Dreamcast extractor reads
/// one: ASCII, without the white space at either end, and with everything within it kept, NULs
/// included. None when a byte of it lies outside ASCII.
pub fn ascii_field(bytes: &[u8]) -> Option<String> {
    let text = str::from_utf8(bytes).ok().filter(|text| text.is_ascii())?;
    Some(strip(text).to_owned())
}

#[cfg(test)]
mod tests {
    use std::ops::RangeInclusive;

    use super::{ascii_field, lines, strip};

    /// The characters the format strips from either end of a line and of a field, as the ranges
    /// the extractor's white space covers. They are given here apart from the reader's own list,
    /// so that a reader that strips another set is caught.
    const FORMAT_STRIPPED: [RangeInclusive<char>; 10] = [
        '\u{9}'..='\u{d}',
        '\u{1c}'..='\u{20}',
        '\u{85}'..='\u{85}',
        '\u{a0}'..='\u{a0}',
        '\u{1680}'..='\u{1680}',
        '\u{2000}'..='\u{200a}',
        '\u{2028}'..='\u{2029}',
        '\u{202f}'..='\u{202f}',
        '\u{205f}'..='\u{205f}',
        '\u{3000}'..='\u{3000}',
    ];

    /// The characters the format ends a line at, as ranges.
    const FORMAT_BREAKS: [RangeInclusive<char>; 4] = [
        '\u{a}'..='\u{d}',
        '\u{1c}'..='\u{1e}',
        '\u{85}'..='\u{85}',
        '\u{2028}'..='\u{2029}',
    ];

    /// The ASCII information separators, which the extractor strips beside Unicode's white space.
    const SEPARATORS: RangeInclusive<char> = '\u{1c}'..='\u{1f}';

    /// Whether a character lies in one of `ranges`.
    fn within(ranges: &[RangeInclusive<char>], character: char) -> bool {
        ranges.iter().any(|range| range.contains(&character))
    }

    /// Every character there is: every code point but the surrogates'.
    fn every_char() -> impl Iterator<Item = char> {
        (0..=u32::from(char::MAX)).filter_map(char::from_u32)
    }

    #[test]
    fn strip_takes_the_scripts_whitespace() {
        // Every character alone: stripped away when it is the extractor's white space, which is
        // Unicode's and the four separators, and kept otherwise.
        let mut buffer = [0; 4];
        let mut stripped = 0;
        for character in every_char() {
            let white = within(&FORMAT_STRIPPED, character);
            assert_eq!(
                white,
                character.is_whitespace() || SEPARATORS.contains(&character),
                "{character:?}"
            );
            let alone: &str = character.encode_utf8(&mut buffer);
            let kept = if white { "" } else { alone };
            assert_eq!(strip(alone), kept, "{character:?}");
            stripped += usize::from(white);
        }
        assert_eq!(stripped, 29);

        // Each is stripped from either end, however many stand there and in whatever mix, and kept
        // within the text.
        let all: String = every_char()
            .filter(|&character| within(&FORMAT_STRIPPED, character))
            .collect();
        for white in all.chars() {
            assert_eq!(strip(&format!("{white}a{white}")), "a", "{white:?}");
            let within_text = format!("a{white}b");
            assert_eq!(strip(&format!("{all}{within_text}{all}")), within_text);
        }
        // NUL, letters, the zero width space, the byte order mark, the word joiner and the
        // Mongolian vowel separator, which is no white space either, are none of it.
        for kept in [
            "\0", "a", "\0a\0", "\u{200b}", "\u{feff}", "\u{2060}", "\u{180e}",
        ] {
            assert_eq!(strip(kept), kept, "{kept:?}");
        }
        assert_eq!(strip(""), "");
    }

    #[test]
    fn lines_break_where_the_script_breaks() {
        // Every character between two letters: a break ends the first line and begins the second,
        // and any other character is part of the one line.
        let mut text = String::new();
        let mut breaks = 0;
        for character in every_char() {
            text.clear();
            text.extend(['a', character, 'b']);
            if within(&FORMAT_BREAKS, character) {
                assert!(lines(&text).eq(["a", "b"]), "{character:?}");
                breaks += 1;
            } else {
                assert!(lines(&text).eq([text.as_str()]), "{character:?}");
            }
        }
        assert_eq!(breaks, 10);

        let cases: [(&str, &[&str]); 16] = [
            // A carriage return and a line feed end one line together, but a line feed and a
            // carriage return two, as two carriage returns do.
            ("a\r\nb", &["a", "b"]),
            ("a\n\rb", &["a", "", "b"]),
            ("a\r\r\nb", &["a", "", "b"]),
            ("a\r\n\r\nb", &["a", "", "b"]),
            ("a\rb\r", &["a", "b"]),
            // A break at the end ends the last line and begins none.
            ("", &[]),
            ("\n", &[""]),
            ("\r\n", &[""]),
            ("a\n", &["a"]),
            ("a\u{2029}", &["a"]),
            ("a\n\n", &["a", ""]),
            ("\n\na", &["", "", "a"]),
            // The unit separator is stripped from a line's ends but ends no line.
            ("a\u{1f}b\n", &["a\u{1f}b"]),
            ("\u{1f}\u{1f}", &["\u{1f}\u{1f}"]),
            // A line keeps its white space, which stripping takes.
            (" a \n\tb\u{a0}", &[" a ", "\tb\u{a0}"]),
            ("\u{1c}\u{1d}\u{1e}", &["", "", ""]),
        ];
        for (text, expected) in cases {
            let found: Vec<&str> = lines(text).collect();
            assert_eq!(found, expected, "{text:?}");
        }
    }

    #[test]
    fn ascii_fields_strip_as_the_script_strips() {
        // A field padded with spaces, or with the control characters and separators the extractor
        // strips, comes back without them.
        let fields: [(&[u8], &str); 7] = [
            (b"GAUNT1                          ", "GAUNT1"),
            (b"\t\n\x0b\x0c\r\x1c\x1d\x1e\x1f SEGA \x1f\x1c", "SEGA"),
            // What lies within it is kept, spaces and NULs among it, and so are NULs at its ends,
            // which are not white space.
            (b"  GAUNTLET LEGENDS\0\0\0", "GAUNTLET LEGENDS\0\0\0"),
            (b"\0 T9710N \0", "\0 T9710N \0"),
            (b"1ST_READ.BIN    ", "1ST_READ.BIN"),
            (b"", ""),
            (b" \t \x1f ", ""),
        ];
        for (bytes, field) in fields {
            assert_eq!(ascii_field(bytes).as_deref(), Some(field), "{bytes:?}");
        }
        // A title of 16 letters and 112 NULs keeps every NUL.
        let mut title = b"GAUNTLET LEGENDS".to_vec();
        title.resize(128, 0);
        let read = ascii_field(&title).unwrap();
        assert_eq!(read.len(), 128);
        assert!(read.ends_with(&"\0".repeat(112)));

        // Every ASCII byte is taken, and the field is the text stripped.
        let every: Vec<u8> = (0..0x80).collect();
        let text = String::from_utf8(every.clone()).unwrap();
        assert_eq!(ascii_field(&every).as_deref(), Some(strip(&text)));
        // Every byte outside ASCII refuses the field, wherever it stands: even those whose
        // character in Latin-1 is white space, the next line control and the no-break space.
        for byte in 0x80..=0xFF {
            for at in 0..3 {
                let mut field = *b"V1.003";
                field[at * 2] = byte;
                assert_eq!(ascii_field(&field), None, "{byte:#04x} at {at}");
            }
            assert_eq!(ascii_field(&[byte]), None, "{byte:#04x}");
        }
    }
}
