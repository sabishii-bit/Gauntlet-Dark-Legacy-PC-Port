//! What a folder and its volume allow before anything is written there: the nearest folder on a
//! path that exists, the room its volume still has for this process, whether its file system runs
//! programs, and whether the folder takes what a job will make in it and tells letter case apart,
//! which entries of the launcher's own, made there and removed at once, show.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use crate::links::walked;
use crate::{
    FilesError, RETRY_WINDOW, STAGING_RANDOM_CHARS, is_absent, is_staging_name, retry_locked,
};

/// The prefix of the entries a folder probe makes and removes: names of the launcher's own.
pub const PROBE_PREFIX: &str = ".gdl-launcher-probe-";

/// How long before a probe begins an entry of a probe's must have last changed for the probe to
/// take it for one that an earlier probe left, and not one that a probe still running made: a
/// probe removes what it made once it has looked, trying each entry for at most [`RETRY_WINDOW`],
/// so a running probe's entries are seconds old. The rest of the minute leaves room for a file
/// system that keeps times to the even second, and for the clock of another machine that sets
/// them in a shared folder.
const LEFT_PROBE_AGE: Duration = Duration::from_secs(60);

// A probe removes at most two entries, each tried for the retry window.
const _: () = assert!(LEFT_PROBE_AGE.as_secs() > 2 * RETRY_WINDOW.as_secs());

/// What a folder that a probe looks at is to the job that will write there, which tells the probe
/// what to make in it: what the job makes there. Windows keeps the right to add a folder to a
/// folder apart from the right to add a file, as at the top of its system drive, where any user
/// may make a folder but no file, so a probe that made a file alone would refuse a folder a job
/// could make its destination in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProbedFolder {
    /// The folder the job writes into, there already: the job makes a folder of its own in it,
    /// and puts files and folders in place in it. The probe makes a folder and a file there.
    Destination,
    /// The nearest folder that is there above a destination that is not: the job makes only the
    /// folders down to the destination in it. The probe makes a folder there.
    AboveDestination,
}

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

/// What making a job's entries in a folder showed: whether the folder takes them, and whether its
/// file system ignores letter case.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FolderProbe {
    /// Whether every entry the probe tried could be made there. One refused for want of
    /// permission, or by a file system that is read-only, is not.
    pub writable: bool,
    /// Whether what was made could also be found by its name in upper case, as a file system that
    /// ignores letter case finds it. Always `false` when the folder is not writable.
    pub ignores_case: bool,
}

/// What a probe finds in a folder that refuses what a job would make there.
const NOT_WRITABLE: FolderProbe = FolderProbe {
    writable: false,
    ignores_case: false,
};

/// `path` itself when it is there, or else the nearest folder above it that is, looked at without
/// following a link the path ends in: the first that is there is returned whatever it is, a file
/// or a link included, for the caller to judge. Names that are not there, as [`is_absent`] tells,
/// are passed over, and so on Windows is a drive that holds no medium, such as an empty card
/// reader, which the system answers as not ready; any other failure to look at one is returned. A
/// relative path is taken from the working folder, as [`no_links`](crate::no_links) takes it.
/// When nothing on the path is there, its root included, as on a drive that does not exist or is
/// empty, the error is [`io::ErrorKind::NotFound`].
pub fn nearest_existing(path: &Path) -> io::Result<PathBuf> {
    let walked = walked(path)?;
    // Without a trailing separator, which would have the system look through a link the path
    // ends in instead of at the link itself.
    for part in walked.components().as_path().ancestors() {
        match fs::symlink_metadata(part) {
            Ok(_) => return Ok(part.to_path_buf()),
            Err(error) if nothing_there(&error) => {}
            Err(error) => return Err(error),
        }
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        format!("nothing on the path {} exists", walked.display()),
    ))
}

/// What Windows answers a look at a drive that holds no medium with: ERROR_NOT_READY, to which
/// std gives no kind of its own.
#[cfg(windows)]
const ERROR_NOT_READY: i32 = 21;

/// Whether an error looking at a name on a path says that nothing is there for
/// [`nearest_existing`] to stop at: what [`is_absent`] calls absent, and on Windows a drive that
/// holds no medium, on which nothing is there either.
fn nothing_there(error: &io::Error) -> bool {
    #[cfg(windows)]
    if error.raw_os_error() == Some(ERROR_NOT_READY) {
        return true;
    }
    is_absent(error)
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

/// Makes in `folder` what a job that `probed` says it is to will make there, each entry new and
/// named [`PROBE_PREFIX`] and then [`STAGING_RANDOM_CHARS`] random ASCII letters and digits, looks
/// for the last made by its name in ASCII upper case, and removes them, so that a folder is
/// judged before a job writes into it: a folder, and in a destination that is there a file beside
/// it, closed at once. A creation that [`refuses_writes`] tells is a refusal gives a folder that
/// is not writable; any other failure is an error at `folder`.
///
/// Each creation is tried once: a refusal is the folder's answer, not a passing hold. Each removal
/// is tried again as [`retry_locked`] tries, since a scanner may open a file the moment it is
/// closed, and every one is tried; one that still fails is an error naming what it left, which
/// is reported before anything the probe found.
///
/// Before it makes anything, the probe removes what an earlier probe left in `folder`, cut short
/// before it removed its entries or refused their removal, so that a folder probed again, as an
/// install probes its destination again before it writes there, keeps none of it: each entry of
/// such a name that is an empty folder or an empty file, never a link, and last changed a minute
/// or more before, which no probe still running made. Each is tried once, and one that cannot be
/// removed stays for a later probe.
pub fn probe_folder(folder: &Path, probed: ProbedFolder) -> Result<FolderProbe, FilesError> {
    remove_left_probe_entries(folder, SystemTime::now())?;
    let Some(made_folder) = make_probe_entry(folder, ProbeEntry::Folder)? else {
        return Ok(NOT_WRITABLE);
    };
    let mut made = vec![(made_folder.clone(), ProbeEntry::Folder)];
    let found = match probed {
        ProbedFolder::AboveDestination => looked_for_in_upper_case(&made_folder),
        ProbedFolder::Destination => match make_probe_entry(folder, ProbeEntry::File) {
            Ok(Some(file)) => {
                made.push((file.clone(), ProbeEntry::File));
                looked_for_in_upper_case(&file)
            }
            Ok(None) => Ok(NOT_WRITABLE),
            Err(error) => Err(error),
        },
    };
    remove_probe_entries(&made)?;
    found
}

/// What a probe makes in a folder: a folder, as a job makes its destination or its own folder
/// there, or a file, as a job puts files in place there.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ProbeEntry {
    /// A new folder.
    Folder,
    /// A new file, closed at once.
    File,
}

/// Makes one new probe entry in `folder` and gives its path, or `None` when the system refuses it
/// as [`refuses_writes`] tells. Any other failure is an error at `folder`.
fn make_probe_entry(folder: &Path, entry: ProbeEntry) -> Result<Option<PathBuf>, FilesError> {
    // Made without cleanup, so that nothing removes an entry but the probe's own removal, which
    // is retried: tempfile's own way of keeping a file it would remove hands Windows a plain
    // path, which fails past MAX_PATH.
    let mut builder = tempfile::Builder::new();
    builder
        .prefix(PROBE_PREFIX)
        .rand_bytes(STAGING_RANDOM_CHARS)
        .disable_cleanup(true);
    let made = match entry {
        ProbeEntry::Folder => builder.tempdir_in(folder).map(tempfile::TempDir::keep),
        ProbeEntry::File => builder.tempfile_in(folder).map(|probe| {
            // Closed at once, so that the removal takes the name away: Windows keeps the name of
            // a file removed while a handle on it is open until the handle closes.
            let (file, path) = probe.into_parts();
            drop(file);
            path.to_path_buf()
        }),
    };
    match made {
        Ok(path) => Ok(Some(path)),
        Err(error) if refuses_writes(&error) => Ok(None),
        Err(source) => Err(FilesError::io(folder, source)),
    }
}

/// What a probe found once `made`, a probe entry, could be made: a writable folder, which ignores
/// letter case when the name of `made` in ASCII upper case finds it too.
fn looked_for_in_upper_case(made: &Path) -> Result<FolderProbe, FilesError> {
    // The prefix holds lower-case letters, so the name in upper case is always another spelling.
    let upper = made.with_file_name(made.file_name().unwrap_or_default().to_ascii_uppercase());
    let ignores_case = match fs::symlink_metadata(&upper) {
        Ok(_) => true,
        Err(error) if is_absent(&error) => false,
        Err(source) => return Err(FilesError::io(&upper, source)),
    };
    Ok(FolderProbe {
        writable: true,
        ignores_case,
    })
}

/// Removes the entries a probe made, the last made first, each tried again as [`retry_locked`]
/// tries; every one is tried, and the first that could not be removed is reported.
fn remove_probe_entries(made: &[(PathBuf, ProbeEntry)]) -> Result<(), FilesError> {
    let mut left = None;
    for (path, entry) in made.iter().rev() {
        let removed = match entry {
            ProbeEntry::Folder => retry_locked(|| fs::remove_dir(path)),
            ProbeEntry::File => retry_locked(|| fs::remove_file(path)),
        };
        if let Err(source) = removed {
            left.get_or_insert_with(|| FilesError::io(path, source));
        }
    }
    left.map_or(Ok(()), Err)
}

/// Removes from `folder` the entries that earlier probes made there and left: each named as a
/// probe names what it makes, an empty folder or an empty file, never a link, last changed
/// [`LEFT_PROBE_AGE`] or more before `now`, so that what a probe still running made is never taken
/// from it. Each is tried once, and one that cannot be removed stays for a later probe: it is not
/// this probe's to answer for, and nothing this probe finds rests on it. A folder this process may
/// not list is left as it is, since the probe may still make entries there; any other failure to
/// list it is an error at `folder`.
fn remove_left_probe_entries(folder: &Path, now: SystemTime) -> Result<(), FilesError> {
    let entries = match fs::read_dir(folder) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::PermissionDenied => return Ok(()),
        Err(source) => return Err(FilesError::io(folder, source)),
    };
    for entry in entries {
        let entry = entry.map_err(|source| FilesError::io(folder, source))?;
        let named = entry
            .file_name()
            .to_str()
            .is_some_and(|name| is_staging_name(PROBE_PREFIX, name));
        if !named {
            continue;
        }
        let path = entry.path();
        // Looked at without following a link: a link is no probe's entry, and what it leads to is
        // never touched.
        let Ok(found) = fs::symlink_metadata(&path) else {
            continue;
        };
        let left = found
            .modified()
            .ok()
            .and_then(|modified| now.duration_since(modified).ok())
            .is_some_and(|age| age >= LEFT_PROBE_AGE);
        if !left {
            continue;
        }
        // A failure is passed over, as said above; a folder that holds anything is no probe's
        // and stays, since only an empty one is removed.
        let _ = if found.is_dir() {
            fs::remove_dir(&path)
        } else if found.is_file() && found.len() == 0 {
            fs::remove_file(&path)
        } else {
            continue;
        };
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};
    use std::time::{Duration, SystemTime};

    use gdl_fixtures::{backdate, deny_writes, folder_names, link_folder};

    use super::{
        FolderProbe, LEFT_PROBE_AGE, NOT_WRITABLE, PROBE_PREFIX, ProbedFolder, nearest_existing,
        nothing_there, probe_folder, refuses_writes, remove_left_probe_entries, volume_of,
    };
    use crate::error::assert_io;

    /// Both things a probed folder may be to a job.
    const PROBED: [ProbedFolder; 2] = [ProbedFolder::Destination, ProbedFolder::AboveDestination];

    /// What a probe finds in a folder of the tests' own, which takes anything: Windows' file
    /// systems ignore letter case, and the Linux tests' do not.
    const WRITABLE: FolderProbe = FolderProbe {
        writable: true,
        ignores_case: cfg!(windows),
    };

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

    /// What a walk up a path passes over: what the link checks call absent, and on Windows a
    /// drive that holds no medium, whose not-ready answer std gives no kind, so that the walk
    /// ends at nothing there rather than at an error. No test can empty a drive, so the rule is
    /// shown on the errors themselves.
    #[test]
    fn a_drive_without_a_medium_holds_nothing() {
        for kind in [io::ErrorKind::NotFound, io::ErrorKind::NotADirectory] {
            assert!(nothing_there(&kind.into()), "{kind:?}");
        }
        for kind in [
            io::ErrorKind::PermissionDenied,
            io::ErrorKind::InvalidInput,
            io::ErrorKind::Other,
        ] {
            assert!(!nothing_there(&kind.into()), "{kind:?}");
        }
        #[cfg(windows)]
        {
            let not_ready = io::Error::from_raw_os_error(super::ERROR_NOT_READY);
            assert!(nothing_there(&not_ready), "{not_ready}");
            // The link checks still report it, as a name they could not look at: std's kind for
            // it is not one of absence.
            assert!(!crate::is_absent(&not_ready), "{not_ready}");
        }
        // Elsewhere that number is another error, which stops the walk: Linux's EISDIR.
        #[cfg(unix)]
        assert!(!nothing_there(&io::Error::from_raw_os_error(libc::EISDIR)));
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

        for probed in PROBED {
            assert_eq!(probe_folder(top, probed).unwrap(), WRITABLE, "{probed:?}");
            assert_eq!(probe_folder(top, probed).unwrap(), WRITABLE, "{probed:?}");
            assert_eq!(folder_names(top).unwrap(), before, "{probed:?}");

            // A folder that is not there is an error at it, and so is a file in a folder's
            // place: neither is a refusal to write.
            let missing = top.join("missing");
            let source = assert_io(probe_folder(&missing, probed), &missing);
            assert_eq!(source.kind(), io::ErrorKind::NotFound, "{source}");
            assert_io(probe_folder(&file, probed), &file);
            assert_eq!(fs::read(&file).unwrap(), b"kept");
            assert_eq!(folder_names(top).unwrap(), before, "{probed:?}");
        }
    }

    /// A name a probe gives what it makes, with `random` for its random part.
    fn probe_name(random: &str) -> String {
        format!("{PROBE_PREFIX}{random}")
    }

    /// What an earlier probe left in a folder, cut short before it removed its entries or refused
    /// their removal, is gone once the folder is probed again: its folder and its file, old enough
    /// to be no running probe's. What a running probe may have made a moment ago stays, and so
    /// does the rest.
    #[test]
    fn a_probe_removes_what_an_earlier_probe_left() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        fs::create_dir(top.join(probe_name("Ab12Cd34"))).unwrap();
        fs::write(top.join(probe_name("Ef56Gh78")), b"").unwrap();
        fs::write(top.join("kept.txt"), b"kept").unwrap();
        backdate(top).unwrap();
        let fresh = [probe_name("Uv12Wx34"), probe_name("Yz56Ab78")];
        fs::create_dir(top.join(&fresh[0])).unwrap();
        fs::write(top.join(&fresh[1]), b"").unwrap();

        for probed in PROBED {
            assert_eq!(probe_folder(top, probed).unwrap(), WRITABLE, "{probed:?}");
            let mut expected = vec!["kept.txt".to_owned(), fresh[0].clone(), fresh[1].clone()];
            expected.sort();
            assert_eq!(folder_names(top).unwrap(), expected, "{probed:?}");
        }
        assert_eq!(fs::read(top.join("kept.txt")).unwrap(), b"kept");
    }

    /// An entry of a probe's is taken for one an earlier probe left once it last changed the
    /// probe age before the probe began, and not a moment sooner; nor while it seems to have
    /// changed after that, as a shared folder whose times another machine's clock sets may show.
    #[test]
    fn a_left_entry_is_told_by_its_age() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        let folder = top.join(probe_name("Ab12Cd34"));
        let file = top.join(probe_name("Ef56Gh78"));
        fs::create_dir(&folder).unwrap();
        fs::write(&file, b"").unwrap();
        let changed = |path: &Path| fs::symlink_metadata(path).unwrap().modified().unwrap();
        let (earliest, latest) = {
            let (folder, file) = (changed(&folder), changed(&file));
            (folder.min(file), folder.max(file))
        };

        let held = [
            earliest + LEFT_PROBE_AGE - Duration::from_millis(1),
            earliest - Duration::from_secs(1),
        ];
        for now in held {
            remove_left_probe_entries(top, now).unwrap();
            assert_eq!(folder_names(top).unwrap().len(), 2, "{now:?}");
        }
        remove_left_probe_entries(top, latest + LEFT_PROBE_AGE).unwrap();
        assert!(folder_names(top).unwrap().is_empty());
    }

    /// What a probe never leaves stays, however old: names that only begin as a probe's do, and a
    /// probe's name on a file that holds something, on a folder that holds something and on a
    /// link, whose target is never touched.
    #[test]
    fn only_a_probe_s_own_entries_are_taken_for_left() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        let target = tempfile::tempdir().unwrap();
        fs::write(target.path().join("target.txt"), b"").unwrap();
        let mut kept = vec![
            format!("{PROBE_PREFIX}notes.txt"),
            probe_name("Ab12Cd3"),
            probe_name("Ab12Cd345"),
            probe_name("Ef56Gh78"),
            probe_name("Ij90Kl12"),
            probe_name("Mn34Op56"),
        ];
        fs::write(top.join(&kept[0]), b"").unwrap();
        fs::create_dir(top.join(&kept[1])).unwrap();
        fs::write(top.join(&kept[2]), b"").unwrap();
        fs::write(top.join(&kept[3]), b"kept").unwrap();
        fs::create_dir(top.join(&kept[4])).unwrap();
        fs::write(top.join(&kept[4]).join("inside"), b"").unwrap();
        link_folder(&top.join(&kept[5]), target.path()).unwrap();
        let file_link = probe_name("Qr78St90");
        if gdl_fixtures::symlink_file_where_allowed(
            &top.join(&file_link),
            &target.path().join("target.txt"),
        ) {
            kept.push(file_link);
        } else {
            eprintln!("skipped the link to a file: this account may not make one");
        }

        let long_after = SystemTime::now() + 2 * LEFT_PROBE_AGE;
        remove_left_probe_entries(top, long_after).unwrap();
        kept.sort();
        assert_eq!(folder_names(top).unwrap(), kept);
        assert_eq!(fs::read(top.join(probe_name("Ef56Gh78"))).unwrap(), b"kept");
        assert_eq!(
            folder_names(&top.join(probe_name("Ij90Kl12"))).unwrap(),
            ["inside"]
        );
        assert_eq!(folder_names(target.path()).unwrap(), ["target.txt"]);
    }

    /// A folder this process may make entries in but not list is probed all the same, and what an
    /// earlier probe left there stays, unseen.
    #[cfg(unix)]
    #[test]
    fn a_folder_that_may_not_be_listed_is_still_probed() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("unlisted");
        fs::create_dir(&folder).unwrap();
        let left = probe_name("Ab12Cd34");
        fs::write(folder.join(&left), b"").unwrap();
        backdate(&folder).unwrap();
        let access = gdl_fixtures::DeniedAccess::unlisted(&[&folder]).unwrap();
        if fs::read_dir(&folder).is_ok() {
            eprintln!("skipped: this user lists a folder denied listing, as root does");
            return;
        }

        for probed in PROBED {
            assert_eq!(
                probe_folder(&folder, probed).unwrap(),
                WRITABLE,
                "{probed:?}"
            );
        }
        drop(access);
        assert_eq!(folder_names(&folder).unwrap(), [left]);
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
        // A destination there takes no file, whatever else it takes; what its probe made before
        // the refusal is gone again.
        let probe = probe_folder(&folder, ProbedFolder::Destination).unwrap();
        assert_eq!(probe, NOT_WRITABLE);
        assert!(folder_names(&folder).unwrap().is_empty());
        // A destination below it is made as a new folder is made there, which on Windows a
        // process whose restore privilege is enabled still does, as the guard's check found.
        let expected = if denied.refuses_folders() {
            NOT_WRITABLE
        } else {
            WRITABLE
        };
        let probe = probe_folder(&folder, ProbedFolder::AboveDestination).unwrap();
        assert_eq!(probe, expected);
        assert!(folder_names(&folder).unwrap().is_empty());
        drop(denied);
        assert!(folder_names(&folder).unwrap().is_empty());
    }

    /// A folder that takes new folders but no new file, as the top of a Windows system drive is
    /// for any user, takes a destination below it, made as a new folder, but is no destination
    /// itself, whatever this process's privileges.
    #[cfg(windows)]
    #[test]
    fn a_folder_taking_only_folders_takes_a_destination_below_it() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("drive top");
        fs::create_dir(&folder).unwrap();
        let denied = gdl_fixtures::deny_new_files(&folder).unwrap();
        assert!(!denied.refuses_folders());

        let probe = probe_folder(&folder, ProbedFolder::AboveDestination).unwrap();
        assert_eq!(probe, WRITABLE);
        assert!(folder_names(&folder).unwrap().is_empty());
        // As a destination it would take its own folder, then refuse the files put in place.
        let probe = probe_folder(&folder, ProbedFolder::Destination).unwrap();
        assert_eq!(probe, NOT_WRITABLE);
        assert!(folder_names(&folder).unwrap().is_empty());
        drop(denied);
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
        use crate::STAGING_RANDOM_CHARS;

        /// MAX_PATH: the most UTF-16 units a plain Windows path may hold, its NUL included.
        const MAX_PATH: usize = 260;

        let probe_name_units = PROBE_PREFIX.len() + STAGING_RANDOM_CHARS;
        assert!(PAST_MAX_PATH_UNITS + 1 + probe_name_units > MAX_PATH);

        let base = tempfile::tempdir().unwrap();
        let folder = gdl_fixtures::long_root(base.path(), PAST_MAX_PATH_UNITS).unwrap();
        assert!(volume_of(&folder).unwrap().available > 0);
        assert_eq!(nearest_existing(&folder.join("game")).unwrap(), folder);
        for probed in PROBED {
            assert_eq!(
                probe_folder(&folder, probed).unwrap(),
                WRITABLE,
                "{probed:?}"
            );
            assert!(folder_names(&folder).unwrap().is_empty(), "{probed:?}");
        }
    }
}
