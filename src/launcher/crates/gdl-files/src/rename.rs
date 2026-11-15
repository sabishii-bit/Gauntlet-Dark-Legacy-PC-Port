//! The renames that put what the launcher has staged in place: one that never replaces what is at
//! its target, and one that replaces a state file whole. On Windows both are written through to
//! disk, reach past MAX_PATH and wait out a scanner's hold on a file.

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
/// On Windows the move is on disk before this returns, and it is tried again as [`retry_locked`]
/// tries while a scanner holds the file, or a file inside the folder. A target that exists is
/// reported as [`io::ErrorKind::AlreadyExists`] without a system error code, the same on every
/// system, and is never waited for.
pub fn rename_noreplace(from: &Path, to: &Path) -> io::Result<()> {
    retry_locked(|| match rename_once(from, to) {
        Err(_) if is_present(to) => Err(target_exists()),
        result => result,
    })
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
/// On Windows the move is on disk before this returns, and it is tried again as [`retry_locked`]
/// tries while a scanner holds either file. On Linux the folder is synced after the rename, so
/// that the rename is on disk too; an error from that sync comes after the rename is made.
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
        fs::File::open(folder_of(to))?.sync_all()
    }
}

/// The folder that holds `path`: its parent, or the working folder for a bare name.
#[cfg(unix)]
fn folder_of(path: &Path) -> &Path {
    match path.parent() {
        Some(parent) if !parent.as_os_str().is_empty() => parent,
        _ => Path::new("."),
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

    #[cfg(unix)]
    #[test]
    fn replace_syncs_the_folder_that_holds_the_target() {
        use super::folder_of;

        for (target, folder) in [
            (
                "/install/.gdl-launcher/receipt.json",
                "/install/.gdl-launcher",
            ),
            ("state/receipt.json", "state"),
            ("receipt.json", "."),
            ("./receipt.json", "."),
        ] {
            assert_eq!(folder_of(Path::new(target)), Path::new(folder), "{target}");
        }
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

        /// A handle on `path` that shares nothing, as a scanner's may.
        fn hold(path: &Path) -> File {
            OpenOptions::new()
                .read(true)
                .share_mode(0)
                .open(path)
                .unwrap()
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
