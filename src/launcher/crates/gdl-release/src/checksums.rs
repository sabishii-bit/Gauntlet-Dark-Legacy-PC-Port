//! A release's checksum list: the SHA-256 digest of each file the release carries beside it, as
//! the release scripts write one. It is read strictly, so that every download is held to the
//! digest the list gives it as well as to GitHub's, and a list in any other form is refused rather
//! than read as far as it goes.

use std::borrow::Cow;
use std::collections::BTreeMap;
use std::collections::btree_map::Entry;

use gdl_media::Sha256Digest;

use crate::ReleaseError;

/// The most bytes a checksum list or its signature may hold.
pub const MAX_LIST_LEN: u64 = 64 << 10;

/// How many hexadecimal digits a line writes its digest in.
const DIGEST_DIGITS: usize = 64;

/// What parts a line's digest from its name: two spaces, as the release scripts write it.
const SEPARATOR: &str = "  ";

/// The most characters a listed name may hold, the most a file name may.
const MAX_NAME_LEN: usize = 255;

/// The characters an asset name is made of besides ASCII letters and digits.
const NAME_PUNCTUATION: [u8; 4] = *b"._+-";

/// What ends every line.
const LINE_FEED: u8 = b'\n';

/// What may stand before a line's line feed: a list written on Windows ends its lines so.
const CARRIAGE_RETURN: u8 = b'\r';

/// A release's checksum list, as the release scripts write it (`scripts/release/release.py`): a
/// line for each file, its SHA-256 digest in lowercase hexadecimal, two spaces and its name.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChecksumList {
    /// The digest of each file the list names.
    entries: BTreeMap<String, Sha256Digest>,
}

impl ChecksumList {
    /// Reads a checksum list: at most [`MAX_LIST_LEN`] bytes of ASCII lines, each ended by a line
    /// feed with an optional carriage return before it, each 64 lowercase hexadecimal digits, two
    /// spaces and a name of 1 to 255 ASCII letters, digits, `.`, `_`, `+` and `-`; at least one
    /// line, none empty, and no name twice. Anything else is [`ReleaseError::ChecksumList`],
    /// naming the line that breaks the form.
    pub fn parse(bytes: &[u8]) -> Result<ChecksumList, ReleaseError> {
        if bytes.len() as u64 > MAX_LIST_LEN {
            return Err(malformed(format!(
                "it holds more than {MAX_LIST_LEN} bytes"
            )));
        }
        if bytes.is_empty() {
            return Err(malformed("it lists no file".to_owned()));
        }
        let mut entries = BTreeMap::new();
        for (index, line) in bytes.split_inclusive(|&byte| byte == LINE_FEED).enumerate() {
            let number = index + 1;
            let refused = |why: &str| malformed(format!("line {number} {why}"));
            let Some(line) = line.strip_suffix(&[LINE_FEED]) else {
                return Err(refused("does not end with a line feed"));
            };
            let line = line.strip_suffix(&[CARRIAGE_RETURN]).unwrap_or(line);
            let line = match std::str::from_utf8(line) {
                Ok(line) if line.is_ascii() => line,
                _ => return Err(refused("holds a byte that is not ASCII")),
            };
            let (digest, name) = entry(line).map_err(|why| refused(&why))?;
            match entries.entry(name.to_owned()) {
                Entry::Vacant(slot) => {
                    slot.insert(digest);
                }
                Entry::Occupied(_) => {
                    return Err(refused(&format!("names {name} a second time")));
                }
            }
        }
        Ok(ChecksumList { entries })
    }

    /// The digest the list gives the file named exactly `name`, or `None` when it names no such
    /// file.
    pub fn digest_of(&self, name: &str) -> Option<Sha256Digest> {
        self.entries.get(name).copied()
    }
}

/// The digest and name one line of a list gives, or why the line gives none.
fn entry(line: &str) -> Result<(Sha256Digest, &str), Cow<'static, str>> {
    if line.is_empty() {
        return Err("is empty".into());
    }
    let not_a_digest = || {
        Cow::Owned(format!(
            "does not begin with {DIGEST_DIGITS} lowercase hexadecimal digits"
        ))
    };
    let (digits, rest) = line
        .split_at_checked(DIGEST_DIGITS)
        .ok_or_else(not_a_digest)?;
    let digest = Sha256Digest::from_hex(digits).ok_or_else(not_a_digest)?;
    let name = rest
        .strip_prefix(SEPARATOR)
        .ok_or("does not part its digest from its name by two spaces")?;
    if name.is_empty() {
        return Err("names no file".into());
    }
    if name.len() > MAX_NAME_LEN {
        return Err(format!("names a file of more than {MAX_NAME_LEN} characters").into());
    }
    let named_as_an_asset = name
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || NAME_PUNCTUATION.contains(&byte));
    if !named_as_an_asset {
        return Err("names a file with a character no asset name holds".into());
    }
    Ok((digest, name))
}

/// The refusal of a list that breaks the form, for `detail`.
fn malformed(detail: String) -> ReleaseError {
    ReleaseError::ChecksumList {
        detail: detail.into(),
    }
}

#[cfg(test)]
mod tests {
    use gdl_media::Sha256Digest;

    use super::{ChecksumList, DIGEST_DIGITS, MAX_LIST_LEN, MAX_NAME_LEN, SEPARATOR};
    use crate::{Platform, ReleaseError, Version, launcher_asset, licences_asset};

    /// What a line holds besides its name: the digest, the two spaces and the line feed.
    const LINE_OVERHEAD: usize = DIGEST_DIGITS + SEPARATOR.len() + 1;

    /// The longest line a list may hold: a name of the most characters one may hold.
    const LONGEST_LINE: usize = LINE_OVERHEAD + MAX_NAME_LEN;

    /// The shortest line a list may hold: a name of one character.
    const SHORTEST_LINE: usize = LINE_OVERHEAD + 1;

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The list the release scripts write of `files`, each name with the digest of its bytes,
    /// each line ended by `ending`.
    fn list_of(files: &[(&str, &[u8])], ending: &str) -> Vec<u8> {
        files
            .iter()
            .map(|(name, bytes)| format!("{}  {name}{ending}", Sha256Digest::of(bytes)))
            .collect::<String>()
            .into_bytes()
    }

    /// A list the release scripts could write that holds exactly `len` bytes, with the name of
    /// its first line: lines of the longest names, then shorter ones to make up the rest.
    fn list_of_len(len: usize) -> (Vec<u8>, String) {
        let digest = Sha256Digest::of(b"payload");
        let mut list = String::new();
        let mut first = None;
        for index in 0usize.. {
            let left = len - list.len();
            if left == 0 {
                break;
            }
            // A line short of the longest leaves at least the shortest line's room after it.
            let line_len = if left <= LONGEST_LINE {
                left
            } else {
                (left - SHORTEST_LINE).min(LONGEST_LINE)
            };
            let name_len = line_len - LINE_OVERHEAD;
            let name = format!("{index:0>name_len$}");
            assert_eq!(name.len(), name_len);
            list.push_str(&format!("{digest}  {name}\n"));
            first.get_or_insert(name);
        }
        assert_eq!(list.len(), len);
        (list.into_bytes(), first.expect("a list of a line or more"))
    }

    /// Why `bytes` are no checksum list, as the refusal says it.
    fn refusal(bytes: &[u8]) -> String {
        match ChecksumList::parse(bytes) {
            Err(error @ ReleaseError::ChecksumList { .. }) => {
                assert_eq!(error.rule(), "checksumList");
                let shown = error.to_string();
                let detail = shown
                    .strip_prefix("The release's checksum list is malformed: ")
                    .unwrap_or_else(|| panic!("{shown}"));
                detail.to_owned()
            }
            other => panic!("{:?} read as {other:?}", String::from_utf8_lossy(bytes)),
        }
    }

    #[test]
    fn release_py_lists_are_read() {
        // The lists a new-line release carries, for a version with build metadata too, whose
        // names hold a `+`: the launcher asset and its licences.
        for text in ["1.0.0-beta", "1.0.0+build.1"] {
            let version = version(text);
            for platform in Platform::ALL {
                let launcher = launcher_asset(&version, platform);
                let licences = licences_asset(&version, platform);
                let files: [(&str, &[u8]); 2] =
                    [(&launcher, b"the launcher"), (&licences, b"the licences")];
                let lf = ChecksumList::parse(&list_of(&files, "\n"))
                    .unwrap_or_else(|error| panic!("{error}"));
                // Python's write_text on Windows ended each line with a carriage return too.
                let crlf = ChecksumList::parse(&list_of(&files, "\r\n"))
                    .unwrap_or_else(|error| panic!("{error}"));
                assert_eq!(lf, crlf);
                for (name, bytes) in files {
                    assert_eq!(lf.digest_of(name), Some(Sha256Digest::of(bytes)), "{name}");
                }
                // A name is looked up exactly as it is written.
                assert_eq!(lf.digest_of(&launcher.to_uppercase()), None);
                assert_eq!(lf.digest_of(&launcher[..launcher.len() - 1]), None);
                assert_eq!(lf.digest_of(&format!("{launcher}.sig")), None);
                assert_eq!(lf.digest_of(""), None);
            }
        }

        // A list as an alpha release carries it: the setup, its licences and the runtime, each
        // line ended as Windows ended it, and lines ended either way in one list.
        let setup = "GauntletDarkLegacy-1.0.0-alpha.3-windows-x64-setup.exe";
        let licences = "GauntletDarkLegacy-1.0.0-alpha.3-windows-x64-setup-licenses.zip";
        let runtime = "GauntletDarkLegacy-1.0.0-alpha.3-windows-x64-runtime.zip";
        let files: [(&str, &[u8]); 3] = [
            (setup, b"setup"),
            (licences, b"licences"),
            (runtime, b"runtime"),
        ];
        let crlf = ChecksumList::parse(&list_of(&files, "\r\n")).expect("an alpha list");
        let mut mixed = list_of(&files[..1], "\r\n");
        mixed.extend(list_of(&files[1..2], "\n"));
        mixed.extend(list_of(&files[2..], "\r\n"));
        assert_eq!(ChecksumList::parse(&mixed).expect("a mixed list"), crlf);
        for (name, bytes) in files {
            assert_eq!(
                crlf.digest_of(name),
                Some(Sha256Digest::of(bytes)),
                "{name}"
            );
        }

        // A digest reads as its text gives it.
        let text = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        let list = ChecksumList::parse(format!("{text}  abc\n").as_bytes()).expect("one line");
        assert_eq!(list.digest_of("abc"), Sha256Digest::from_hex(text));
        assert_eq!(list.digest_of("abc"), Some(Sha256Digest::of(b"abc")));

        // A name of the most characters a file name may hold, and a list of the most bytes.
        let longest = "n".repeat(MAX_NAME_LEN);
        let list =
            ChecksumList::parse(&list_of(&[(&longest, b"long")], "\n")).expect("the longest name");
        assert_eq!(list.digest_of(&longest), Some(Sha256Digest::of(b"long")));
        let bound = usize::try_from(MAX_LIST_LEN).expect("the bound fits in memory");
        let (largest, first) = list_of_len(bound);
        let list = ChecksumList::parse(&largest).unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(list.digest_of(&first), Some(Sha256Digest::of(b"payload")));
    }

    #[test]
    fn malformed_lists_are_refused() {
        let digest = Sha256Digest::of(b"payload").hex();
        let good = format!("{digest}  first.zip\n");
        let bound = usize::try_from(MAX_LIST_LEN).expect("the bound fits in memory");
        let cases: Vec<(&str, Vec<u8>, String)> = vec![
            (
                "one space",
                format!("{good}{digest} second.zip\n").into_bytes(),
                "line 2 does not part its digest from its name by two spaces".to_owned(),
            ),
            (
                "three spaces",
                format!("{digest}   second.zip\n").into_bytes(),
                "line 1 names a file with a character no asset name holds".to_owned(),
            ),
            (
                "sha256sum's binary marker",
                format!("{digest} *second.zip\n").into_bytes(),
                "line 1 does not part its digest from its name by two spaces".to_owned(),
            ),
            (
                "a tab",
                format!("{digest}\tsecond.zip\n").into_bytes(),
                "line 1 does not part its digest from its name by two spaces".to_owned(),
            ),
            (
                "upper-case hex",
                format!("{}  second.zip\n", digest.to_uppercase()).into_bytes(),
                "line 1 does not begin with 64 lowercase hexadecimal digits".to_owned(),
            ),
            (
                "a digit short",
                format!("{good}{}  second.zip\n", &digest[..DIGEST_DIGITS - 1]).into_bytes(),
                "line 2 does not begin with 64 lowercase hexadecimal digits".to_owned(),
            ),
            (
                "a digit more",
                format!("{digest}0  second.zip\n").into_bytes(),
                "line 1 does not part its digest from its name by two spaces".to_owned(),
            ),
            (
                "a digest alone",
                format!("{digest}\n").into_bytes(),
                "line 1 does not part its digest from its name by two spaces".to_owned(),
            ),
            (
                "a short line",
                format!("{good}{}\n", &digest[..10]).into_bytes(),
                "line 2 does not begin with 64 lowercase hexadecimal digits".to_owned(),
            ),
            (
                "a blank line",
                format!("{good}\n{digest}  second.zip\n").into_bytes(),
                "line 2 is empty".to_owned(),
            ),
            (
                "a blank line ended as Windows ends it",
                format!("{good}\r\n").into_bytes(),
                "line 2 is empty".to_owned(),
            ),
            (
                "a name twice",
                format!("{good}{}  first.zip\n", Sha256Digest::of(b"other")).into_bytes(),
                "line 2 names first.zip a second time".to_owned(),
            ),
            (
                "a name twice with one digest",
                format!("{good}{good}").into_bytes(),
                "line 2 names first.zip a second time".to_owned(),
            ),
            (
                "a name with a space",
                format!("{digest}  first part.zip\n").into_bytes(),
                "line 1 names a file with a character no asset name holds".to_owned(),
            ),
            (
                "a name with a path",
                format!("{digest}  ../first.zip\n").into_bytes(),
                "line 1 names a file with a character no asset name holds".to_owned(),
            ),
            (
                "no name",
                format!("{good}{digest}  \n").into_bytes(),
                "line 2 names no file".to_owned(),
            ),
            (
                "a name longer than a file name",
                format!("{digest}  {}\n", "n".repeat(MAX_NAME_LEN + 1)).into_bytes(),
                "line 1 names a file of more than 255 characters".to_owned(),
            ),
            (
                "a carriage return inside a line",
                format!("{digest}  first\r.zip\n").into_bytes(),
                "line 1 names a file with a character no asset name holds".to_owned(),
            ),
            (
                "two carriage returns",
                format!("{digest}  first.zip\r\r\n").into_bytes(),
                "line 1 names a file with a character no asset name holds".to_owned(),
            ),
            (
                "a carriage return alone",
                format!("{digest}  first.zip\r").into_bytes(),
                "line 1 does not end with a line feed".to_owned(),
            ),
            (
                "no line feed at the end",
                format!("{good}{digest}  second.zip").into_bytes(),
                "line 2 does not end with a line feed".to_owned(),
            ),
            ("nothing", Vec::new(), "it lists no file".to_owned()),
            (
                "more than 64 KiB",
                list_of_len(bound + 1).0,
                format!("it holds more than {MAX_LIST_LEN} bytes"),
            ),
            (
                "a byte past ASCII",
                format!("{good}{digest}  caf\u{e9}.zip\n").into_bytes(),
                "line 2 holds a byte that is not ASCII".to_owned(),
            ),
            (
                "a byte that is no UTF-8",
                [good.as_bytes(), digest.as_bytes(), b"  first\xff.zip\n"].concat(),
                "line 2 holds a byte that is not ASCII".to_owned(),
            ),
            (
                "a byte order mark",
                format!("\u{feff}{good}").into_bytes(),
                "line 1 holds a byte that is not ASCII".to_owned(),
            ),
        ];
        for (case, bytes, expected) in cases {
            assert_eq!(refusal(&bytes), expected, "{case}");
        }
    }
}
