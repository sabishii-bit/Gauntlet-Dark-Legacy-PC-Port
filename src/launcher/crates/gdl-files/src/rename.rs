//! The renames that put what the launcher has staged in place: one that never replaces what is at
//! its target, and one that replaces a state file whole. Both are on disk before they return: on
//! Windows they are written through, and elsewhere the folders they change are synced. On Windows
//! they also reach past MAX_PATH and wait out a scanner's hold on a file.

use std::fs;
use std::io;
use std::path::Path;

#[cfg(windows)]
use windows_sys::Win32::Storage::FileSystem::{MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH};

use crate::retry::retry_locked;

/// Renames `from` to `to`, on one volume, without ever replacing what is at `to`: a file, a
/// folder, empty or not, or a link, even one to nothing. The caller holds the lock of the folder
/// they lie in: where a file system cannot refuse an existing target itself (on Linux, NFS, some
/// FUSE file systems and 9p), the target is first looked for and then renamed onto, and another
/// writer could come between the two.
///
/// The move is on disk before this returns. On Windows it is written through, and tried again as
/// [`retry_locked`] tries while a scanner holds the file, or a file inside the folder. Elsewhere
/// the folders it changes are synced once it is made, so an error from that sync comes after the
/// move. A target that exists is reported as [`io::ErrorKind::AlreadyExists`] without a system
/// error code, the same on every system, and is never waited for.
pub fn rename_noreplace(from: &Path, to: &Path) -> io::Result<()> {
    retry_locked(|| match rename_once(from, to) {
        Err(_) if is_present(to) => Err(target_exists()),
        result => result,
    })?;
    #[cfg(unix)]
    sync_renamed(from, to, &fs::File::sync_all)?;
    Ok(())
}

/// One attempt at a rename that never replaces its target.
#[cfg(windows)]
fn rename_once(from: &Path, to: &Path) -> io::Result<()> {
    crate::platform::move_file(from, to, MOVEFILE_WRITE_THROUGH)
}

/// One attempt at a rename that never replaces its target.
#[cfg(target_os = "linux")]
fn rename_once(from: &Path, to: &Path) -> io::Result<()> {
    rename_noreplace_with(crate::platform::renameat2_noreplace, from, to)
}

/// One attempt at a rename that never replaces its target.
#[cfg(all(unix, not(target_os = "linux")))]
fn rename_once(from: &Path, to: &Path) -> io::Result<()> {
    rename_checked(from, to)
}

/// Renames `from` to `to` with `primitive`, a rename that refuses an existing target itself, or
/// with [`rename_checked`] where that is not supported: renameat2 answers EINVAL on a file system
/// without RENAME_NOREPLACE, such as NFS, some FUSE file systems and 9p (WSL2's mounts of Windows
/// drives), and ENOSYS on a kernel without renameat2. Any other error is returned as it is.
#[cfg(target_os = "linux")]
pub(crate) fn rename_noreplace_with(
    primitive: impl Fn(&Path, &Path) -> io::Result<()>,
    from: &Path,
    to: &Path,
) -> io::Result<()> {
    match primitive(from, to) {
        Err(error) if matches!(error.raw_os_error(), Some(libc::EINVAL | libc::ENOSYS)) => {
            rename_checked(from, to)
        }
        result => result,
    }
}

/// Renames `from` to `to` only when nothing is at `to`, a link to nothing included, since `rename`
/// itself replaces a file, and an empty folder with a folder. The look and the rename are two
/// steps, which the caller's lock keeps other writers from coming between.
#[cfg(unix)]
pub(crate) fn rename_checked(from: &Path, to: &Path) -> io::Result<()> {
    match fs::symlink_metadata(to) {
        Ok(_) => Err(target_exists()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => fs::rename(from, to),
        Err(error) => Err(error),
    }
}

/// Replaces the file `to` with the file `from` in one step, so that a reader finds the old file or
/// the new one, never part of either; a missing `to` is made. Both are files. The caller has
/// written `from`, synced it through its own handle and closed it: on Windows a handle opened
/// again only to read cannot be flushed, so this never opens `from` itself.
///
/// The rename is on disk before this returns. On Windows it is written through, and tried again
/// as [`retry_locked`] tries while a scanner holds either file. Elsewhere the folders it changes
/// are synced once it is made, so an error from that sync comes after `to` is replaced.
pub fn replace(from: &Path, to: &Path) -> io::Result<()> {
    #[cfg(windows)]
    {
        retry_locked(|| {
            crate::platform::move_file(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        })
    }
    #[cfg(unix)]
    {
        fs::rename(from, to)?;
        sync_renamed(from, to, &fs::File::sync_all)
    }
}

/// The folder that holds `path`: its parent, or the working folder for a bare name.
pub(crate) fn folder_of(path: &Path) -> &Path {
    match path.parent() {
        Some(parent) if !parent.as_os_str().is_empty() => parent,
        _ => Path::new("."),
    }
}

/// How a folder, opened, is synced: with [`fs::File::sync_all`], unless a test stands in for it.
#[cfg(unix)]
type SyncFolder<'a> = &'a dyn Fn(&fs::File) -> io::Result<()>;

/// Syncs the folders that a rename of `from` to `to` changed: the folder `to` is now in, then the
/// one `from` left when that is another, so that neither the new name nor the old one's going is
/// lost to a crash. The first error stops it.
#[cfg(unix)]
fn sync_renamed(from: &Path, to: &Path, sync: SyncFolder<'_>) -> io::Result<()> {
    let target = folder_of(to);
    let source = folder_of(from);
    sync_folder(target, sync)?;
    if source != target {
        sync_folder(source, sync)?;
    }
    Ok(())
}

/// Opens `folder` and syncs it with `sync`. A folder whose file system cannot sync folders at all
/// counts as synced, since nothing more can be done for it there: Linux answers EINVAL where a file
/// system has no sync of folders, as VirtualBox's shared folders have none, and some systems EBADF
/// for a folder opened only to read.
#[cfg(unix)]
fn sync_folder(folder: &Path, sync: SyncFolder<'_>) -> io::Result<()> {
    match sync(&fs::File::open(folder)?) {
        Err(error) if matches!(error.raw_os_error(), Some(libc::EINVAL | libc::EBADF)) => Ok(()),
        result => result,
    }
}

/// Whether anything is at `path`, a link to nothing included.
fn is_present(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

/// The error of a rename whose target exists: a kind of its own and no system code, the same on
/// every system, so that no retry takes it for a passing hold.
fn target_exists() -> io::Error {
    io::Error::new(io::ErrorKind::AlreadyExists, "the rename's target exists")
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs::{self, File};
    use std::io::{self, Write};
    use std::path::{Path, PathBuf};
    use std::time::Instant;

    use super::{rename_noreplace, replace};
    use crate::{RETRY_WINDOW, root_utf16_len};

    /// MAX_PATH: the most UTF-16 units a plain Windows path may have, its NUL included.
    const MAX_PATH: usize = 260;

    /// How deep the long-path test's folder lies, in UTF-16 units: well past MAX_PATH.
    const DEEP_UNITS: usize = 300;

    /// The characters of each folder name that make up that depth.
    const DEEP_NAME_CHARS: usize = 50;

    /// ERROR_ACCESS_DENIED: what Windows answers a flush through a handle that may only read, and a
    /// rename of a folder while a file in it is open.
    #[cfg(windows)]
    const ACCESS_DENIED: i32 = 5;

    /// What is at a path, to compare before and after a rename.
    #[derive(Debug, PartialEq, Eq)]
    enum Entry {
        Missing,
        File(Vec<u8>),
        Folder(BTreeMap<String, Entry>),
        Link(PathBuf),
    }

    /// What is at `path`: a folder with everything inside it.
    fn snapshot(path: &Path) -> Entry {
        let metadata = match fs::symlink_metadata(path) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Entry::Missing,
            Err(error) => panic!("{}: {error}", path.display()),
        };
        if metadata.file_type().is_symlink() {
            Entry::Link(fs::read_link(path).unwrap())
        } else if metadata.is_dir() {
            let entries = fs::read_dir(path).unwrap().map(|entry| {
                let entry = entry.unwrap();
                let name = entry.file_name().into_string().unwrap();
                (name, snapshot(&entry.path()))
            });
            Entry::Folder(entries.collect())
        } else {
            Entry::File(fs::read(path).unwrap())
        }
    }

    /// Writes `bytes` to the new file `path` the way the launcher writes what it publishes: through
    /// its own handle, synced, then closed.
    fn write_synced(path: &Path, bytes: &[u8]) {
        let mut file = File::create_new(path).unwrap();
        file.write_all(bytes).unwrap();
        file.sync_all().unwrap();
    }

    /// Stages `path` as a folder holding a file when `folder` is set, else as a file.
    fn stage(path: &Path, folder: bool) {
        if folder {
            fs::create_dir(path).unwrap();
            write_synced(&path.join("config.json"), b"{}");
        } else {
            write_synced(path, b"staged");
        }
    }

    /// Puts something at the target `path`: an empty folder when `folder` is set, else a file.
    fn occupy(path: &Path, folder: bool) {
        if folder {
            fs::create_dir(path).unwrap();
        } else {
            write_synced(path, b"kept");
        }
    }

    /// Checks that `error` refuses a target that exists, and came without a wait.
    fn assert_refused(error: &io::Error, started: Instant) {
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{error}");
        assert_eq!(error.raw_os_error(), None, "{error}");
        assert!(started.elapsed() < RETRY_WINDOW, "{error}");
    }

    /// Renames `from` onto `to`, which holds something, and checks that the rename is refused and
    /// leaves both as they were.
    fn assert_rename_refused(from: &Path, to: &Path) {
        let before = (snapshot(from), snapshot(to));
        let started = Instant::now();
        let error = rename_noreplace(from, to).unwrap_err();
        assert_refused(&error, started);
        assert_eq!((snapshot(from), snapshot(to)), before);
    }

    #[test]
    fn refuses_an_existing_file() {
        for folder in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            stage(&from, folder);
            let to = dir.path().join("receipt.json");
            occupy(&to, false);
            assert_rename_refused(&from, &to);
        }
    }

    #[test]
    fn refuses_an_existing_empty_folder() {
        for folder in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            stage(&from, folder);
            let to = dir.path().join("data");
            occupy(&to, true);
            assert_rename_refused(&from, &to);
            // A folder that holds something is refused all the more.
            write_synced(&to.join("config.json"), b"kept");
            assert_rename_refused(&from, &to);
        }
    }

    #[cfg(unix)]
    #[test]
    fn refuses_a_link_even_one_to_nothing() {
        let dir = tempfile::tempdir().unwrap();
        let from = dir.path().join("staged");
        stage(&from, false);
        let folder = dir.path().join("folder");
        occupy(&folder, true);
        for (name, pointed) in [("dangling", dir.path().join("nowhere")), ("linked", folder)] {
            let to = dir.path().join(name);
            std::os::unix::fs::symlink(&pointed, &to).unwrap();
            assert_rename_refused(&from, &to);
        }
    }

    #[test]
    fn moves_a_folder_tree() {
        let dir = tempfile::tempdir().unwrap();
        let from = dir.path().join("install-AbC123xy");
        fs::create_dir_all(from.join("data").join("text")).unwrap();
        fs::create_dir(from.join("empty")).unwrap();
        write_synced(&from.join("VERSION"), b"1.0.0-beta\n");
        write_synced(&from.join("data").join("config.json"), b"{}");
        write_synced(
            &from.join("data").join("text").join("en.json"),
            b"{\"a\": 1}",
        );
        let tree = snapshot(&from);
        let to = dir.path().join("runtime");
        rename_noreplace(&from, &to).unwrap();
        assert_eq!(snapshot(&to), tree);
        assert_eq!(snapshot(&from), Entry::Missing);

        // A file moves the same way.
        let version = to.join("VERSION");
        let moved = dir.path().join("VERSION");
        rename_noreplace(&version, &moved).unwrap();
        assert_eq!(fs::read(&moved).unwrap(), b"1.0.0-beta\n");
        assert_eq!(snapshot(&version), Entry::Missing);

        // A missing source, or a target in a missing folder, is reported as missing, not as a
        // target that exists.
        let error = rename_noreplace(&version, &dir.path().join("elsewhere")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
        let nowhere = dir.path().join("missing").join("VERSION");
        let error = rename_noreplace(&moved, &nowhere).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
        assert_eq!(fs::read(&moved).unwrap(), b"1.0.0-beta\n");
    }

    #[test]
    fn renames_and_replaces_past_max_path() {
        let dir = tempfile::tempdir().unwrap();
        let mut deep = std::path::absolute(dir.path()).unwrap();
        while root_utf16_len(&deep).unwrap() < DEEP_UNITS {
            deep.push("d".repeat(DEEP_NAME_CHARS));
        }
        fs::create_dir_all(&deep).unwrap();
        let staged = deep.join("receipt.json.pending");
        let published = deep.join("receipt.json");
        assert!(root_utf16_len(&published).unwrap() > MAX_PATH);

        write_synced(&staged, b"first");
        rename_noreplace(&staged, &published).unwrap();
        assert_eq!(fs::read(&published).unwrap(), b"first");
        assert_eq!(snapshot(&staged), Entry::Missing);

        write_synced(&staged, b"second");
        assert_rename_refused(&staged, &published);
        replace(&staged, &published).unwrap();
        assert_eq!(fs::read(&published).unwrap(), b"second");
        assert_eq!(snapshot(&staged), Entry::Missing);

        // A folder moves there too, and is refused onto a folder that exists.
        let stage = deep.join("stage-n64-AbC123xy");
        fs::create_dir(&stage).unwrap();
        write_synced(&stage.join("disc-manifest.json"), b"{}");
        let tree = snapshot(&stage);
        let extracted = deep.join("extracted");
        rename_noreplace(&stage, &extracted).unwrap();
        assert_eq!(snapshot(&extracted), tree);
        occupy(&stage, true);
        assert_rename_refused(&extracted, &stage);
    }

    #[cfg(unix)]
    #[test]
    fn rename_checked_refuses_what_rename_would_replace() {
        use super::rename_checked;

        for folder in [false, true] {
            // rename itself replaces a file with a file, and an empty folder with a folder.
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            let to = dir.path().join("published");
            stage(&from, folder);
            occupy(&to, folder);
            let staged = snapshot(&from);
            fs::rename(&from, &to).unwrap();
            assert_eq!(snapshot(&to), staged);

            // rename_checked refuses either, and leaves both as they were.
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            let to = dir.path().join("published");
            stage(&from, folder);
            occupy(&to, folder);
            let before = (snapshot(&from), snapshot(&to));
            let error = rename_checked(&from, &to).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
            assert_eq!(error.raw_os_error(), None);
            assert_eq!((snapshot(&from), snapshot(&to)), before);

            // A target that is missing is renamed to.
            let free = dir.path().join("free");
            rename_checked(&from, &free).unwrap();
            assert_eq!(snapshot(&free), before.0);
            assert_eq!(snapshot(&from), Entry::Missing);
        }

        // A link counts as something, even one to nothing.
        let dir = tempfile::tempdir().unwrap();
        let from = dir.path().join("staged");
        stage(&from, false);
        let dangling = dir.path().join("dangling");
        std::os::unix::fs::symlink(dir.path().join("nowhere"), &dangling).unwrap();
        let error = rename_checked(&from, &dangling).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        assert!(fs::symlink_metadata(&dangling).unwrap().is_symlink());

        // An error in looking for the target is the answer, and nothing is renamed.
        let error = rename_checked(&from, &from.join("inside")).unwrap_err();
        assert_eq!(error.raw_os_error(), Some(libc::ENOTDIR), "{error}");
        assert_eq!(fs::read(&from).unwrap(), b"staged");
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn falls_back_when_the_flag_is_unsupported() {
        use std::cell::Cell;

        use super::rename_noreplace_with;

        // A file system without RENAME_NOREPLACE answers EINVAL, as NFS, some FUSE file systems
        // and 9p (WSL2's mounts of Windows drives) do; a kernel without renameat2 answers ENOSYS.
        for code in [libc::EINVAL, libc::ENOSYS] {
            let calls = Cell::new(0);
            let unsupported = |_: &Path, _: &Path| {
                calls.set(calls.get() + 1);
                Err(io::Error::from_raw_os_error(code))
            };
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            let to = dir.path().join("published");
            stage(&from, false);
            let staged = snapshot(&from);
            rename_noreplace_with(unsupported, &from, &to).unwrap();
            assert_eq!(calls.get(), 1, "{code}");
            assert_eq!(snapshot(&to), staged, "{code}");
            assert_eq!(snapshot(&from), Entry::Missing, "{code}");

            // The fallback still refuses a target that exists, a file or an empty folder.
            for folder in [false, true] {
                let from = dir.path().join(format!("staged-{folder}"));
                let to = dir.path().join(format!("taken-{folder}"));
                stage(&from, folder);
                occupy(&to, folder);
                let before = (snapshot(&from), snapshot(&to));
                let error = rename_noreplace_with(unsupported, &from, &to).unwrap_err();
                assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{code}");
                assert_eq!((snapshot(&from), snapshot(&to)), before, "{code}");
            }
            assert_eq!(calls.get(), 3, "{code}");
        }

        // Any other answer is the answer, and nothing moves: a denial, another device, a target
        // that exists, and an error that only looks like EINVAL, since it has no code.
        let errors: [fn() -> io::Error; 4] = [
            || io::Error::from_raw_os_error(libc::EACCES),
            || io::Error::from_raw_os_error(libc::EXDEV),
            || io::Error::from_raw_os_error(libc::EEXIST),
            || io::ErrorKind::InvalidInput.into(),
        ];
        for error in errors {
            let expected = error();
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            let to = dir.path().join("published");
            stage(&from, false);
            let returned = rename_noreplace_with(|_, _| Err(error()), &from, &to).unwrap_err();
            assert_eq!(
                returned.raw_os_error(),
                expected.raw_os_error(),
                "{expected}"
            );
            assert_eq!(returned.kind(), expected.kind(), "{expected}");
            assert_eq!(fs::read(&from).unwrap(), b"staged", "{expected}");
            assert_eq!(snapshot(&to), Entry::Missing, "{expected}");
        }

        // A rename that succeeds is the only one made.
        let calls = Cell::new(0);
        let dir = tempfile::tempdir().unwrap();
        let from = dir.path().join("staged");
        let to = dir.path().join("published");
        stage(&from, false);
        let succeeded = |_: &Path, _: &Path| {
            calls.set(calls.get() + 1);
            Ok(())
        };
        rename_noreplace_with(succeeded, &from, &to).unwrap();
        assert_eq!(calls.get(), 1);
        assert_eq!(fs::read(&from).unwrap(), b"staged");
        assert_eq!(snapshot(&to), Entry::Missing);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn renameat2_refuses_an_existing_target() {
        use crate::platform::renameat2_noreplace;

        let dir = tempfile::tempdir().unwrap();
        let from = dir.path().join("staged");
        let to = dir.path().join("published");
        stage(&from, false);
        occupy(&to, false);
        let before = (snapshot(&from), snapshot(&to));
        match renameat2_noreplace(&from, &to) {
            Err(error) if error.raw_os_error() == Some(libc::EINVAL) => {
                eprintln!(
                    "skipped: the file system under {} has no RENAME_NOREPLACE",
                    dir.path().display()
                );
                return;
            }
            Err(error) => assert_eq!(error.raw_os_error(), Some(libc::EEXIST), "{error}"),
            Ok(()) => panic!("renameat2 replaced {}", to.display()),
        }
        assert_eq!((snapshot(&from), snapshot(&to)), before);

        // An empty folder, which rename would replace with a folder, is refused too, and so is any
        // folder for a file.
        for folder in [false, true] {
            let from = dir.path().join(format!("staged-{folder}"));
            let to = dir.path().join(format!("empty-{folder}"));
            stage(&from, folder);
            occupy(&to, true);
            let before = (snapshot(&from), snapshot(&to));
            let error = renameat2_noreplace(&from, &to).unwrap_err();
            assert_eq!(error.raw_os_error(), Some(libc::EEXIST), "{error}");
            assert_eq!((snapshot(&from), snapshot(&to)), before);
        }

        // A target that is missing is renamed to.
        let free = dir.path().join("free");
        renameat2_noreplace(&from, &free).unwrap();
        assert_eq!(snapshot(&free), before.0);
        assert_eq!(snapshot(&from), Entry::Missing);

        // A path holding a NUL never reaches the kernel.
        for (from, to) in [
            (Path::new("a\0b"), free.as_path()),
            (&free, Path::new("c\0d")),
        ] {
            let error = renameat2_noreplace(from, to).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
            assert_eq!(error.raw_os_error(), None);
        }
        assert_eq!(snapshot(&free), before.0);
    }

    #[test]
    fn replace_replaces_a_file() {
        let dir = tempfile::tempdir().unwrap();
        let to = dir.path().join("receipt.json");
        let from = dir.path().join("receipt.json.pending");
        write_synced(&to, b"{\"old\": true}\n");
        write_synced(&from, b"{}\n");
        replace(&from, &to).unwrap();
        assert_eq!(fs::read(&to).unwrap(), b"{}\n");
        assert_eq!(snapshot(&from), Entry::Missing);

        // Again, with a longer file, and nothing else in the folder is touched.
        write_synced(&from, b"{\"longer\": [1, 2, 3]}\n");
        replace(&from, &to).unwrap();
        assert_eq!(fs::read(&to).unwrap(), b"{\"longer\": [1, 2, 3]}\n");
        let names: Vec<_> = fs::read_dir(dir.path())
            .unwrap()
            .map(|entry| entry.unwrap().file_name())
            .collect();
        assert_eq!(names, ["receipt.json"]);

        // A missing source replaces nothing.
        let error = replace(&from, &to).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
        assert_eq!(fs::read(&to).unwrap(), b"{\"longer\": [1, 2, 3]}\n");
    }

    #[test]
    fn replace_takes_a_closed_synced_file() {
        let dir = tempfile::tempdir().unwrap();
        let from = dir.path().join("receipt.json.pending");
        write_synced(&from, b"{}\n");
        // A handle opened again only to read cannot be flushed on Windows, which is why replace
        // takes the file as its writer synced it and never opens it itself.
        #[cfg(windows)]
        {
            let error = File::open(&from).unwrap().sync_all().unwrap_err();
            assert_eq!(error.raw_os_error(), Some(ACCESS_DENIED), "{error}");
        }
        // A missing target is made.
        let to = dir.path().join("receipt.json");
        replace(&from, &to).unwrap();
        assert_eq!(fs::read(&to).unwrap(), b"{}\n");
        assert_eq!(snapshot(&from), Entry::Missing);
    }

    #[test]
    fn folder_of_is_the_parent_or_the_working_folder() {
        use super::folder_of;

        for (path, folder) in [
            (
                "/install/.gdl-launcher/receipt.json",
                "/install/.gdl-launcher",
            ),
            ("state/receipt.json", "state"),
            ("receipt.json", "."),
            ("./receipt.json", "."),
        ] {
            assert_eq!(folder_of(Path::new(path)), Path::new(folder), "{path}");
        }
    }

    #[cfg(unix)]
    #[test]
    fn renames_sync_the_folders_they_change() {
        use std::cell::RefCell;
        use std::os::unix::fs::MetadataExt;

        use super::sync_renamed;

        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("install-AbC123xy");
        let target = dir.path().join("install");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&target).unwrap();
        let inode = |path: &Path| fs::metadata(path).unwrap().ino();
        let (source_inode, target_inode) = (inode(&source), inode(&target));

        // The folder renamed into, then the one renamed out of, each the folder itself and never
        // the file; a folder both lie in is synced once.
        let synced = RefCell::new(Vec::new());
        let record = |folder: &File| {
            synced.borrow_mut().push(folder.metadata()?.ino());
            folder.sync_all()
        };
        sync_renamed(&source.join("data"), &target.join("data"), &record).unwrap();
        assert_eq!(*synced.borrow(), [target_inode, source_inode]);
        synced.borrow_mut().clear();
        let pending = target.join("receipt.json.pending");
        sync_renamed(&pending, &target.join("receipt.json"), &record).unwrap();
        assert_eq!(*synced.borrow(), [target_inode]);

        // A file system that cannot sync folders answers EINVAL, or EBADF on some systems, and
        // its folders count as synced. Any other answer is the answer, and stops the syncs.
        for (code, refused) in [
            (libc::EINVAL, false),
            (libc::EBADF, false),
            (libc::EIO, true),
            (libc::ENOSPC, true),
        ] {
            synced.borrow_mut().clear();
            let answer = |folder: &File| {
                synced.borrow_mut().push(folder.metadata()?.ino());
                Err(io::Error::from_raw_os_error(code))
            };
            let result = sync_renamed(&source.join("data"), &target.join("data"), &answer);
            if refused {
                assert_eq!(result.unwrap_err().raw_os_error(), Some(code));
                assert_eq!(*synced.borrow(), [target_inode], "{code}");
            } else {
                result.unwrap();
                assert_eq!(*synced.borrow(), [target_inode, source_inode], "{code}");
            }
        }
    }

    /// Gives a folder back its owner's whole access when dropped, before the `TempDir` above it
    /// goes, which could not remove a folder that it may not read.
    #[cfg(unix)]
    struct AccessRestored<'a>(&'a Path);

    #[cfg(unix)]
    impl Drop for AccessRestored<'_> {
        fn drop(&mut self) {
            use std::os::unix::fs::PermissionsExt;
            let _ = fs::set_permissions(self.0, fs::Permissions::from_mode(0o700));
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_folder_that_cannot_be_synced_is_reported_after_the_rename() {
        use std::os::unix::fs::PermissionsExt;

        /// Writing and searching but no reading: names can be made and removed in such a folder,
        /// but it cannot be opened to be synced.
        const WRITE_AND_SEARCH: u32 = 0o300;

        let dir = tempfile::tempdir().unwrap();
        let locked = dir.path().join("locked");
        fs::create_dir(&locked).unwrap();
        let receipt = locked.join("receipt.json");
        let pending = locked.join("receipt.json.pending");
        write_synced(&receipt, b"old");
        write_synced(&pending, b"new");
        let staged = dir.path().join("staged");
        write_synced(&staged, b"staged");
        let _restored = AccessRestored(&locked);
        fs::set_permissions(&locked, fs::Permissions::from_mode(WRITE_AND_SEARCH)).unwrap();
        if File::open(&locked).is_ok() {
            eprintln!("skipped: this user opens folders it may not read, as root does");
            return;
        }

        // Into the folder: the move is made, and then its sync is refused.
        let moved = locked.join("staged");
        let error = rename_noreplace(&staged, &moved).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied, "{error}");
        assert_eq!(snapshot(&staged), Entry::Missing);
        assert_eq!(fs::read(&moved).unwrap(), b"staged");

        // Out of it, which is synced too, after the folder moved into.
        let back = dir.path().join("back");
        let error = rename_noreplace(&moved, &back).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied, "{error}");
        assert_eq!(snapshot(&moved), Entry::Missing);
        assert_eq!(fs::read(&back).unwrap(), b"staged");

        // A state file replaced in it: the new file is in place when the error comes.
        let error = replace(&pending, &receipt).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied, "{error}");
        assert_eq!(snapshot(&pending), Entry::Missing);
        assert_eq!(fs::read(&receipt).unwrap(), b"new");
    }

    /// Holds on files, which make Windows refuse a rename until they close.
    #[cfg(windows)]
    mod holds {
        use std::fs::{File, OpenOptions};
        use std::os::windows::fs::OpenOptionsExt;
        use std::path::Path;
        use std::thread;
        use std::time::{Duration, Instant};

        use windows_sys::Win32::Storage::FileSystem::MOVEFILE_WRITE_THROUGH;

        use super::{ACCESS_DENIED, assert_refused, occupy, snapshot, stage};
        use crate::{rename_noreplace, replace};

        /// ERROR_SHARING_VIOLATION: what Windows answers a rename of a file while a handle on it
        /// does not share its deletion.
        const SHARING_VIOLATION: i32 = 32;

        /// How long a test's stand-in for a scanner holds its file: a few retries' worth.
        const HOLD: Duration = Duration::from_millis(300);

        /// A handle on `path` that shares nothing, as a scanner's may, taken once any real scanner
        /// of the file just written has let it go.
        fn hold(path: &Path) -> File {
            crate::retry_locked(|| OpenOptions::new().read(true).share_mode(0).open(path)).unwrap()
        }

        /// Closes `handle` on a thread of its own once HOLD has passed.
        fn close_later(handle: File) -> thread::JoinHandle<()> {
            thread::spawn(move || {
                thread::sleep(HOLD);
                drop(handle);
            })
        }

        #[test]
        fn a_rename_waits_out_a_hold_on_its_file() {
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            let to = dir.path().join("published");
            stage(&from, false);
            let started = Instant::now();
            let closer = close_later(hold(&from));
            rename_noreplace(&from, &to).unwrap();
            assert!(started.elapsed() >= HOLD);
            closer.join().unwrap();
            assert_eq!(std::fs::read(&to).unwrap(), b"staged");
        }

        #[test]
        fn a_replace_waits_out_a_hold_on_either_file() {
            for held_target in [false, true] {
                let dir = tempfile::tempdir().unwrap();
                let from = dir.path().join("receipt.json.pending");
                let to = dir.path().join("receipt.json");
                stage(&from, false);
                occupy(&to, false);
                let held = if held_target { &to } else { &from };
                let started = Instant::now();
                let closer = close_later(hold(held));
                replace(&from, &to).unwrap();
                assert!(started.elapsed() >= HOLD, "{}", held.display());
                closer.join().unwrap();
                assert_eq!(std::fs::read(&to).unwrap(), b"staged");
            }
        }

        #[test]
        fn an_existing_target_is_never_waited_for() {
            // A held file onto a file: Windows answers with a sharing violation, which alone
            // would be waited out.
            let dir = tempfile::tempdir().unwrap();
            let from = dir.path().join("staged");
            let to = dir.path().join("published");
            stage(&from, false);
            occupy(&to, false);
            let before = (snapshot(&from), snapshot(&to));
            let handle = hold(&from);
            let answer = crate::platform::move_file(&from, &to, MOVEFILE_WRITE_THROUGH);
            assert_eq!(answer.unwrap_err().raw_os_error(), Some(SHARING_VIOLATION));
            let started = Instant::now();
            assert_refused(&rename_noreplace(&from, &to).unwrap_err(), started);
            drop(handle);
            assert_eq!((snapshot(&from), snapshot(&to)), before);

            // A folder with an open file inside onto a folder: Windows answers that access is
            // denied, which alone would be waited out too.
            let from = dir.path().join("stage");
            let to = dir.path().join("data");
            stage(&from, true);
            occupy(&to, true);
            let before = (snapshot(&from), snapshot(&to));
            let handle = File::open(from.join("config.json")).unwrap();
            let answer = crate::platform::move_file(&from, &to, MOVEFILE_WRITE_THROUGH);
            assert_eq!(answer.unwrap_err().raw_os_error(), Some(ACCESS_DENIED));
            let started = Instant::now();
            assert_refused(&rename_noreplace(&from, &to).unwrap_err(), started);
            drop(handle);
            assert_eq!((snapshot(&from), snapshot(&to)), before);
        }
    }
}
