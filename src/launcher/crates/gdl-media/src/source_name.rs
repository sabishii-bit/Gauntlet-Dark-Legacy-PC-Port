//! A dump's file name as the system gave it, kept in UTF-16 code units, so that a name that is not
//! Unicode keeps every unit and reaches a manifest as the extraction scripts write it. Windows
//! gives a name in UTF-16 units. A Unix system gives bytes, which are read here as UTF-8; the
//! scripts read them so where Python reads names as UTF-8, under a UTF-8 or the C locale or in
//! Python's UTF-8 mode, and under another locale they read a name outside ASCII in that locale's
//! encoding instead.

use std::ffi::OsStr;
use std::path::Path;

/// What a byte that is no part of UTF-8 is added to, to become a unit of its own: the bytes 0x80
/// to 0xFF become the low surrogates U+DC80 to U+DCFF, as the extraction scripts read such a byte
/// in a name where Python reads names as UTF-8.
#[cfg(unix)]
const ESCAPED_BYTE_BASE: u16 = 0xDC00;

/// A dump's file name as the system gave it, in UTF-16 code units.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceName(Vec<u16>);

impl SourceName {
    /// The name of the file `path` names: its last component as written, never resolved and never
    /// the spelling the file system keeps. `None` when `path` names no file, as a root or a path
    /// that ends in `..` does.
    pub fn of(path: &Path) -> Option<SourceName> {
        path.file_name().map(|name| SourceName(units_of(name)))
    }

    /// The name these units spell.
    pub fn from_units(units: Vec<u16>) -> SourceName {
        SourceName(units)
    }

    /// The name's UTF-16 code units, each unpaired surrogate kept as itself.
    pub fn units(&self) -> &[u16] {
        &self.0
    }

    /// The name as text, with U+FFFD in place of each unpaired surrogate.
    pub fn to_string_lossy(&self) -> String {
        String::from_utf16_lossy(&self.0)
    }
}

/// The UTF-16 units of a name Windows gave, which are the units it holds, unpaired surrogates
/// included.
#[cfg(windows)]
fn units_of(name: &OsStr) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;

    name.encode_wide().collect()
}

/// The UTF-16 units of a name a Unix system gave as bytes: each run of UTF-8 in it in UTF-16, and
/// each byte that is no part of UTF-8 a low surrogate of its own.
#[cfg(unix)]
fn units_of(name: &OsStr) -> Vec<u16> {
    use std::os::unix::ffi::OsStrExt;

    let mut units = Vec::with_capacity(name.len());
    for chunk in name.as_bytes().utf8_chunks() {
        units.extend(chunk.valid().encode_utf16());
        let escaped = chunk.invalid().iter();
        units.extend(escaped.map(|&byte| ESCAPED_BYTE_BASE + u16::from(byte)));
    }
    units
}

#[cfg(test)]
mod tests {
    use std::ffi::OsString;
    use std::path::Path;

    use super::SourceName;

    /// A name of letters, one of them accented, and of one character outside the Basic
    /// Multilingual Plane, which UTF-16 writes as a surrogate pair.
    const UNICODE_NAME: &str = "sourc\u{e9}\u{1d11e}.z64";

    /// That name's units.
    const UNICODE_UNITS: [u16; 12] = [
        115, 111, 117, 114, 99, 0xE9, 0xD834, 0xDD1E, 46, 122, 54, 52,
    ];

    /// The units of a name that is not Unicode: `x`, then the byte 0x80 or the unpaired surrogate
    /// U+DC80, then `.z64`.
    const NOT_UNICODE_UNITS: [u16; 6] = [120, 0xDC80, 46, 122, 54, 52];

    /// A name that is not Unicode as Windows holds one: an unpaired surrogate among its units.
    #[cfg(windows)]
    fn not_unicode() -> OsString {
        use std::os::windows::ffi::OsStringExt;

        OsString::from_wide(&NOT_UNICODE_UNITS)
    }

    /// A name that is not Unicode as Linux holds one: a byte that is no part of UTF-8.
    #[cfg(unix)]
    fn not_unicode() -> OsString {
        use std::os::unix::ffi::OsStringExt;

        OsString::from_vec(b"x\x80.z64".to_vec())
    }

    #[test]
    fn a_unicode_name_keeps_its_characters() {
        assert!(UNICODE_NAME.encode_utf16().eq(UNICODE_UNITS));
        // The last component as written, whatever comes before it: no folder named exists, and
        // nothing is looked up.
        let paths = [
            Path::new(UNICODE_NAME).to_path_buf(),
            Path::new("no such folder").join(UNICODE_NAME),
            Path::new("no such folder").join("..").join(UNICODE_NAME),
        ];
        for path in paths {
            let name = SourceName::of(&path).unwrap();
            assert_eq!(name.units(), UNICODE_UNITS, "{}", path.display());
            assert_eq!(name.to_string_lossy(), UNICODE_NAME);
            assert_eq!(name, SourceName::from_units(UNICODE_UNITS.to_vec()));
        }

        // Letter case is kept as written.
        let name = SourceName::of(Path::new("Dump.Z64")).unwrap();
        assert_eq!(name.to_string_lossy(), "Dump.Z64");
        assert_ne!(name, SourceName::of(Path::new("dump.z64")).unwrap());
    }

    #[test]
    fn a_name_outside_unicode_keeps_its_units() {
        let path = Path::new("dumps").join(not_unicode());
        let name = SourceName::of(&path).unwrap();
        assert_eq!(name.units(), NOT_UNICODE_UNITS);
        assert_eq!(name, SourceName::from_units(NOT_UNICODE_UNITS.to_vec()));
        assert_eq!(name.to_string_lossy(), "x\u{fffd}.z64");
    }

    #[test]
    fn a_path_that_names_no_file_has_no_name() {
        for path in ["", "/", "..", "dumps/.."] {
            assert_eq!(SourceName::of(Path::new(path)), None, "{path:?}");
        }
    }

    /// Each byte that is no part of UTF-8 becomes the low surrogate of its own value, wherever it
    /// stands and whatever sequence it cuts short, as the extraction scripts read a name on Linux.
    #[cfg(unix)]
    #[test]
    fn each_byte_outside_utf8_is_escaped_alone() {
        use std::ffi::OsStr;
        use std::os::unix::ffi::OsStrExt;

        let cases: [(&[u8], &[u16]); 8] = [
            // A sequence cut short, then a letter.
            (b"\xe2\x82a", &[0xDCE2, 0xDC82, 0x61]),
            // A surrogate written in UTF-8.
            (b"\xed\xa0\x80", &[0xDCED, 0xDCA0, 0xDC80]),
            // A character past U+10FFFF.
            (b"\xf4\x90\x80\x80", &[0xDCF4, 0xDC90, 0xDC80, 0xDC80]),
            // A character written in more bytes than it needs.
            (b"\xc0\xaf", &[0xDCC0, 0xDCAF]),
            // Letters, an accented one, then a byte no sequence begins with.
            (b"caf\xc3\xa9\xff", &[0x63, 0x61, 0x66, 0xE9, 0xDCFF]),
            // A character outside the Basic Multilingual Plane, whole and then cut short.
            (b"\xf0\x9d\x84\x9e", &[0xD834, 0xDD1E]),
            (b"\xf0\x9d\x84", &[0xDCF0, 0xDC9D, 0xDC84]),
            // Two continuation bytes with nothing to continue.
            (b"\x80\x80", &[0xDC80, 0xDC80]),
        ];
        for (bytes, units) in cases {
            let name = SourceName::of(Path::new(OsStr::from_bytes(bytes))).unwrap();
            assert_eq!(name.units(), units, "{bytes:x?}");
        }
    }

    /// Windows gives a name's units as it holds them: an unpaired surrogate of either kind stays
    /// where it stands, and a pair stays a pair.
    #[cfg(windows)]
    #[test]
    fn unpaired_surrogates_keep_their_places() {
        use std::os::windows::ffi::OsStringExt;

        let cases: [&[u16]; 3] = [
            &[0xDD1E, 0x61, 0xD834],
            &[0xD834, 0xDD1E],
            &[0xDD1E, 0xD834],
        ];
        for units in cases {
            let path = Path::new("dumps").join(OsString::from_wide(units));
            let name = SourceName::of(&path).unwrap();
            assert_eq!(name.units(), units, "{units:x?}");
        }
    }
}
