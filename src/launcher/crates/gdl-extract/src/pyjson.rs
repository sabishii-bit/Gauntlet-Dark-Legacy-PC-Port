//! JSON as the extraction scripts write their manifests: each member of an array or an object on a
//! line of its own, two spaces deeper for each container it lies in, members in the order the code
//! gives them, and every character outside printable ASCII escaped, so that a manifest the launcher
//! writes is byte for byte the scripts' own. Beside it, the values every manifest is made of.

use gdl_media::Sha256Digest;

/// What a member of an array or an object is indented by, for each container it lies in.
pub const INDENT: &str = "  ";

/// What opens and closes an array.
const ARRAY_BRACKETS: [u8; 2] = *b"[]";

/// What opens and closes an object.
const OBJECT_BRACKETS: [u8; 2] = *b"{}";

/// What follows each member of an array or an object but its last.
const MEMBER_SEPARATOR: u8 = b',';

/// What lies between an object's key and its value.
const KEY_SEPARATOR: &[u8] = b": ";

/// The first of the characters a string holds as themselves, the space.
const FIRST_PLAIN: u8 = b' ';

/// The last of the characters a string holds as themselves, the tilde. Between the two lie the
/// printable ASCII characters, of which only the quote and the backslash are escaped.
const LAST_PLAIN: u8 = b'~';

/// The backspace, which has an escape of its own.
const BACKSPACE: u8 = 0x08;

/// The form feed, which has an escape of its own.
const FORM_FEED: u8 = 0x0C;

/// A value of a manifest, of the kinds the extraction scripts' manifests hold. There is no float:
/// no manifest holds one, so none can be written.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum JsonValue {
    /// A whole number that may be negative.
    Int(i64),
    /// A whole number that is never negative.
    Uint(u64),
    /// `true` or `false`.
    Bool(bool),
    /// Text.
    Str(String),
    /// Text given by its UTF-16 code units, as a dump's name is kept, which may hold an unpaired
    /// surrogate: it is written a unit at a time, as text is.
    Units(Vec<u16>),
    /// Values in their order.
    Array(Vec<JsonValue>),
    /// Members in the order the extraction scripts write them, each key once.
    Object(Vec<(&'static str, JsonValue)>),
}

/// The document `value` makes, as the extraction scripts write a manifest: an array's or an
/// object's members each on a line of its own, one [`INDENT`] deeper than the line the container
/// opens on, a comma after each but the last, and the closing bracket on a line of its own at the
/// container's depth; an empty array or object as `[]` or `{}`; a key followed by a colon and a
/// space; numbers in decimal; and a newline after the whole. Every byte of it is ASCII.
pub fn render(value: &JsonValue) -> Vec<u8> {
    let mut document = Vec::new();
    write_value(&mut document, value, 0);
    document.push(b'\n');
    document
}

/// Text, as a manifest holds it.
pub(crate) fn text(value: &str) -> JsonValue {
    JsonValue::Str(value.to_owned())
}

/// A digest, as a manifest holds it: in lowercase hexadecimal.
pub(crate) fn digest(value: &Sha256Digest) -> JsonValue {
    JsonValue::Str(value.hex())
}

/// Rows, as a manifest holds them: an array of what `each` makes of every row, in order.
pub(crate) fn array<T>(rows: &[T], each: fn(&T) -> JsonValue) -> JsonValue {
    JsonValue::Array(rows.iter().map(each).collect())
}

/// Writes `value` where it stands in a document: on a line `depth` containers deep.
fn write_value(out: &mut Vec<u8>, value: &JsonValue, depth: usize) {
    match value {
        JsonValue::Int(number) => out.extend_from_slice(number.to_string().as_bytes()),
        JsonValue::Uint(number) => out.extend_from_slice(number.to_string().as_bytes()),
        JsonValue::Bool(true) => out.extend_from_slice(b"true"),
        JsonValue::Bool(false) => out.extend_from_slice(b"false"),
        JsonValue::Str(text) => write_string(out, text.encode_utf16()),
        JsonValue::Units(units) => write_string(out, units.iter().copied()),
        JsonValue::Array(items) => write_container(out, ARRAY_BRACKETS, items, depth, write_value),
        JsonValue::Object(members) => {
            write_container(out, OBJECT_BRACKETS, members, depth, write_member);
        }
    }
}

/// Writes an object's member where it stands, on a line `depth` containers deep: its key, then
/// its value.
fn write_member(out: &mut Vec<u8>, member: &(&'static str, JsonValue), depth: usize) {
    let (key, value) = member;
    write_string(out, key.encode_utf16());
    out.extend_from_slice(KEY_SEPARATOR);
    write_value(out, value, depth);
}

/// Writes a container that opens on a line `depth` containers deep, between its two `brackets`:
/// each of its `members`, by `write_each`, on a line of its own one container deeper, or nothing
/// between the brackets when it has none.
fn write_container<T>(
    out: &mut Vec<u8>,
    brackets: [u8; 2],
    members: &[T],
    depth: usize,
    write_each: fn(&mut Vec<u8>, &T, usize),
) {
    let [open, close] = brackets;
    out.push(open);
    if !members.is_empty() {
        for (index, member) in members.iter().enumerate() {
            if index > 0 {
                out.push(MEMBER_SEPARATOR);
            }
            new_line(out, depth + 1);
            write_each(out, member, depth + 1);
        }
        new_line(out, depth);
    }
    out.push(close);
}

/// Ends the line and indents the next for `depth` containers.
fn new_line(out: &mut Vec<u8>, depth: usize) {
    out.push(b'\n');
    for _ in 0..depth {
        out.extend_from_slice(INDENT.as_bytes());
    }
}

/// Writes text, given by its UTF-16 code units, as a quoted string, each unit by itself whatever
/// its neighbours: the printable ASCII characters as themselves but for the quote and the
/// backslash; those two, the line feed, the carriage return, the tab, the backspace and the form
/// feed by their short escapes; and every other unit as `\u` and four lowercase hexadecimal
/// digits. So an unpaired surrogate is kept as it is, and a character beyond the Basic
/// Multilingual Plane is written as its surrogate pair.
fn write_string(out: &mut Vec<u8>, units: impl IntoIterator<Item = u16>) {
    out.push(b'"');
    for unit in units {
        match u8::try_from(unit) {
            Ok(b'"') => out.extend_from_slice(br#"\""#),
            Ok(b'\\') => out.extend_from_slice(br"\\"),
            Ok(b'\n') => out.extend_from_slice(br"\n"),
            Ok(b'\r') => out.extend_from_slice(br"\r"),
            Ok(b'\t') => out.extend_from_slice(br"\t"),
            Ok(BACKSPACE) => out.extend_from_slice(br"\b"),
            Ok(FORM_FEED) => out.extend_from_slice(br"\f"),
            Ok(plain @ FIRST_PLAIN..=LAST_PLAIN) => out.push(plain),
            _ => out.extend_from_slice(format!(r"\u{unit:04x}").as_bytes()),
        }
    }
    out.push(b'"');
}

#[cfg(test)]
mod tests {
    use serde_json::Value;

    use super::{INDENT, JsonValue, render};

    /// The strings the shared vectors pin, each with what the extraction scripts write for it.
    const JSON_STRINGS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/legends/json-strings.json"
    ));

    /// How many strings the shared vectors pin.
    const STRING_CASES: usize = 10;

    /// The step between the characters beyond the Basic Multilingual Plane that a test writes,
    /// from U+10000 to U+10FFFF, both included. It is no multiple of the 1,024 values each half of
    /// a surrogate pair takes, so the halves of the characters' pairs run through all of them.
    const ASTRAL_STEP: usize = 1_023;

    /// A key that is no ASCII: an accented letter, and a character beyond the Basic Multilingual
    /// Plane.
    const NON_ASCII_KEY: &str = "cl\u{e9} \u{1d11e}";

    /// A document as text, which it always is, since every byte of it is ASCII.
    fn text_of(document: Vec<u8>) -> String {
        String::from_utf8(document).expect("a document is ASCII")
    }

    /// What a document of a string writes between its quotes.
    fn string_body(document: &[u8]) -> &[u8] {
        document
            .strip_prefix(b"\"")
            .and_then(|rest| rest.strip_suffix(b"\"\n"))
            .expect("a document of a string")
    }

    /// The UTF-16 unit that a document of a string of one unit stands for, read by the forms the
    /// extraction scripts write: a printable ASCII character but the quote and the backslash as
    /// itself, one of JSON's short escapes, or `\u` and four lowercase hexadecimal digits.
    fn unit_read_from(document: &[u8]) -> u16 {
        let body = string_body(document);
        match body {
            [plain] => {
                assert!(matches!(plain, b' '..=b'~'), "{body:?}");
                assert!(!matches!(plain, b'"' | b'\\'), "{body:?}");
                u16::from(*plain)
            }
            [b'\\', b'u', digits @ ..] => {
                assert_eq!(digits.len(), 4, "{body:?}");
                let lowercase = |digit: &u8| matches!(digit, b'0'..=b'9' | b'a'..=b'f');
                assert!(digits.iter().all(lowercase), "{body:?}");
                u16::from_str_radix(std::str::from_utf8(digits).unwrap(), 16).unwrap()
            }
            [b'\\', escaped] => u16::from(match escaped {
                b'"' => b'"',
                b'\\' => b'\\',
                b'n' => b'\n',
                b'r' => b'\r',
                b't' => b'\t',
                // The backspace and the form feed.
                b'b' => 0x08,
                b'f' => 0x0C,
                _ => panic!("no short escape: {body:?}"),
            }),
            _ => panic!("no string of one unit: {body:?}"),
        }
    }

    #[test]
    fn strings_follow_the_shared_vectors() {
        let vectors: Value = serde_json::from_str(JSON_STRINGS).unwrap();
        let cases = vectors["cases"].as_array().unwrap();
        assert_eq!(cases.len(), STRING_CASES);
        let mut given_by_units = 0;
        for case in cases {
            let rendered = case["rendered"].as_str().unwrap();
            let values = if let Some(text) = case.get("text") {
                // Text given by its characters, and by its UTF-16 units, is written alike.
                let text = text.as_str().unwrap();
                let units = text.encode_utf16().collect();
                vec![JsonValue::Str(text.to_owned()), JsonValue::Units(units)]
            } else {
                // Text that is no Unicode, which only its units can give.
                given_by_units += 1;
                let units = case["units"].as_array().unwrap().iter();
                let units = units.map(|unit| u16::try_from(unit.as_u64().unwrap()).unwrap());
                vec![JsonValue::Units(units.collect())]
            };
            for value in values {
                assert_eq!(text_of(render(&value)), format!("{rendered}\n"), "{case}");
                // A manifest is indented, which writes a string the same way.
                let member = JsonValue::Object(vec![("name", value)]);
                let expected = format!("{{\n{INDENT}\"name\": {rendered}\n}}\n");
                assert_eq!(text_of(render(&member)), expected, "{case}");
            }
        }
        assert!(given_by_units > 0);
    }

    #[test]
    fn containers_nest_as_the_scripts_indent_them() {
        use JsonValue::{Array, Bool, Int, Object, Str, Uint, Units};

        // An object holding an array, an empty array, an empty object, a nested object and an
        // array of arrays, its keys in no sorted order, as the scripts keep the order they are
        // given.
        let value = Object(vec![
            (
                "array",
                Array(vec![Uint(1), Str("two".to_owned()), Bool(true)]),
            ),
            ("empty array", Array(Vec::new())),
            ("empty object", Object(Vec::new())),
            (
                "object",
                Object(vec![
                    ("inner", Object(vec![("deepest", Bool(false))])),
                    ("units", Units(vec![u16::from(b'x')])),
                ]),
            ),
            (
                "arrays",
                Array(vec![
                    Array(vec![Int(-1), Array(Vec::new())]),
                    Array(Vec::new()),
                    Array(vec![Object(Vec::new())]),
                ]),
            ),
        ]);
        let expected = concat!(
            "{\n",
            "  \"array\": [\n",
            "    1,\n",
            "    \"two\",\n",
            "    true\n",
            "  ],\n",
            "  \"empty array\": [],\n",
            "  \"empty object\": {},\n",
            "  \"object\": {\n",
            "    \"inner\": {\n",
            "      \"deepest\": false\n",
            "    },\n",
            "    \"units\": \"x\"\n",
            "  },\n",
            "  \"arrays\": [\n",
            "    [\n",
            "      -1,\n",
            "      []\n",
            "    ],\n",
            "    [],\n",
            "    [\n",
            "      {}\n",
            "    ]\n",
            "  ]\n",
            "}\n",
        );
        assert_eq!(text_of(render(&value)), expected);

        // A document of an empty container, and of a value alone.
        assert_eq!(text_of(render(&Array(Vec::new()))), "[]\n");
        assert_eq!(text_of(render(&Object(Vec::new()))), "{}\n");
        assert_eq!(text_of(render(&Bool(false))), "false\n");
        assert_eq!(text_of(render(&Str("x".to_owned()))), "\"x\"\n");
    }

    #[test]
    fn integers_print_in_decimal() {
        use JsonValue::{Array, Int, Uint};

        let value = Array(vec![
            Int(i64::MIN),
            Int(-150),
            Int(0),
            Int(i64::MAX),
            Uint(0),
            Uint(u64::MAX),
        ]);
        let expected = concat!(
            "[\n",
            "  -9223372036854775808,\n",
            "  -150,\n",
            "  0,\n",
            "  9223372036854775807,\n",
            "  0,\n",
            "  18446744073709551615\n",
            "]\n",
        );
        assert_eq!(text_of(render(&value)), expected);
        assert_eq!(text_of(render(&Int(-150))), "-150\n");
    }

    #[test]
    fn every_byte_written_is_ascii() {
        // Every UTF-16 unit, an unpaired surrogate or not, is written in ASCII by a form the
        // scripts write, and reads back as itself.
        let mut bodies = Vec::new();
        for unit in 0..=u16::MAX {
            let document = render(&JsonValue::Units(vec![unit]));
            assert!(document.is_ascii(), "{unit:#06x}");
            assert_eq!(unit_read_from(&document), unit, "{unit:#06x}");
            bodies.extend_from_slice(string_body(&document));
        }
        // In one string, each is written as it is alone, whatever its neighbours.
        let every_unit = render(&JsonValue::Units((0..=u16::MAX).collect()));
        assert!(every_unit.is_ascii());
        assert!(string_body(&every_unit) == bodies);

        // Every character of the Basic Multilingual Plane and characters beyond it, under a key
        // that is no ASCII either, are written in ASCII, and read back as themselves.
        let plane = (0..=0xFFFF).filter_map(char::from_u32);
        let beyond = (0x1_0000..=u32::from(char::MAX)).step_by(ASTRAL_STEP);
        let text: String = plane.chain(beyond.filter_map(char::from_u32)).collect();
        assert!(text.ends_with(char::MAX));
        let document = render(&JsonValue::Object(vec![(
            NON_ASCII_KEY,
            JsonValue::Str(text.clone()),
        )]));
        assert!(document.is_ascii());
        // A key is escaped as any other string is.
        let key_line = br#"  "cl\u00e9 \ud834\udd1e": ""#;
        assert!(document.starts_with(b"{\n") && document[2..].starts_with(key_line));
        let read: Value = serde_json::from_slice(&document).unwrap();
        let members = read.as_object().unwrap();
        assert_eq!(members.len(), 1);
        assert_eq!(members[NON_ASCII_KEY], Value::String(text));
    }
}
