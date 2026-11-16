//! Links on the way to what the launcher changes. A link, a junction or a mount point in a path
//! would let the launcher's writes land outside the folder it was given, so every folder above
//! a path it changes must be a real one.

use std::borrow::Cow;
use std::ffi::{OsStr, OsString};
use std::fs;
use std::io;
use std::path::{Component, MAIN_SEPARATOR_STR, Path, PathBuf};

use crate::FilesError;

/// Makes `path` absolute, then folds its `.` and `..` by name alone, as the release scripts'
/// installer folds a destination it is given: a `..` takes away the name before it, and above a
/// root it stays at the root. Links are not looked at, so a `..` after a link is taken back out
/// of the link rather than to the folder above its target; the launcher then refuses links on the
/// path folded.
///
/// A trailing separator is dropped. A path that begins with exactly two slashes keeps them, as
/// std's own absolute paths do, since POSIX lets a system give such a path a meaning of its own.
pub fn absolute_lexical(path: &Path) -> io::Result<PathBuf> {
    let absolute = std::path::absolute(path)?;
    let mut root = PathBuf::new();
    let mut names = Vec::new();
    for component in absolute.components() {
        match component {
            Component::Prefix(_) | Component::RootDir => root.push(component),
            Component::CurDir => {}
            Component::ParentDir => {
                names.pop();
            }
            Component::Normal(name) => names.push(name),
        }
    }
    let mut folded = if begins_with_two_slashes(&absolute) {
        OsString::from(TWO_SLASHES)
    } else {
        root.into_os_string()
    };
    // Joined as text, not pushed: a push reads each name as a path again, and a name that reads
    // as a drive, such as `C:`, would take the place of everything before it.
    for name in names {
        if !ends_in_separator(&folded) {
            folded.push(MAIN_SEPARATOR_STR);
        }
        folded.push(name);
    }
    Ok(PathBuf::from(folded))
}

/// The start of a path that POSIX leaves to each system to read: two slashes.
const TWO_SLASHES: &str = "//";

/// Whether `absolute`, a path std made absolute, begins with two slashes, which the root that its
/// components give would turn into one. std keeps two only where a path began with exactly two,
/// and turns three or more into one. Only Unix reads such a path so; a Windows share begins with
/// two backslashes and keeps its own prefix.
fn begins_with_two_slashes(absolute: &Path) -> bool {
    cfg!(unix)
        && absolute
            .as_os_str()
            .as_encoded_bytes()
            .starts_with(TWO_SLASHES.as_bytes())
}

/// Whether `path` ends in a separator, as a root does, so that a name joins it without another.
fn ends_in_separator(path: &OsStr) -> bool {
    path.as_encoded_bytes()
        .last()
        .is_some_and(|&byte| std::path::is_separator(char::from(byte)))
}

/// Refuses `path` when it, or any folder above it that exists, is a link: a symbolic link, or on
/// Windows a junction or a mount point, which are the reparse points whose tag marks them as
/// standing for another name. A cloud file's placeholder is no such link and passes. A relative
/// path is taken from the working folder, whose own folders count as folders above it.
///
/// The path is checked as it is written, every name in it, even one that a `..` then steps back
/// out of. A name that does not exist yet, as [`is_absent`] tells, is passed over, so a path can
/// be checked before it is made. A folder above the path that the system will not let this
/// process look into is reported as an error, since nothing then shows that it is free of links.
pub fn no_links(path: &Path) -> Result<(), FilesError> {
    let walked = walked(path).map_err(|source| FilesError::io(path, source))?;
    // Without a trailing separator, which would have the system look through a link the path
    // ends in instead of at the link itself.
    for part in walked.components().as_path().ancestors() {
        match fs::symlink_metadata(part) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Err(FilesError::Link {
                    path: part.to_path_buf(),
                });
            }
            Ok(_) => {}
            Err(error) if is_absent(&error) => {}
            Err(source) => return Err(FilesError::io(part, source)),
        }
    }
    Ok(())
}

/// The path a file call on `path` walks: `path` itself when it is absolute, else `path` under
/// the working folder, made absolute as std makes it, which on Unix keeps each `..` for the
/// system to resolve as it walks.
fn walked(path: &Path) -> io::Result<Cow<'_, Path>> {
    if path.is_absolute() {
        Ok(Cow::Borrowed(path))
    } else {
        std::path::absolute(path).map(Cow::Owned)
    }
}

/// Whether an error looking at a path says that nothing is there: the path is missing, or a
/// folder above it is a file. [`no_links`] passes over what this calls absent, and code that looks
/// at a path after checking it uses the same rule, so that both agree on what is there.
pub fn is_absent(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::NotFound | io::ErrorKind::NotADirectory
    )
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};

    use gdl_fixtures::symlink_file;

    use super::{absolute_lexical, is_absent, no_links};
    use crate::FilesError;

    /// The separator a path is joined with on this system.
    const SEPARATOR: char = std::path::MAIN_SEPARATOR;

    /// `path` with a separator after it.
    fn trailing(path: &Path) -> PathBuf {
        let mut text = path.as_os_str().to_owned();
        text.push(SEPARATOR.to_string());
        PathBuf::from(text)
    }

    /// Checks that `no_links` refuses `path` for the link at `link`, named as it is written:
    /// paths compare by their components, which a separator at the end leaves alike.
    fn assert_link(path: &Path, link: &Path) {
        match no_links(path) {
            Err(FilesError::Link { path: found }) => {
                assert_eq!(found.as_os_str(), link.as_os_str(), "{}", path.display());
            }
            other => panic!("{}: {other:?}", path.display()),
        }
    }

    /// A path through `link` that steps back out of it, which is checked as it is written: every
    /// name in it, as the release scripts check a path.
    fn stepping_back_out(link: &Path) -> PathBuf {
        link.join("..").join("real")
    }

    /// A folder `real` holding `sub/receipt.json`, under `dir`.
    fn plain_tree(dir: &Path) -> PathBuf {
        let real = dir.join("real");
        fs::create_dir_all(real.join("sub")).unwrap();
        fs::write(real.join("sub").join("receipt.json"), b"{}").unwrap();
        real
    }

    /// The paths through `link`, a link to a [`plain_tree`], that are refused for it.
    fn paths_through(link: &Path) -> Vec<PathBuf> {
        vec![
            link.to_path_buf(),
            trailing(link),
            link.join("sub"),
            link.join("sub").join("receipt.json"),
            link.join("missing").join("deeper.json"),
        ]
    }

    #[cfg(windows)]
    #[test]
    fn refuses_a_junction_on_the_path() {
        let dir = tempfile::tempdir().unwrap();
        let real = plain_tree(dir.path());
        let link = dir.path().join("link");
        gdl_fixtures::make_junction(&link, &real).unwrap();
        for path in paths_through(&link) {
            assert_link(&path, &link);
        }
        // Windows folds the `..` away and never passes through the junction here, yet the path
        // is refused for the name it holds.
        assert_link(&stepping_back_out(&link), &link);

        // In the verbatim form that long paths take, the junction is found by that form.
        let verbatim = PathBuf::from(format!(r"\\?\{}", link.display()));
        for path in paths_through(&verbatim) {
            assert_link(&path, &verbatim);
        }

        // What the junction leads to is fine reached directly.
        for path in [real.join("sub"), real.join("sub").join("receipt.json")] {
            no_links(&path).unwrap();
        }
    }

    #[test]
    fn refuses_a_symbolic_link_ancestor() {
        let dir = tempfile::tempdir().unwrap();
        let real = plain_tree(dir.path());
        let link = dir.path().join("link");
        if let Err(error) = symlink_folder(&real, &link) {
            eprintln!("skipped: this account cannot make symbolic links ({error})");
            return;
        }
        for path in paths_through(&link) {
            assert_link(&path, &link);
        }
        assert_link(&stepping_back_out(&link), &link);

        // A link to a file, and one to nothing, are refused as links too.
        let file_link = dir.path().join("receipt.json");
        symlink_file(&file_link, &real.join("sub").join("receipt.json")).unwrap();
        assert_link(&file_link, &file_link);
        let dangling = dir.path().join("dangling.json");
        symlink_file(&dangling, &dir.path().join("nowhere.json")).unwrap();
        assert_link(&dangling, &dangling);

        // Unix resolves a `..` after a link through the link's target, so the link is still
        // passed through and still refused.
        #[cfg(unix)]
        {
            assert_link(&link.join("sub").join(".."), &link);
            assert_link(&link.join(".").join("sub"), &link);
        }

        no_links(&real.join("sub").join("receipt.json")).unwrap();
    }

    /// Makes `link` a symbolic link to the folder `target`.
    fn symlink_folder(target: &Path, link: &Path) -> io::Result<()> {
        #[cfg(windows)]
        return std::os::windows::fs::symlink_dir(target, link);
        #[cfg(unix)]
        return std::os::unix::fs::symlink(target, link);
    }

    #[test]
    fn accepts_a_plain_tree() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("install");
        let state = root.join(".gdl-launcher");
        fs::create_dir_all(&state).unwrap();
        let receipt = state.join("receipt.json");
        fs::write(&receipt, b"{}").unwrap();
        for path in [
            dir.path().to_path_buf(),
            root.clone(),
            trailing(&root),
            state.clone(),
            receipt.clone(),
            // Names not made yet, at any depth.
            state.join("journal"),
            root.join("n64").join("extracted").join("manifest.json"),
            // Dots, which each system resolves its own way.
            root.join(".").join(".gdl-launcher"),
            root.join("saves").join("..").join(".gdl-launcher"),
            // Nothing can be under a file, so nothing there is a link.
            receipt.join("inside.json"),
        ] {
            no_links(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()));
        }
        #[cfg(windows)]
        {
            let verbatim = PathBuf::from(format!(r"\\?\{}", receipt.display()));
            no_links(&verbatim).unwrap();
        }

        // An empty path names nothing to check, and is refused rather than passed.
        match no_links(Path::new("")) {
            Err(FilesError::Io { path, source }) => {
                assert_eq!(path, Path::new(""));
                assert_eq!(source.kind(), io::ErrorKind::InvalidInput);
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn nothing_is_there_when_the_name_is_missing_or_a_file_stands_above_it() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("receipt.json");
        fs::write(&file, b"{}").unwrap();
        for path in [
            dir.path().join("missing"),
            dir.path().join("missing").join("deeper.json"),
            file.join("inside.json"),
        ] {
            let error = fs::symlink_metadata(&path).unwrap_err();
            assert!(is_absent(&error), "{}: {error}", path.display());
            no_links(&path).unwrap();
        }
        // A refusal to look shows nothing, and is no absence.
        for kind in [io::ErrorKind::PermissionDenied, io::ErrorKind::InvalidInput] {
            assert!(!is_absent(&io::Error::from(kind)), "{kind:?}");
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_folder_that_cannot_be_searched_is_reported() {
        let dir = tempfile::tempdir().unwrap();
        let locked = dir.path().join("locked");
        fs::create_dir(&locked).unwrap();
        let receipt = locked.join("receipt.json");
        fs::write(&receipt, b"{}").unwrap();
        let denied = gdl_fixtures::DeniedAccess::deny(&[&locked]).unwrap();
        if fs::symlink_metadata(&receipt).is_ok() {
            eprintln!("skipped: this user looks into folders of mode 0o000, as root does");
            return;
        }
        // Whether the file is a link cannot be seen, so it is not taken to be none.
        match no_links(&receipt) {
            Err(FilesError::Io { path, source }) => {
                assert_eq!(path, receipt);
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        // The folder itself is seen from the folder above it.
        no_links(&locked).unwrap();
        drop(denied);
    }

    /// Checks that `absolute_lexical` folds `given` to `folded`, compared as text: paths compare
    /// by their components, which leave a separator at the end, or a second slash at the start,
    /// unseen.
    fn assert_folds(given: &str, folded: &Path) {
        let found = absolute_lexical(Path::new(given)).unwrap();
        assert_eq!(found.as_os_str(), folded.as_os_str(), "{given}");
    }

    #[test]
    fn absolute_lexical_folds_dots() {
        let working = std::env::current_dir().unwrap();
        assert_folds("a/./b/../c", &working.join("a").join("c"));
        assert_folds("a/b/", &working.join("a").join("b"));
        // Above every folder of the working folder, a `..` stays at its root.
        let climb = vec![".."; working.components().count() + 2].join("/");
        let root = working.ancestors().last().unwrap();
        assert_folds(&format!("{climb}/x"), &root.join("x"));

        #[cfg(unix)]
        for (given, folded) in [
            ("/a/b/../c", "/a/c"),
            ("/../a", "/a"),
            ("/..", "/"),
            ("/a/b/../../../c", "/c"),
            ("/a/b/..", "/a"),
            ("/a/./b/", "/a/b"),
            ("/", "/"),
            ("///a", "/a"),
            ("//a/../b", "//b"),
            ("//a/..", "//"),
            ("/a/\u{e9}/../\u{1d11e}", "/a/\u{1d11e}"),
        ] {
            assert_folds(given, Path::new(folded));
        }
        #[cfg(windows)]
        for (given, folded) in [
            (r"C:\a\..\b", r"C:\b"),
            (r"C:\..\a", r"C:\a"),
            (r"C:\a\b\", r"C:\a\b"),
            (r"C:/a/./b", r"C:\a\b"),
            (r"C:\", r"C:\"),
            (r"\\srv\share\a\..\b", r"\\srv\share\b"),
            // Windows leaves a verbatim path's dots to the folding, which treats them alike.
            (r"\\?\C:\a\..\b", r"\\?\C:\b"),
            (r"\\?\C:\a\.\b\", r"\\?\C:\a\b"),
            (r"\\?\C:\..", r"\\?\C:\"),
            (r"\\?\UNC\srv\share\a\..\b", r"\\?\UNC\srv\share\b"),
            // A name that reads as a drive's stays a name.
            (r"D:\games\C:\x", r"D:\games\C:\x"),
        ] {
            assert_folds(given, Path::new(folded));
        }

        // An empty path has no absolute form.
        let error = absolute_lexical(Path::new("")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
    }
}
