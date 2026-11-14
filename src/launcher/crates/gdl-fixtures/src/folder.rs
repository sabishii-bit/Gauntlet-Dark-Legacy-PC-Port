//! Install folders in the states the launcher's tests need: roots of an exact length, junctions,
//! the names the alpha installer leaves, and entries only root may open.

use std::fs;
use std::io;
use std::iter;
use std::path::{Component, Path, PathBuf};

/// The most characters a padding folder of a long root takes, well under the 255 that NTFS and
/// ext4 allow a name.
const MAX_PADDING_CHARS: usize = 64;

/// What the padding folders between a long root's base and the root itself are named with.
const PADDING_CHAR: char = 'p';

/// What a long root's own folder is named with. It differs from the padding, so a root made from
/// a base never lies inside another root made from it.
const ROOT_CHAR: char = 'r';

/// The names the alpha installer gives folders: the transaction folders of its game updates and of
/// its own. Every other name it leaves is a file.
const ALPHA_FOLDERS: [&str; 2] = [".gdl-update", ".gdl-installer-update"];

/// Characters that cmd changes in a command line even between quotes: `%` and `!` begin variables,
/// and `"` ends the quoting.
#[cfg(windows)]
const CHANGED_BY_CMD: [u8; 3] = *b"%!\"";

/// The mode of a denied entry: no reading, writing or searching for anyone but root.
#[cfg(unix)]
const NO_ACCESS: u32 = 0o000;

/// The bits of a mode that hold its permissions, the set-id and sticky bits included.
#[cfg(unix)]
const PERMISSION_BITS: u32 = 0o7777;

/// Makes a new, empty folder under `base` whose absolute path is exactly `utf16_units` UTF-16
/// units long, the measure the launcher holds an install root to, and returns that path.
///
/// Padding folders, each name far shorter than file systems allow, make up the length. `base` and
/// the padding are made when missing; the root itself must not exist yet. A base whose absolute
/// path keeps a verbatim prefix or a `..` is refused, because its length would then depend on how
/// it is resolved.
pub fn long_root(base: &Path, utf16_units: usize) -> io::Result<PathBuf> {
    let base = plain_absolute(base)?;
    let separator = usize::from(!ends_in_separator(&base));
    let lengths = utf16_units
        .checked_sub(utf16_len(&base)?)
        .and_then(|left| padding_lengths(left, separator))
        .unwrap_or_default();
    let Some((&own, padding)) = lengths.split_last() else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "no folder of {utf16_units} UTF-16 units fits under {}",
                base.display()
            ),
        ));
    };
    let mut parent = base;
    for &chars in padding {
        parent.push(iter::repeat_n(PADDING_CHAR, chars).collect::<String>());
    }
    let root = parent.join(iter::repeat_n(ROOT_CHAR, own).collect::<String>());
    let measured = utf16_len(&std::path::absolute(&root)?)?;
    if measured != utf16_units {
        return Err(io::Error::other(format!(
            "{} measures {measured} UTF-16 units, not {utf16_units}",
            root.display()
        )));
    }
    fs::create_dir_all(&parent)?;
    fs::create_dir(&root)?;
    Ok(root)
}

/// The lengths of folder names that use up exactly `left` units of a path, each name costing a
/// separator before it, the first only when `separator` is 1. `None` when no names can.
fn padding_lengths(mut left: usize, mut separator: usize) -> Option<Vec<usize>> {
    let mut lengths = Vec::new();
    while left > 0 {
        let room = left.checked_sub(separator).filter(|room| *room > 0)?;
        let chars = match room.checked_sub(MAX_PADDING_CHARS) {
            None | Some(0) => room,
            // A single unit left over could not hold another separator and character.
            Some(1) => MAX_PADDING_CHARS - 1,
            Some(_) => MAX_PADDING_CHARS,
        };
        lengths.push(chars);
        left -= separator + chars;
        separator = 1;
    }
    Some(lengths)
}

/// `path` made absolute by the rules std applies to it, refused when that keeps a verbatim prefix
/// or a `..`.
fn plain_absolute(path: &Path) -> io::Result<PathBuf> {
    let absolute = std::path::absolute(path)?;
    let plain = absolute.components().all(|component| match component {
        Component::Prefix(prefix) => !prefix.kind().is_verbatim(),
        other => other != Component::ParentDir,
    });
    if plain {
        Ok(absolute)
    } else {
        Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "{} measures differently once it is resolved",
                absolute.display()
            ),
        ))
    }
}

/// Whether `path` ends in a separator, as a drive's root does, so that a name joins it without
/// another.
fn ends_in_separator(path: &Path) -> bool {
    path.as_os_str()
        .as_encoded_bytes()
        .last()
        .is_some_and(|&byte| std::path::is_separator(char::from(byte)))
}

/// A path's length in UTF-16 units, the units of Windows' own path limits.
#[cfg(windows)]
fn utf16_len(path: &Path) -> io::Result<usize> {
    use std::os::windows::ffi::OsStrExt;
    Ok(path.as_os_str().encode_wide().count())
}

/// A path's length in UTF-16 units, the units of Windows' own path limits. A path that is not
/// UTF-8 has no such length here and is refused.
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

/// Makes `link` a junction to the folder `target` through `mklink /J`, which, unlike a symbolic
/// link, needs no privilege. Both paths reach cmd absolute and quoted; a path holding a character
/// that cmd would change even so is refused before anything runs.
#[cfg(windows)]
pub fn make_junction(link: &Path, target: &Path) -> io::Result<()> {
    use std::ffi::OsString;
    use std::os::windows::process::CommandExt;
    use std::process::Command;

    let link = std::path::absolute(link)?;
    let target = std::path::absolute(target)?;
    for path in [&link, &target] {
        let bytes = path.as_os_str().as_encoded_bytes();
        if bytes.iter().any(|byte| CHANGED_BY_CMD.contains(byte)) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("cmd would change {}", path.display()),
            ));
        }
    }
    let quoted = |path: &Path| {
        let mut text = OsString::from("\"");
        text.push(path.as_os_str());
        text.push("\"");
        text
    };
    let shell = std::env::var_os("ComSpec").unwrap_or_else(|| OsString::from("cmd.exe"));
    let output = Command::new(shell)
        .args(["/d", "/c", "mklink", "/J"])
        .raw_arg(quoted(&link))
        .raw_arg(quoted(&target))
        .output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "mklink /J {} {} failed ({}): {}",
            link.display(),
            target.display(),
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    if !fs::symlink_metadata(&link)?.file_type().is_symlink() {
        return Err(io::Error::other(format!(
            "mklink /J made no junction at {}",
            link.display()
        )));
    }
    Ok(())
}

/// Leaves `names` in `root` the way the alpha installer leaves them: its transaction folders, in
/// any letter case, as empty folders, and every other name as an empty file. Each must be a single
/// name, or nothing is made, and one that exists already is refused.
pub fn make_alpha_markers(root: &Path, names: &[&str]) -> io::Result<()> {
    if let Some(name) = names
        .iter()
        .find(|name| Path::new(name).file_name() != Some(name.as_ref()))
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{name:?} is not a single name"),
        ));
    }
    for name in names {
        let path = root.join(name);
        if ALPHA_FOLDERS
            .iter()
            .any(|folder| folder.eq_ignore_ascii_case(name))
        {
            fs::create_dir(&path)?;
        } else {
            fs::File::create_new(&path)?;
        }
    }
    Ok(())
}

/// Entries whose every permission a test has taken away, so that opening or listing them fails
/// for anyone but root. Dropping it gives each its mode back, which a `TempDir` needs before it
/// can remove them; made after the `TempDir`, it is dropped before it.
#[cfg(unix)]
#[derive(Debug)]
#[must_use = "dropping it gives the entries their modes back at once"]
pub struct DeniedAccess {
    /// Each denied entry and its mode before, in the order they were denied.
    modes: Vec<(PathBuf, u32)>,
}

#[cfg(unix)]
impl DeniedAccess {
    /// Sets each of `paths` to mode 0o000, in order. An entry inside a folder must come before
    /// that folder, which cannot be searched once denied. When one cannot be changed, those
    /// already denied get their modes back before the error returns.
    pub fn deny(paths: &[&Path]) -> io::Result<DeniedAccess> {
        use std::os::unix::fs::PermissionsExt;

        let mut denied = DeniedAccess {
            modes: Vec::with_capacity(paths.len()),
        };
        for path in paths {
            let mode = fs::metadata(path)?.permissions().mode() & PERMISSION_BITS;
            fs::set_permissions(path, fs::Permissions::from_mode(NO_ACCESS))?;
            denied.modes.push((path.to_path_buf(), mode));
        }
        Ok(denied)
    }
}

#[cfg(unix)]
impl Drop for DeniedAccess {
    fn drop(&mut self) {
        use std::os::unix::fs::PermissionsExt;

        // Last denied first, so a folder can be searched again before what lies inside it.
        for (path, mode) in self.modes.iter().rev() {
            // A drop cannot report a failure; removing the test's folder reports it instead.
            let _ = fs::set_permissions(path, fs::Permissions::from_mode(*mode));
        }
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::fs;
    use std::io;
    use std::path::Path;

    use super::{MAX_PADDING_CHARS, long_root, make_alpha_markers};

    /// The names the alpha installer leaves in a folder it installed or updated.
    const ALPHA_NAMES: [&str; 7] = [
        "installation.json",
        ".gdl-update.lock",
        ".gdl-update",
        ".gdl-installer-update",
        ".gdl-install.lock",
        "GauntletDarkLegacy-Update",
        "GauntletDarkLegacy-Update.exe",
    ];

    /// Those of them that are folders: the transaction folders of its game updates and of its own.
    const ALPHA_FOLDER_NAMES: [&str; 2] = [".gdl-update", ".gdl-installer-update"];

    /// A path's absolute length in UTF-16 units, as the launcher measures an install root.
    fn absolute_units(path: &Path) -> usize {
        let absolute = std::path::absolute(path).unwrap();
        #[cfg(windows)]
        let units = {
            use std::os::windows::ffi::OsStrExt;
            absolute.as_os_str().encode_wide().count()
        };
        #[cfg(not(windows))]
        let units = absolute.to_str().unwrap().encode_utf16().count();
        units
    }

    #[test]
    fn long_roots_measure_exactly() {
        let dir = tempfile::tempdir().unwrap();
        let base = std::path::absolute(dir.path()).unwrap();
        // A separator and one character.
        let shortest = absolute_units(&base) + 2;
        // Every length through three padding folders, then the launcher's 180-unit install root,
        // the lengths about MAX_PATH and one well past it.
        let mut targets: BTreeSet<usize> =
            (shortest..shortest + 3 * (MAX_PADDING_CHARS + 1) + 2).collect();
        targets.extend(
            [180, 259, 260, 261, 300]
                .into_iter()
                .filter(|target| *target >= shortest),
        );
        let mut roots = Vec::new();
        for &target in &targets {
            let root = long_root(&base, target).unwrap();
            assert_eq!(root, std::path::absolute(&root).unwrap());
            assert_eq!(absolute_units(&root), target, "{}", root.display());
            assert!(fs::symlink_metadata(&root).unwrap().is_dir(), "{target}");
            for component in root.strip_prefix(&base).unwrap() {
                let chars = component.len();
                assert!((1..=MAX_PADDING_CHARS).contains(&chars), "{target}");
            }
            roots.push(root);
        }
        // Roots of different lengths share no more than padding, so each is still empty.
        for root in &roots {
            let entries = fs::read_dir(root).unwrap().count();
            assert_eq!(entries, 0, "{}", root.display());
        }
        // A length made before names a root that exists, which is refused rather than shared.
        let error = long_root(&base, shortest).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        // No room for a separator and a character.
        for target in [0, shortest - 2, shortest - 1] {
            let error = long_root(&base, target).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{target}");
        }

        // A base that ends in a separator needs none before its first padding folder.
        let parent = base.join("separated");
        fs::create_dir(&parent).unwrap();
        let separated = parent.join("");
        let root = long_root(&separated, absolute_units(&separated) + 1).unwrap();
        assert_eq!(absolute_units(&root), absolute_units(&parent) + 2);
        assert_eq!(root.parent(), Some(parent.as_path()));
        let error = long_root(&separated, absolute_units(&separated)).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);

        // A missing base is made with the root.
        let missing = base.join("made").join("with spaces");
        let root = long_root(&missing, absolute_units(&missing) + 2).unwrap();
        assert!(missing.is_dir() && root.starts_with(&missing));

        // Lengths count UTF-16 units, whatever a path's bytes or characters: U+00E9 takes one
        // unit and U+1D11E two, so a separator and one character remain for the root.
        let unicode = base.join("\u{e9}\u{1d11e}");
        let root = long_root(&unicode, absolute_units(&base) + 6).unwrap();
        assert_eq!(absolute_units(&root), absolute_units(&base) + 6);
        assert_eq!(root.parent(), Some(unicode.as_path()));

        // A base whose length depends on how it is resolved is refused before anything is made.
        #[cfg(windows)]
        {
            let unmade = base.join("verbatim");
            let verbatim = format!(r"\\?\{}", unmade.display());
            let error = long_root(Path::new(&verbatim), 300).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
            assert!(!unmade.exists());
        }
        #[cfg(unix)]
        {
            let dotted = base.join("up").join("..").join("down");
            let error = long_root(&dotted, absolute_units(&dotted) + 2).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
            assert!(!base.join("up").exists());
        }
    }

    #[cfg(windows)]
    #[test]
    fn junctions_read_as_links() {
        use std::os::windows::fs::FileTypeExt;

        use super::make_junction;

        let dir = tempfile::tempdir().unwrap();
        // Characters that cmd reads specially outside quotes must reach mklink as they are, in a
        // path with a space and in one without, which argument quoting alone would leave bare.
        for folder in ["a b & (c) ^d", "x&y^z(1)"] {
            let place = dir.path().join(folder);
            let target = place.join("target");
            fs::create_dir_all(&target).unwrap();
            fs::write(target.join("file.txt"), b"through the junction").unwrap();
            let link = place.join("link");
            make_junction(&link, &target).unwrap();

            let kind = fs::symlink_metadata(&link).unwrap().file_type();
            assert!(
                kind.is_symlink() && kind.is_symlink_dir() && !kind.is_dir(),
                "{folder}"
            );
            assert!(fs::metadata(&link).unwrap().is_dir(), "{folder}");
            let pointed = fs::read_link(&link).unwrap();
            assert_eq!(pointed, std::path::absolute(&target).unwrap());
            let read = fs::read(link.join("file.txt")).unwrap();
            assert_eq!(read, b"through the junction");

            // A link that exists already is refused, as mklink refuses it.
            assert!(make_junction(&link, &target).is_err(), "{folder}");
            // Removing the junction leaves what it pointed to.
            fs::remove_dir(&link).unwrap();
            assert!(target.join("file.txt").is_file(), "{folder}");
        }

        // Paths that cmd would change are refused before anything runs.
        let target = dir.path().join("x&y^z(1)").join("target");
        let other = dir.path().join("other");
        for name in ["100%", "%TEMP%", "loud!", "quote\"d"] {
            let changed = dir.path().join(name);
            let error = make_junction(&changed, &target).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{name}");
            assert!(fs::symlink_metadata(&changed).is_err(), "{name}");
            let error = make_junction(&other, &changed).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{name}");
        }
        assert!(fs::symlink_metadata(&other).is_err());
    }

    #[test]
    fn alpha_markers_take_their_kinds() {
        for upper in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let names: Vec<String> = ALPHA_NAMES
                .iter()
                .map(|name| {
                    if upper {
                        name.to_ascii_uppercase()
                    } else {
                        (*name).to_owned()
                    }
                })
                .collect();
            let names: Vec<&str> = names.iter().map(String::as_str).collect();
            make_alpha_markers(dir.path(), &names).unwrap();

            let listed: BTreeSet<String> = fs::read_dir(dir.path())
                .unwrap()
                .map(|entry| entry.unwrap().file_name().into_string().unwrap())
                .collect();
            let expected: BTreeSet<String> = names.iter().map(|name| name.to_string()).collect();
            assert_eq!(listed, expected);
            for name in &names {
                let path = dir.path().join(name);
                let metadata = fs::symlink_metadata(&path).unwrap();
                if ALPHA_FOLDER_NAMES
                    .iter()
                    .any(|folder| folder.eq_ignore_ascii_case(name))
                {
                    assert!(metadata.is_dir(), "{name}");
                    assert_eq!(fs::read_dir(&path).unwrap().count(), 0, "{name}");
                } else {
                    assert!(metadata.is_file(), "{name}");
                    assert_eq!(metadata.len(), 0, "{name}");
                }
            }
            // A name that is there already is refused, a folder's and a file's alike.
            for name in [names[0], names[2]] {
                let error = make_alpha_markers(dir.path(), &[name]).unwrap_err();
                assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{name}");
            }
        }

        // Only single names, so nothing lands outside the folder, and nothing is made when one is
        // refused.
        let dir = tempfile::tempdir().unwrap();
        let outside = dir.path().join("outside");
        let outside = outside.to_str().unwrap();
        for name in ["", ".", "..", "a/b", "a/", "../escaped", outside] {
            let error = make_alpha_markers(dir.path(), &["installation.json", name]).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{name:?}");
        }
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 0);
    }

    #[cfg(unix)]
    #[test]
    fn denied_access_is_restored_on_drop() {
        use std::os::unix::fs::PermissionsExt;

        use super::DeniedAccess;

        let mode = |path: &Path| fs::metadata(path).unwrap().permissions().mode() & 0o7777;
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("file");
        let folder = dir.path().join("folder");
        let inner = folder.join("inner");
        fs::write(&file, b"kept").unwrap();
        fs::create_dir(&folder).unwrap();
        fs::write(&inner, b"inside").unwrap();
        // The folder's sticky bit shows that a mode comes back whole, not only its permissions.
        for (path, bits) in [(&file, 0o640), (&folder, 0o1750), (&inner, 0o604)] {
            fs::set_permissions(path, fs::Permissions::from_mode(bits)).unwrap();
        }

        let denied = DeniedAccess::deny(&[&file, &folder]).unwrap();
        assert_eq!(mode(&file), 0);
        assert_eq!(mode(&folder), 0);
        if fs::read_dir(&folder).is_ok() {
            eprintln!(
                "skipped the denial checks: this user lists folders of mode 0o000, as root does"
            );
        } else {
            let error = fs::read(&file).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
            let error = fs::read_dir(&folder).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
        }
        drop(denied);
        assert_eq!(mode(&file), 0o640);
        assert_eq!(mode(&folder), 0o1750);
        assert_eq!(fs::read(&file).unwrap(), b"kept");
        assert_eq!(fs::read_dir(&folder).unwrap().count(), 1);

        // What lies inside a folder is denied before it and given back after it.
        let nested = DeniedAccess::deny(&[&inner, &folder]).unwrap();
        assert_eq!(mode(&folder), 0);
        drop(nested);
        assert_eq!(mode(&folder), 0o1750);
        assert_eq!(mode(&inner), 0o604);
        assert_eq!(fs::read(&inner).unwrap(), b"inside");

        // An entry that cannot be denied gives back the modes already taken.
        let missing = dir.path().join("missing");
        let error = DeniedAccess::deny(&[&file, &missing]).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        assert_eq!(mode(&file), 0o640);

        // With every mode back, the folder is removed whole.
        dir.close().unwrap();
    }
}
