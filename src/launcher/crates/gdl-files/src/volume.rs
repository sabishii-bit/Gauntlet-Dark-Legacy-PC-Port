//! What a folder and its volume allow before anything is written there: the nearest folder on a
//! path that exists, the room its volume still has for this process, whether its file system runs
//! programs, and whether the folder takes a new file and tells letter case apart, which a file of
//! the launcher's own, made there and removed at once, shows.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use crate::{FilesError, STAGING_RANDOM_CHARS, is_absent, retry_locked};

/// The prefix of the file a folder probe makes and removes: a name of the launcher's own.
pub const PROBE_PREFIX: &str = ".gdl-launcher-probe-";

/// What a folder's volume allows: the room it still has for this process, and whether programs
/// run from it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Volume {
    /// The bytes this process may still write there: on Windows what the volume allows this
    /// account, its disk quota included, and on Linux what the free blocks that a user without
    /// privilege may fill hold.
    pub available: u64,
    /// Whether the file system is mounted so that no program on it runs, as Linux's `noexec`
    /// mounts it. Always `false` on Windows.
    pub noexec: bool,
}

/// What making a file in a folder showed: whether the folder takes new files, and whether its
/// file system ignores letter case.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FolderProbe {
    /// Whether a new file could be made there. One refused for want of permission, or by a file
    /// system that is read-only, is not.
    pub writable: bool,
    /// Whether the file made could also be found by its name in upper case, as a file system that
    /// ignores letter case finds it. Always `false` when no file could be made.
    pub ignores_case: bool,
}

/// `path` itself when it is there, or else the nearest folder above it that is, looked at without
/// following a link the path ends in: the first that is there is returned whatever it is, a file
/// or a link included, for the caller to judge. Names that are not there, as [`is_absent`] tells,
/// are passed over; any other failure to look at one is returned. A relative path is taken from
/// the working folder. When nothing on the path is there, its root included, as on a drive that
/// does not exist, the error is [`io::ErrorKind::NotFound`].
pub fn nearest_existing(path: &Path) -> io::Result<PathBuf> {
    let walked = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::path::absolute(path)?
    };
    // Without a trailing separator, which would have the system look through a link the path
    // ends in instead of at the link itself.
    for part in walked.components().as_path().ancestors() {
        match fs::symlink_metadata(part) {
            Ok(_) => return Ok(part.to_path_buf()),
            Err(error) if is_absent(&error) => {}
            Err(error) => return Err(error),
        }
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!("nothing on the path {} exists", walked.display()),
    ))
}

/// What the volume that holds `folder` allows: the bytes this process may still write there and,
/// on Linux, whether its file system runs programs. The folder must exist.
pub fn volume_of(folder: &Path) -> io::Result<Volume> {
    #[cfg(windows)]
    {
        Ok(Volume {
            available: crate::platform::available_to_caller(folder)?,
            noexec: false,
        })
    }
    #[cfg(target_os = "linux")]
    {
        crate::platform::file_system_of(folder)
    }
    #[cfg(not(any(windows, target_os = "linux")))]
    {
        let _ = folder;
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "this system's volumes are not measured",
        ))
    }
}

/// Whether `error` is the system's refusal to write: for want of permission (Windows' access
/// denied, Unix's EACCES and EPERM), or because the file system is read-only (Unix's EROFS,
/// Windows' write protection). A folder probe takes such a refusal for a folder that is not
/// writable, where any other failure is an error.
pub fn refuses_writes(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::PermissionDenied | io::ErrorKind::ReadOnlyFilesystem
    )
}

/// Makes a new file in `folder`, named [`PROBE_PREFIX`] and then [`STAGING_RANDOM_CHARS`] random
/// ASCII letters and digits, closes it, looks for it by its name in ASCII upper case, and removes
/// it, so that a folder is judged before a job writes into it. A creation that
/// [`refuses_writes`] tells is a refusal gives a folder that is not writable; any other failure
/// is an error at `folder`.
///
/// The creation is tried once: a refusal is the folder's answer, not a passing hold. The removal
/// is tried again as [`retry_locked`] tries, since a scanner may open a file the moment it is
/// closed; a removal that still fails is an error naming the file, which is then left.
pub fn probe_folder(folder: &Path) -> Result<FolderProbe, FilesError> {
    // Made without cleanup, so that nothing removes the file but the removal below, which is
    // retried: tempfile's own way of keeping a file it would remove hands Windows a plain path,
    // which fails past MAX_PATH.
    let made = tempfile::Builder::new()
        .prefix(PROBE_PREFIX)
        .rand_bytes(STAGING_RANDOM_CHARS)
        .disable_cleanup(true)
        .tempfile_in(folder);
    let probe = match made {
        Ok(probe) => probe,
        Err(error) if refuses_writes(&error) => {
            return Ok(FolderProbe {
                writable: false,
                ignores_case: false,
            });
        }
        Err(source) => return Err(FilesError::io(folder, source)),
    };
    // Closed at once, so that the removal takes the name away: Windows keeps the name of a file
    // removed while a handle on it is open until the handle closes.
    let (file, path) = probe.into_parts();
    drop(file);
    let path = path.to_path_buf();
    // The prefix holds lower-case letters, so the name in upper case is always another spelling.
    let upper = path.with_file_name(path.file_name().unwrap_or_default().to_ascii_uppercase());
    let looked = fs::symlink_metadata(&upper);
    retry_locked(|| fs::remove_file(&path)).map_err(|source| FilesError::io(&path, source))?;
    let ignores_case = match looked {
        Ok(_) => true,
        Err(error) if is_absent(&error) => false,
        Err(source) => return Err(FilesError::io(&upper, source)),
    };
    Ok(FolderProbe {
        writable: true,
        ignores_case,
    })
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};

    use gdl_fixtures::{deny_writes, folder_names, link_folder};

    use super::{FolderProbe, nearest_existing, probe_folder, refuses_writes, volume_of};
    use crate::error::assert_io;

    #[test]
    fn the_nearest_existing_folder_is_found_and_a_missing_root_is_not_found() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();

        // A folder that is there is its own nearest, and one below it that is not finds it.
        assert_eq!(nearest_existing(top).unwrap(), top);
        assert_eq!(
            nearest_existing(&top.join("game").join("deeper").join("deepest")).unwrap(),
            top
        );

        // What is there is returned whatever it is, for the caller to judge: a file, and a link,
        // which is looked at and not through.
        let file = top.join("file");
        fs::write(&file, b"file").unwrap();
        assert_eq!(nearest_existing(&file.join("game")).unwrap(), file);
        let real = top.join("real");
        fs::create_dir(&real).unwrap();
        let link = top.join("link");
        link_folder(&link, &real).unwrap();
        assert_eq!(nearest_existing(&link.join("game")).unwrap(), link);
        assert_eq!(nearest_existing(&link).unwrap(), link);

        // A relative path is taken from the working folder, and an empty one names nothing.
        let working = std::env::current_dir().unwrap();
        let name = format!(
            "{}-absent",
            top.file_name()
                .unwrap()
                .to_str()
                .expect("a temporary folder's name is ASCII")
        );
        assert!(fs::symlink_metadata(working.join(&name)).is_err());
        assert_eq!(
            nearest_existing(&Path::new(&name).join("game")).unwrap(),
            working
        );
        let error = nearest_existing(Path::new("")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{error}");

        // Under a missing top folder the root is still found.
        #[cfg(unix)]
        {
            let missing = Path::new("/").join(&name);
            assert!(fs::symlink_metadata(&missing).is_err());
            assert_eq!(
                nearest_existing(&missing.join("game")).unwrap(),
                Path::new("/")
            );
        }

        // On a drive that is not there, nothing on the path is: not even its root.
        #[cfg(windows)]
        {
            let missing_drive = (b'A'..=b'Z')
                .rev()
                .map(|letter| PathBuf::from(format!("{}:\\", char::from(letter))))
                .find(|root| {
                    fs::symlink_metadata(root)
                        .is_err_and(|error| error.kind() == io::ErrorKind::NotFound)
                });
            let Some(root) = missing_drive else {
                eprintln!("skipped the missing drive: every drive letter names a drive");
                return;
            };
            let error = nearest_existing(&root.join("game").join("deeper")).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
        }
    }

    #[test]
    fn a_volume_has_room_and_a_temporary_folder_runs_programs() {
        let dir = tempfile::tempdir().unwrap();
        let volume = volume_of(dir.path()).unwrap();
        assert!(volume.available > 0, "{volume:?}");
        assert!(!volume.noexec, "{volume:?}");

        // A relative folder is taken from the working folder.
        assert!(volume_of(Path::new(".")).unwrap().available > 0);

        // The folder must be there.
        let error = volume_of(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound, "{error}");
    }

    /// The variable that names a folder on a file system mounted noexec, where the system the
    /// tests run on has one.
    #[cfg(target_os = "linux")]
    const NOEXEC_MOUNT: &str = "GDL_NOEXEC_MOUNT";

    #[cfg(target_os = "linux")]
    #[test]
    fn a_noexec_mount_says_so() {
        let Some(mount) = std::env::var_os(NOEXEC_MOUNT) else {
            eprintln!("skipped: {NOEXEC_MOUNT} names no folder on a noexec mount");
            return;
        };
        let mount = PathBuf::from(mount);
        let volume = volume_of(&mount).unwrap();
        assert!(volume.noexec, "{}: {volume:?}", mount.display());
    }

    #[test]
    fn a_probe_leaves_nothing_and_tells_case() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        let file = top.join("kept.txt");
        fs::write(&file, b"kept").unwrap();
        fs::create_dir(top.join("folder")).unwrap();
        let before = folder_names(top).unwrap();

        // Windows' file systems ignore letter case, and the Linux tests' do not.
        let expected = FolderProbe {
            writable: true,
            ignores_case: cfg!(windows),
        };
        assert_eq!(probe_folder(top).unwrap(), expected);
        assert_eq!(probe_folder(top).unwrap(), expected);
        assert_eq!(folder_names(top).unwrap(), before);

        // A folder that is not there is an error at it, and so is a file in a folder's place:
        // neither is a refusal to write.
        let missing = top.join("missing");
        let source = assert_io(probe_folder(&missing), &missing);
        assert_eq!(source.kind(), io::ErrorKind::NotFound, "{source}");
        assert_io(probe_folder(&file), &file);
        assert_eq!(fs::read(&file).unwrap(), b"kept");
        assert_eq!(folder_names(top).unwrap(), before);
    }

    #[test]
    fn a_folder_without_writes_is_not_writable() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("denied");
        fs::create_dir(&folder).unwrap();
        let denied = deny_writes(&folder).unwrap();
        if !denied.proven() {
            eprintln!("skipped: this user makes files in a folder denied writes, as root does");
            return;
        }
        let probe = probe_folder(&folder).unwrap();
        assert_eq!(
            probe,
            FolderProbe {
                writable: false,
                ignores_case: false,
            }
        );
        drop(denied);
        assert!(folder_names(&folder).unwrap().is_empty());
    }

    #[test]
    fn read_only_file_systems_refuse_writes() {
        for kind in [
            io::ErrorKind::PermissionDenied,
            io::ErrorKind::ReadOnlyFilesystem,
        ] {
            assert!(refuses_writes(&kind.into()), "{kind:?}");
        }
        for kind in [
            io::ErrorKind::NotFound,
            io::ErrorKind::StorageFull,
            io::ErrorKind::AlreadyExists,
            io::ErrorKind::Other,
        ] {
            assert!(!refuses_writes(&kind.into()), "{kind:?}");
        }

        // The systems' own codes for them.
        #[cfg(windows)]
        {
            /// ERROR_FILE_NOT_FOUND.
            const FILE_NOT_FOUND: i32 = 2;
            /// ERROR_ACCESS_DENIED.
            const ACCESS_DENIED: i32 = 5;
            /// ERROR_WRITE_PROTECT, a write-protected medium.
            const WRITE_PROTECT: i32 = 19;
            /// ERROR_SHARING_VIOLATION, a passing hold.
            const SHARING_VIOLATION: i32 = 32;
            /// ERROR_DISK_FULL.
            const DISK_FULL: i32 = 112;

            for (code, refused) in [
                (ACCESS_DENIED, true),
                (WRITE_PROTECT, true),
                (FILE_NOT_FOUND, false),
                (SHARING_VIOLATION, false),
                (DISK_FULL, false),
            ] {
                let error = io::Error::from_raw_os_error(code);
                assert_eq!(refuses_writes(&error), refused, "{code}: {error}");
            }
        }
        #[cfg(unix)]
        for (code, refused) in [
            (libc::EACCES, true),
            (libc::EPERM, true),
            (libc::EROFS, true),
            (libc::ENOENT, false),
            (libc::ENOSPC, false),
            (libc::EEXIST, false),
        ] {
            let error = io::Error::from_raw_os_error(code);
            assert_eq!(refuses_writes(&error), refused, "{code}: {error}");
        }
    }

    /// The length, in UTF-16 units, of a folder whose probe file lies past MAX_PATH, where the
    /// probe and the volume's measure must still reach.
    #[cfg(windows)]
    const PAST_MAX_PATH_UNITS: usize = 250;

    #[cfg(windows)]
    #[test]
    fn a_folder_past_max_path_is_measured_and_probed() {
        use super::PROBE_PREFIX;
        use crate::STAGING_RANDOM_CHARS;

        /// MAX_PATH: the most UTF-16 units a plain Windows path may hold, its NUL included.
        const MAX_PATH: usize = 260;

        let probe_name_units = PROBE_PREFIX.len() + STAGING_RANDOM_CHARS;
        assert!(PAST_MAX_PATH_UNITS + 1 + probe_name_units > MAX_PATH);

        let base = tempfile::tempdir().unwrap();
        let folder = gdl_fixtures::long_root(base.path(), PAST_MAX_PATH_UNITS).unwrap();
        assert!(volume_of(&folder).unwrap().available > 0);
        assert_eq!(nearest_existing(&folder.join("game")).unwrap(), folder);
        assert_eq!(
            probe_folder(&folder).unwrap(),
            FolderProbe {
                writable: true,
                ignores_case: true,
            }
        );
        assert!(folder_names(&folder).unwrap().is_empty());
    }
}
