//! Paths past MAX_PATH: the verbatim form that the crate's raw Windows calls take, the plain form a
//! path is shown, compared and measured in, and the length an install root is held to on Windows.

use std::io;
use std::path::{Path, PathBuf};

/// The most UTF-16 units an install root may have on Windows. The game is not long-path aware, so
/// Windows holds every path it opens to MAX_PATH, 260 units, and this leaves room below the root
/// for the deepest file the game reads.
pub const MAX_INSTALL_ROOT_UTF16: usize = 180;

/// The separator of Windows paths, as a UTF-16 unit.
#[cfg(windows)]
const SEPARATOR: u16 = b'\\' as u16;

/// The colon after a drive's letter, as a UTF-16 unit.
#[cfg(windows)]
const COLON: u16 = b':' as u16;

/// `\\?\`, which begins a verbatim path: Windows takes what follows as it is, without folding
/// `.` and `..` or holding it to MAX_PATH.
#[cfg(windows)]
const VERBATIM: [u16; 4] = [SEPARATOR, SEPARATOR, b'?' as u16, SEPARATOR];

/// `\??\`, the prefix of the system's own object names, which Windows also takes as it is.
#[cfg(windows)]
const OBJECT_NAMES: [u16; 4] = [SEPARATOR, b'?' as u16, b'?' as u16, SEPARATOR];

/// `\\.\`, which begins a device's path.
#[cfg(windows)]
const DEVICE: [u16; 4] = [SEPARATOR, SEPARATOR, b'.' as u16, SEPARATOR];

/// `\\?\UNC\`, which begins the verbatim path of a share, in place of the share's own `\\`.
#[cfg(windows)]
const VERBATIM_SHARE: [u16; 8] = [
    SEPARATOR,
    SEPARATOR,
    b'?' as u16,
    SEPARATOR,
    b'U' as u16,
    b'N' as u16,
    b'C' as u16,
    SEPARATOR,
];

/// `\\`, which begins the plain path of a share.
#[cfg(windows)]
const SHARE: [u16; 2] = [SEPARATOR, SEPARATOR];

/// Returns `path` without its verbatim prefix: `\\?\C:\x` becomes `C:\x`, and
/// `\\?\UNC\server\share\x` becomes `\\server\share\x`. Any other path, and every path on other
/// systems, comes back as it is. The plain form is for showing, comparing and measuring a path,
/// not for opening one: Windows drops a trailing dot or space from a plain path's name, which a
/// verbatim path keeps.
pub fn strip_verbatim(path: &Path) -> PathBuf {
    #[cfg(windows)]
    {
        use std::ffi::OsString;
        use std::os::windows::ffi::{OsStrExt, OsStringExt};

        let units: Vec<u16> = path.as_os_str().encode_wide().collect();
        let plain = if let Some(share) = units.strip_prefix(&VERBATIM_SHARE[..]) {
            Some([&SHARE[..], share].concat())
        } else {
            units
                .strip_prefix(&VERBATIM[..])
                .filter(|rest| is_drive_path(rest))
                .map(<[u16]>::to_vec)
        };
        if let Some(plain) = plain {
            return PathBuf::from(OsString::from_wide(&plain));
        }
    }
    path.to_path_buf()
}

/// Whether `units` begin with a drive's letter, its colon and a separator, as `C:\` does.
#[cfg(windows)]
fn is_drive_path(units: &[u16]) -> bool {
    match units {
        [letter, COLON, SEPARATOR, ..] => {
            u8::try_from(*letter).is_ok_and(|letter| letter.is_ascii_alphabetic())
        }
        _ => false,
    }
}

/// The length of `root` made absolute, without a verbatim prefix, in UTF-16 units: the units of
/// Windows' path limits. On Windows `.` and `..` are folded first, as Windows folds them; on
/// other systems a `..` counts as it stands, and a path that is not UTF-8 has no such length and is
/// refused.
pub fn root_utf16_len(root: &Path) -> io::Result<usize> {
    let absolute = strip_verbatim(&std::path::absolute(root)?);
    utf16_len(&absolute)
}

/// Whether `root` is short enough to install the game into: on Windows, at most
/// [`MAX_INSTALL_ROOT_UTF16`] units as [`root_utf16_len`] measures it. On other systems the game is
/// held to no such limit, and every root fits.
pub fn root_fits(root: &Path) -> io::Result<bool> {
    if cfg!(windows) {
        Ok(root_utf16_len(root)? <= MAX_INSTALL_ROOT_UTF16)
    } else {
        Ok(true)
    }
}

/// A path's length in UTF-16 units.
#[cfg(windows)]
fn utf16_len(path: &Path) -> io::Result<usize> {
    use std::os::windows::ffi::OsStrExt;
    Ok(path.as_os_str().encode_wide().count())
}

/// A path's length in UTF-16 units, refused when the path is not UTF-8.
#[cfg(not(windows))]
fn utf16_len(path: &Path) -> io::Result<usize> {
    path.to_str()
        .map(|text| text.encode_utf16().count())
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("{} is not UTF-8", path.display()),
            )
        })
}

/// A path in the form that raw Windows calls take past MAX_PATH: absolute and verbatim, as UTF-16
/// that ends in a NUL. std's own file calls make this form themselves; the launcher's raw calls,
/// in this crate and in the others, take it from here.
#[cfg(windows)]
pub struct VerbatimPath(Vec<u16>);

#[cfg(windows)]
impl VerbatimPath {
    /// Makes `path` absolute by Windows' rules, which fold `.` and `..` and turn `/` into `\`, then
    /// gives it its prefix as std does: `\\?\` before a drive's path, `\\?\UNC\` in place of a
    /// share's `\\`, and `\\?\` in place of a device's `\\.\`. A path that begins with `\\?\` or
    /// `\??\` is taken as it is. A path holding a NUL is refused.
    pub fn new(path: &Path) -> io::Result<VerbatimPath> {
        use std::os::windows::ffi::OsStrExt;

        let units: Vec<u16> = path.as_os_str().encode_wide().collect();
        if units.contains(&0) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "a path given to Windows holds a NUL",
            ));
        }
        let mut units = if is_taken_as_it_is(&units) {
            units
        } else {
            let absolute: Vec<u16> = std::path::absolute(path)?
                .as_os_str()
                .encode_wide()
                .collect();
            if matches!(absolute.as_slice(), [_, COLON, SEPARATOR, ..]) {
                [&VERBATIM[..], &absolute].concat()
            } else if let Some(device) = absolute.strip_prefix(&DEVICE[..]) {
                [&VERBATIM[..], device].concat()
            } else if is_taken_as_it_is(&absolute) {
                absolute
            } else if let Some(share) = absolute.strip_prefix(&SHARE[..]) {
                [&VERBATIM_SHARE[..], share].concat()
            } else {
                absolute
            }
        };
        units.push(0);
        Ok(VerbatimPath(units))
    }

    /// The path, for a call that only reads it while `self` lives.
    pub fn as_ptr(&self) -> *const u16 {
        self.0.as_ptr()
    }
}

/// Whether Windows takes a path that begins as `units` do as it stands: a verbatim path, or one of
/// the system's object names.
#[cfg(windows)]
fn is_taken_as_it_is(units: &[u16]) -> bool {
    units.starts_with(&VERBATIM) || units.starts_with(&OBJECT_NAMES)
}

#[cfg(test)]
mod tests {
    use std::io;
    use std::path::Path;

    use super::{MAX_INSTALL_ROOT_UTF16, root_fits, root_utf16_len, strip_verbatim};

    /// The text of a verbatim path, which ends in its one NUL.
    #[cfg(windows)]
    fn text_of(path: &super::VerbatimPath) -> String {
        let (last, text) = path.0.split_last().unwrap();
        assert_eq!(*last, 0);
        assert!(!text.contains(&0));
        assert_eq!(path.as_ptr(), path.0.as_ptr());
        String::from_utf16(text).unwrap()
    }

    /// The text of `path`'s verbatim form.
    #[cfg(windows)]
    fn verbatim(path: &str) -> String {
        text_of(&super::VerbatimPath::new(Path::new(path)).unwrap())
    }

    #[cfg(windows)]
    #[test]
    fn verbatim_forms() {
        use super::VerbatimPath;

        for (given, expected) in [
            (r"C:\a", r"\\?\C:\a"),
            (r"d:\x\y.json", r"\\?\d:\x\y.json"),
            (r"\\srv\s\x", r"\\?\UNC\srv\s\x"),
            (r"//srv/s/x", r"\\?\UNC\srv\s\x"),
            (r"\\?\C:\x", r"\\?\C:\x"),
            (r"\\?\UNC\srv\s\x", r"\\?\UNC\srv\s\x"),
            (r"\??\C:\x", r"\??\C:\x"),
            (r"\\.\C:\x", r"\\?\C:\x"),
            // Windows folds a plain path's dots and turns its slashes, and drops the dots and
            // spaces that end its last name, as it does when it opens the path.
            (r"C:/a/../b", r"\\?\C:\b"),
            (r"C:\a\.\b\..\c", r"\\?\C:\a\c"),
            (r"C:\a\..\..\b", r"\\?\C:\b"),
            (r"C:\a\b. .", r"\\?\C:\a\b"),
            (r"\\srv\s\..\t", r"\\?\UNC\srv\s\t"),
            // A verbatim path is never folded: its dots are names.
            (r"\\?\C:\a\..\b", r"\\?\C:\a\..\b"),
            // A verbatim prefix spelt with a forward slash is none to std, but Windows gives one
            // back when it makes the path absolute, and that path is then taken as it is.
            (r"//?/C:/x", r"\\?\C:\x"),
            (r"/\?\C:\x", r"\\?\C:\x"),
            (r"//?/UNC/srv/s/x", r"\\?\UNC\srv\s\x"),
            ("C:\\\u{e9}\u{1d11e}", "\\\\?\\C:\\\u{e9}\u{1d11e}"),
        ] {
            assert_eq!(verbatim(given), expected, "{given}");
        }

        // A relative path is taken from the working folder.
        let working = std::env::current_dir().unwrap();
        let working = working.to_str().unwrap();
        assert!(!working.starts_with(r"\\"), "{working}");
        assert_eq!(
            verbatim(r"relative\x"),
            format!(r"\\?\{}\relative\x", working.trim_end_matches('\\'))
        );

        // A NUL anywhere is refused, in a plain path and a verbatim one alike, as is an empty path.
        for given in ["C:\\a\0b", "\\\\?\\C:\\a\0b", "\0", ""] {
            let error = VerbatimPath::new(Path::new(given)).err().unwrap();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{given:?}");
        }
    }

    #[cfg(windows)]
    #[test]
    fn verbatim_paths_are_public() {
        // Reached from the crate's top, as the launcher's other crates reach it.
        use crate::VerbatimPath;

        let drive = VerbatimPath::new(Path::new(r"C:\a\b")).unwrap();
        assert_eq!(text_of(&drive), r"\\?\C:\a\b");
        let share = VerbatimPath::new(Path::new(r"\\srv\s\x")).unwrap();
        assert_eq!(text_of(&share), r"\\?\UNC\srv\s\x");
        let error = VerbatimPath::new(Path::new("C:\\a\0b")).err().unwrap();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
    }

    #[test]
    fn strip_and_root_length() {
        #[cfg(windows)]
        {
            // Stripping undoes the prefix a drive's path or a share's path is given.
            for given in [
                r"C:\a",
                r"C:\",
                r"d:\x\y.json",
                r"\\srv\share\x",
                r"\\srv\share",
                r"C:/a/../b",
            ] {
                let absolute = std::path::absolute(given).unwrap();
                let verbatim = std::path::PathBuf::from(verbatim(given));
                assert_ne!(verbatim, absolute, "{given}");
                assert_eq!(strip_verbatim(&verbatim), absolute, "{given}");
            }
            // A path without a plain form keeps its prefix, and a plain path stays as it is.
            for kept in [
                r"\\?\Volume{0e5a1c3f-2b4d-4c6e-8f10-123456789abc}\x",
                r"\\?\GLOBALROOT\Device\HarddiskVolume1\x",
                r"\\?\C:",
                r"\\?\1:\x",
                r"\??\C:\x",
                r"C:\x",
                r"\\srv\s\x",
                r"relative\x",
                "",
            ] {
                assert_eq!(strip_verbatim(Path::new(kept)), Path::new(kept), "{kept}");
            }

            // The length of the absolute root, without its prefix, in UTF-16 units: U+00E9 is one
            // unit and U+1D11E two.
            for (root, units) in [
                (r"C:\abc", 6),
                (r"\\?\C:\abc", 6),
                (r"C:\abc\", 7),
                (r"C:/abc/../d", 4),
                (r"\\srv\share", 11),
                (r"\\?\UNC\srv\share", 11),
                ("C:\\\u{e9}\u{1d11e}", 6),
            ] {
                assert_eq!(root_utf16_len(Path::new(root)).unwrap(), units, "{root}");
            }
        }
        #[cfg(not(windows))]
        {
            use std::ffi::OsStr;
            use std::os::unix::ffi::OsStrExt;

            // Nothing is a prefix here: these are names that happen to hold backslashes.
            for kept in [r"\\?\C:\x", r"\\?\UNC\srv\s\x", "/a/b", "relative", ""] {
                assert_eq!(strip_verbatim(Path::new(kept)), Path::new(kept), "{kept}");
            }

            for (root, units) in [
                ("/abc", 4),
                ("//abc", 5),
                ("/abc/./d", 6),
                ("/abc/../d", 9),
                ("/\u{e9}\u{1d11e}", 4),
                (r"/\\?\C:", 7),
            ] {
                assert_eq!(root_utf16_len(Path::new(root)).unwrap(), units, "{root}");
            }
            let working = std::env::current_dir().unwrap();
            let working = working.to_str().unwrap().encode_utf16().count();
            assert_eq!(root_utf16_len(Path::new("abc")).unwrap(), working + 4);

            // Bytes that are not UTF-8 have no length in UTF-16 units.
            let unreadable = Path::new(OsStr::from_bytes(b"/r\xff"));
            let error = root_utf16_len(unreadable).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
        }

        // An empty root has no absolute form.
        let error = root_utf16_len(Path::new("")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
    }

    #[cfg(windows)]
    #[test]
    fn a_windows_root_fits_up_to_180_units() {
        assert_eq!(MAX_INSTALL_ROOT_UTF16, 180);
        /// A root on a drive that measures `units`: `C:\` and a name.
        fn on_drive(units: usize) -> String {
            format!(r"C:\{}", "r".repeat(units - 3))
        }
        /// A root on a share that measures `units`: `\\srv\share\` and a name.
        fn on_share(units: usize) -> String {
            format!(r"\\srv\share\{}", "s".repeat(units - 12))
        }
        let fits = |root: &str| root_fits(Path::new(root)).unwrap();

        for (root, longer) in [
            (on_drive(180), on_drive(181)),
            (on_share(180), on_share(181)),
            // Measured without its verbatim prefix.
            (
                format!(r"\\?\{}", on_drive(180)),
                format!(r"\\?\{}", on_drive(181)),
            ),
            (
                format!(r"\\?\UNC\{}", &on_share(180)[2..]),
                format!(r"\\?\UNC\{}", &on_share(181)[2..]),
            ),
            // Measured once absolute: dots folded and slashes turned.
            (
                format!(r"{}\sub\..", on_drive(180)),
                format!(r"{}/sub/./..", on_drive(181)),
            ),
            // Measured in UTF-16 units, so U+1D11E counts two.
            (
                format!(r"C:\{}{}", "r".repeat(175), '\u{1d11e}'),
                format!(r"C:\{}{}", "r".repeat(176), '\u{1d11e}'),
            ),
        ] {
            assert_eq!(root_utf16_len(Path::new(&root)).unwrap(), 180, "{root}");
            assert!(fits(&root), "{root}");
            assert_eq!(root_utf16_len(Path::new(&longer)).unwrap(), 181, "{longer}");
            assert!(!fits(&longer), "{longer}");
        }

        // A relative root counts the working folder it lies in.
        let working = std::env::current_dir().unwrap();
        let working = root_utf16_len(&working).unwrap();
        let name = |units: usize| "r".repeat(units - working - 1);
        assert!(fits(&name(180)));
        assert!(!fits(&name(181)));

        // A root that has no absolute form is an error, not a root that fits.
        assert_eq!(
            root_fits(Path::new("")).unwrap_err().kind(),
            io::ErrorKind::InvalidInput
        );
    }

    #[cfg(not(windows))]
    #[test]
    fn long_roots_fit_on_linux() {
        use std::ffi::OsStr;
        use std::os::unix::ffi::OsStrExt;

        // A root that Windows would refuse fits here.
        let root = format!("/{}", "r".repeat(299));
        let units = root_utf16_len(Path::new(&root)).unwrap();
        assert_eq!(units, 300);
        assert!(units > MAX_INSTALL_ROOT_UTF16);
        assert!(root_fits(Path::new(&root)).unwrap());

        // Far past any Windows limit, and not even UTF-8: no root is too long here.
        let deep = format!("/{}", vec!["r".repeat(250); 40].join("/"));
        assert!(root_utf16_len(Path::new(&deep)).unwrap() > 10_000);
        assert!(root_fits(Path::new(&deep)).unwrap());
        let unreadable = Path::new(OsStr::from_bytes(b"/r\xff"));
        assert!(root_fits(unreadable).unwrap());
    }
}
