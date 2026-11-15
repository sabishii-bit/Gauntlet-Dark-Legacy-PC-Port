//! An installation's folder: where the launcher keeps its own state in it, the lock a job holds
//! while it changes the installation, and the names by which a folder the alpha installer made is
//! known. Such a folder is recognised by its names alone, so the launcher never reads, and never
//! comes to rely on, a file the alpha installer wrote.

use std::fs::{self, File, OpenOptions, TryLockError};
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{ALPHA_NAMES, FilesError, absolute_lexical, no_links};

/// The folder at an installation's top that holds the launcher's own state.
pub const STATE_DIR: &str = ".gdl-launcher";

/// The file in the state folder whose lock a job holds while it changes the installation. It
/// holds nothing, and nothing reads it: while the lock is held, Windows refuses every other
/// handle's read of it.
pub const LOCK_FILE: &str = "lock";

/// The installation's receipt, in the state folder.
pub const RECEIPT_FILE: &str = "receipt.json";

/// The folder in the state folder that holds the journals of the jobs under way.
pub const JOURNAL_DIR: &str = "journal";

/// The flag that has Windows open a link or a junction itself rather than what it leads to.
#[cfg(windows)]
const FILE_FLAG_OPEN_REPARSE_POINT: u32 = 0x0020_0000;

/// Why a lock is refused when the file it locked is no longer the one at the lock file's name.
#[cfg(unix)]
const LOCK_FILE_REPLACED: &str = "the lock file was replaced while the lock was taken";

/// Refuses `root`, an installation's folder, when it or a folder above it is a link: the rule that
/// [`InstallFolder::at`], [`InstallFolder::installed`] and [`InstallFolder::inspect`] hold an
/// installation's folder to. gdl-files' readers and writers hold each whole path they take to the
/// same rule through [`no_links`], the folders above the installation included, so a change to
/// what this refuses above the folder must reach them too; otherwise a folder accepted here would
/// be refused once something in it is read or written.
pub fn check_links(root: &Path) -> Result<(), FilesError> {
    no_links(root)
}

/// A folder that may hold an installation: one that is there, is a directory and has no link on
/// its path. What it holds is judged by its readers.
#[derive(Clone, Debug)]
pub struct InstallFolder {
    /// The folder, absolute, with its `.` and `..` folded by name.
    root: PathBuf,
}

impl InstallFolder {
    /// The folder at `root`, made absolute and folded by name as every folder the launcher is
    /// given is folded, refused when it or a folder above it is a link, when it is not a
    /// directory, and when nothing is there.
    pub fn at(root: &Path) -> Result<InstallFolder, FolderError> {
        let root = absolute_lexical(root).map_err(|source| FolderError::Io {
            path: root.to_path_buf(),
            source,
        })?;
        check_links(&root).map_err(|error| FolderError::of_links_check(error, &root))?;
        // Looked at without following a link, so one that took the name since the check is
        // refused rather than followed.
        match fs::symlink_metadata(&root) {
            Ok(metadata) if metadata.is_dir() => Ok(InstallFolder { root }),
            Ok(_) => Err(FolderError::NotDirectory { path: root }),
            Err(error) if is_absent(&error) => Err(FolderError::Absent { path: root }),
            Err(source) => Err(FolderError::Io { path: root, source }),
        }
    }

    /// The folder itself.
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// The folder in it that holds the launcher's state.
    pub fn state_dir(&self) -> PathBuf {
        self.root.join(STATE_DIR)
    }

    /// Takes the installation's lock, which a job holds while it changes the installation. While
    /// it is held, no other job, in this launcher or another, can take it: the attempt is
    /// [`LockError::Busy`] at once rather than a wait, so that a player is told the installation
    /// is in use instead of facing a launcher that hangs. The lock is the system's own on
    /// [`LOCK_FILE`], so a launcher that crashes never leaves it held.
    ///
    /// The state folder must exist already, as a real folder. The lock file is opened without
    /// following a link at its name, made when it is missing, never emptied, and never removed:
    /// were one job to remove it while another held its lock, a third could lock a new file of the
    /// same name. Once the lock is taken, the state folder and the lock file are looked at again,
    /// and on Unix the file locked must be the one at the lock file's name, so that a lock taken
    /// through a link that took either name meanwhile is let go and refused.
    pub fn lock(&self) -> Result<InstallLock, LockError> {
        let state = self.state_dir();
        check_state_folder(&state)?;
        let path = state.join(LOCK_FILE);
        let file = open_lock_file(&path)?;
        match file.try_lock() {
            Ok(()) => {}
            Err(TryLockError::WouldBlock) => return Err(LockError::Busy),
            Err(TryLockError::Error(source)) => return Err(LockError::Io { path, source }),
        }
        // Dropped, and so let go, when a second look refuses it.
        let lock = InstallLock { file };
        check_state_folder(&state)?;
        check_lock_file(&path, &lock.file)?;
        Ok(lock)
    }
}

/// Refuses `state` unless a real folder is there to hold the lock file: a link is refused as one,
/// and nothing there, or something other than a folder, is no state folder.
fn check_state_folder(state: &Path) -> Result<(), LockError> {
    let path = || state.to_path_buf();
    match fs::symlink_metadata(state) {
        Ok(metadata) if metadata.file_type().is_symlink() => Err(LockError::Links { path: path() }),
        Ok(metadata) if metadata.is_dir() => Ok(()),
        Ok(_) => Err(LockError::NoStateFolder { path: path() }),
        Err(error) if is_absent(&error) => Err(LockError::NoStateFolder { path: path() }),
        Err(source) => Err(LockError::Io {
            path: path(),
            source,
        }),
    }
}

/// Opens the lock file at `path` to read and to write, since whether a file open only to read can
/// be locked differs between systems, making it when it is missing and keeping what it holds. The
/// open never follows a link at the name: Unix refuses to open one, and Windows opens the link
/// itself, so that a link there is refused as one rather than locked through.
fn open_lock_file(path: &Path) -> Result<File, LockError> {
    let mut options = OpenOptions::new();
    options.read(true).write(true).create(true).truncate(false);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.custom_flags(FILE_FLAG_OPEN_REPARSE_POINT);
    }
    let file = options.open(path).map_err(|source| {
        // A link refused by Unix, or a junction that Windows will not open as a file, is told
        // from any other refusal by what is at the name.
        if is_link(path) {
            LockError::Links {
                path: path.to_path_buf(),
            }
        } else {
            LockError::Io {
                path: path.to_path_buf(),
                source,
            }
        }
    })?;
    match file.metadata() {
        Ok(metadata) if metadata.file_type().is_symlink() => Err(LockError::Links {
            path: path.to_path_buf(),
        }),
        Ok(_) => Ok(file),
        Err(source) => Err(LockError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// Refuses the lock that `file` holds unless what is at the lock file's name `path` is no link
/// and, on Unix, is the file that `file` has open.
fn check_lock_file(path: &Path, file: &File) -> Result<(), LockError> {
    let entry = fs::symlink_metadata(path).map_err(|source| LockError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if entry.file_type().is_symlink() {
        return Err(LockError::Links {
            path: path.to_path_buf(),
        });
    }
    check_same_file(path, file, &entry)
}

/// Refuses the lock that `file` holds unless `entry`, what is at the lock file's name `path`, is
/// the file it has open, by device and inode.
#[cfg(unix)]
fn check_same_file(path: &Path, file: &File, entry: &fs::Metadata) -> Result<(), LockError> {
    use std::os::unix::fs::MetadataExt;

    let held = file.metadata().map_err(|source| LockError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if (held.dev(), held.ino()) == (entry.dev(), entry.ino()) {
        Ok(())
    } else {
        Err(LockError::Io {
            path: path.to_path_buf(),
            source: io::Error::other(LOCK_FILE_REPLACED),
        })
    }
}

/// Windows gives an open file no identity that std lets a program compare with what is at a name,
/// so there only the links are looked at.
#[cfg(not(unix))]
fn check_same_file(_: &Path, _: &File, _: &fs::Metadata) -> Result<(), LockError> {
    Ok(())
}

/// Whether a link is at `path`, looked at without following it.
fn is_link(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_symlink())
}

/// Whether an error looking at a path says that nothing is there: the path is missing, or a
/// folder above it is a file.
pub(crate) fn is_absent(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::NotFound | io::ErrorKind::NotADirectory
    )
}

/// An installation's lock, held until this is dropped.
#[derive(Debug)]
#[must_use = "dropping it releases the lock at once"]
pub struct InstallLock {
    /// The lock file, open, which holds the lock.
    file: File,
}

impl Drop for InstallLock {
    fn drop(&mut self) {
        // Released before the file closes: Windows may let go of a closed file's lock only some
        // time later, and a job that takes the lock again at once must find it free. Should the
        // release fail, closing the file releases the lock all the same.
        let _ = self.file.unlock();
    }
}

/// The alpha installer's names that `root` holds directly, by which a folder it installed or
/// updated is known: the spellings of [`ALPHA_NAMES`], in that order, each once however many
/// letter cases of it the folder holds. A name counts in any ASCII letter case on every system,
/// as the launcher refuses to write one in any, and both of the alpha updater's names count on
/// both systems, since a folder may have been installed from either.
///
/// The folder is only listed. No entry in it is opened or looked at, so none of the alpha
/// installer's files is read, and a marker the launcher may not open is still found.
pub fn alpha_markers(root: &Path) -> io::Result<Vec<&'static str>> {
    let mut held = [false; ALPHA_NAMES.len()];
    for entry in fs::read_dir(root)? {
        let name = entry?.file_name();
        if let Some(index) = ALPHA_NAMES
            .iter()
            .position(|&alpha| name.eq_ignore_ascii_case(alpha))
        {
            held[index] = true;
        }
    }
    Ok(ALPHA_NAMES
        .into_iter()
        .zip(held)
        .filter_map(|(name, held)| held.then_some(name))
        .collect())
}

/// Why a folder cannot hold an installation, or cannot be looked at, as technical detail for
/// logs. What a player reads is chosen from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum FolderError {
    /// `path` is a link: the folder itself or a folder above it.
    #[error("Choose a directory without links or junctions: {}", .path.display())]
    Links {
        /// The link.
        path: PathBuf,
    },
    /// Something other than a directory is at `path`.
    #[error("Install folder is not a directory: {}", .path.display())]
    NotDirectory {
        /// The folder as it was given, made absolute.
        path: PathBuf,
    },
    /// Nothing is at `path`.
    #[error("Install folder does not exist: {}", .path.display())]
    Absent {
        /// The folder as it was given, made absolute.
        path: PathBuf,
    },
    /// The system refused to look at `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The path looked at: the folder or one above it.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
}

impl FolderError {
    /// The refusal of the folder `root` for what its link check reported. A link check reports
    /// links and the system's refusals; anything else it reported would count as the latter.
    fn of_links_check(error: FilesError, root: &Path) -> FolderError {
        match error {
            FilesError::Link { path } => FolderError::Links { path },
            FilesError::Io { path, source } => FolderError::Io { path, source },
            other => FolderError::Io {
                path: root.to_path_buf(),
                source: io::Error::other(other),
            },
        }
    }
}

/// Why an installation's lock was not taken, as technical detail for logs. What a player reads is
/// chosen from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum LockError {
    /// Another job holds the lock, in this launcher or another.
    #[error("Another job is using this installation")]
    Busy,
    /// `path`, the state folder or the lock file, is a link.
    #[error("Choose a directory without links or junctions: {}", .path.display())]
    Links {
        /// The link.
        path: PathBuf,
    },
    /// The installation has no state folder at `path` to hold the lock file.
    #[error("The installation has no launcher state folder: {}", .path.display())]
    NoStateFolder {
        /// Where the state folder belongs.
        path: PathBuf,
    },
    /// The system refused to look at, open or lock `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The state folder or the lock file.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};

    use gdl_files::{ALPHA_NAMES, FilesError};
    use gdl_fixtures::{folder_names, hold_closed, link_folder, make_alpha_markers, symlink_file};
    use tempfile::TempDir;

    use super::{
        FolderError, InstallFolder, InstallLock, LOCK_FILE, LockError, STATE_DIR, alpha_markers,
        check_links, check_lock_file, check_state_folder,
    };

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// How many times a test takes the lock again the moment it is released.
    const TAKES_IN_A_ROW: usize = 100;

    /// A new installation folder, holding nothing yet, in a folder of the test's own.
    fn bare_installation() -> (TempDir, InstallFolder) {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("install");
        fs::create_dir(&root).unwrap();
        let folder = InstallFolder::at(&root).unwrap();
        (dir, folder)
    }

    /// A new installation folder holding an empty state folder, in a folder of the test's own.
    fn installation() -> (TempDir, InstallFolder) {
        let (dir, folder) = bare_installation();
        fs::create_dir(folder.state_dir()).unwrap();
        (dir, folder)
    }

    /// Checks that `result` refused the link at `link`.
    #[track_caller]
    fn assert_lock_links(result: Result<InstallLock, LockError>, link: &Path) {
        match result {
            Err(LockError::Links { path }) => assert_eq!(path, link),
            other => panic!("{other:?}"),
        }
    }

    /// Checks that `result` found no state folder at `state`.
    #[track_caller]
    fn assert_no_state_folder(result: Result<InstallLock, LockError>, state: &Path) {
        match result {
            Err(LockError::NoStateFolder { path }) => assert_eq!(path, state),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn at_refuses_a_link_a_file_and_an_absent_folder() {
        let dir = tempfile::tempdir().unwrap();
        let real = dir.path().join("real");
        fs::create_dir_all(real.join("sub")).unwrap();

        // A real folder is taken absolute and folded by name, its state folder inside it.
        let dotted = dir.path().join("x").join("..").join(".").join("real");
        let folder = InstallFolder::at(&dotted).unwrap();
        assert_eq!(folder.root(), real);
        assert_eq!(folder.state_dir(), real.join(STATE_DIR));

        // A link to a folder, which any account can make on either system, and a folder reached
        // through one.
        let link = dir.path().join("link");
        link_folder(&link, &real).unwrap();
        for path in [link.clone(), link.join("sub")] {
            match InstallFolder::at(&path) {
                Err(FolderError::Links { path: found }) => assert_eq!(found, link),
                other => panic!("{}: {other:?}", path.display()),
            }
            match check_links(&path) {
                Err(FilesError::Link { path: found }) => assert_eq!(found, link),
                other => panic!("{}: {other:?}", path.display()),
            }
        }
        check_links(&real.join("sub")).unwrap();

        // A file.
        let file = dir.path().join("file.txt");
        fs::write(&file, b"not a folder").unwrap();
        match InstallFolder::at(&file) {
            Err(FolderError::NotDirectory { path }) => assert_eq!(path, file),
            other => panic!("{other:?}"),
        }

        // Nothing, at the top or deeper, and a name under a file, where nothing can be.
        let missing = dir.path().join("missing");
        for path in [missing.clone(), missing.join("deeper"), file.join("inside")] {
            match InstallFolder::at(&path) {
                Err(FolderError::Absent { path: found }) => assert_eq!(found, path),
                other => panic!("{}: {other:?}", path.display()),
            }
        }
        // A name that names nothing has no absolute form.
        match InstallFolder::at(Path::new("")) {
            Err(FolderError::Io { path, source }) => {
                assert_eq!(path, Path::new(""));
                assert_eq!(source.kind(), io::ErrorKind::InvalidInput);
            }
            other => panic!("{other:?}"),
        }
        // Nothing was made by looking.
        assert_eq!(
            folder_names(dir.path()).unwrap(),
            ["file.txt", "link", "real"]
        );
        assert_eq!(folder_names(&real).unwrap(), ["sub"]);
    }

    #[cfg(unix)]
    #[test]
    fn a_folder_that_cannot_be_searched_is_reported() {
        let dir = tempfile::tempdir().unwrap();
        let locked = dir.path().join("locked");
        let root = locked.join("install");
        fs::create_dir_all(&root).unwrap();
        let (_other, folder) = installation();
        let state = folder.state_dir();
        let denied = gdl_fixtures::DeniedAccess::deny(&[&locked, &state]).unwrap();
        if fs::symlink_metadata(&root).is_ok() {
            eprintln!("skipped: this user looks into folders of mode 0o000, as root does");
            return;
        }
        // Whether the folder is a link cannot be seen, so it is not taken to be none.
        match InstallFolder::at(&root) {
            Err(FolderError::Io { path, source }) => {
                assert_eq!(path, root);
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        // Nor whether the lock file is one.
        match folder.lock() {
            Err(LockError::Io { path, source }) => {
                assert_eq!(path, state.join(LOCK_FILE));
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        drop(denied);
        assert!(folder_names(&state).unwrap().is_empty());
    }

    #[test]
    fn a_second_lock_is_busy() {
        let (_dir, folder) = installation();
        let held = folder.lock().unwrap();
        // Through the same folder, and through another taken at the same place, as another
        // launcher would take it: each attempt opens the lock file anew.
        let again = InstallFolder::at(folder.root()).unwrap();
        for taker in [&folder, &again, &folder] {
            match taker.lock() {
                Err(LockError::Busy) => {}
                unexpected => panic!("{unexpected:?}"),
            }
        }
        // The refused attempts left the lock to its holder, until it lets go.
        drop(held);
        let taken = again.lock().unwrap();
        assert!(matches!(folder.lock(), Err(LockError::Busy)));
        drop(taken);
    }

    #[test]
    fn a_released_lock_can_be_taken_again() {
        let (_dir, folder) = installation();
        let again = InstallFolder::at(folder.root()).unwrap();
        // The moment it is released, as one job follows another, from either folder.
        for take in 0..TAKES_IN_A_ROW {
            let taker = if take % 2 == 0 { &folder } else { &again };
            let held = taker
                .lock()
                .unwrap_or_else(|error| panic!("take {take}: {error}"));
            drop(held);
        }
    }

    #[test]
    fn the_lock_file_survives_release() {
        let (_dir, folder) = installation();
        let lock_file = folder.state_dir().join(LOCK_FILE);
        let held = folder.lock().unwrap();
        assert!(fs::symlink_metadata(&lock_file).unwrap().is_file());
        drop(held);
        let metadata = fs::symlink_metadata(&lock_file).unwrap();
        assert!(metadata.is_file());
        assert_eq!(metadata.len(), 0);

        // A lock file that holds something keeps it, held and released.
        fs::write(&lock_file, b"kept").unwrap();
        let held = folder.lock().unwrap();
        drop(held);
        assert_eq!(fs::read(&lock_file).unwrap(), b"kept");
        // The lock file is all the lock makes.
        assert_eq!(folder_names(&folder.state_dir()).unwrap(), [LOCK_FILE]);
        assert_eq!(folder_names(folder.root()).unwrap(), [STATE_DIR]);
    }

    #[test]
    fn a_linked_lock_is_refused() {
        // A link to a folder in the lock file's place: on Windows a junction.
        let (dir, folder) = installation();
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        let lock_file = folder.state_dir().join(LOCK_FILE);
        link_folder(&lock_file, &elsewhere).unwrap();
        assert_lock_links(folder.lock(), &lock_file);
        assert!(folder_names(&elsewhere).unwrap().is_empty());

        // The state folder itself a link, which would put the lock file elsewhere.
        let (dir, folder) = bare_installation();
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        let state = folder.state_dir();
        link_folder(&state, &elsewhere).unwrap();
        assert_lock_links(folder.lock(), &state);
        assert!(folder_names(&elsewhere).unwrap().is_empty());

        // A link to a file and one to nothing, where this account may make them: what they lead
        // to is neither made nor changed.
        let (dir, folder) = installation();
        let lock_file = folder.state_dir().join(LOCK_FILE);
        let target = dir.path().join("target");
        fs::write(&target, b"untouched").unwrap();
        if let Err(error) = symlink_file(&lock_file, &target) {
            eprintln!("skipped the links to files: this account cannot make them ({error})");
            return;
        }
        assert_lock_links(folder.lock(), &lock_file);
        assert_eq!(fs::read(&target).unwrap(), b"untouched");
        let (dir, folder) = installation();
        let lock_file = folder.state_dir().join(LOCK_FILE);
        let nowhere = dir.path().join("nowhere");
        symlink_file(&lock_file, &nowhere).unwrap();
        assert_lock_links(folder.lock(), &lock_file);
        assert!(fs::symlink_metadata(&nowhere).is_err());
    }

    #[test]
    fn no_lock_without_the_state_folder() {
        let (_dir, folder) = bare_installation();
        let state = folder.state_dir();
        assert_no_state_folder(folder.lock(), &state);
        // Nothing is made in its place.
        assert!(folder_names(folder.root()).unwrap().is_empty());

        // A file in its place is no state folder either, and stays as it was.
        fs::write(&state, b"a file").unwrap();
        assert_no_state_folder(folder.lock(), &state);
        assert_eq!(fs::read(&state).unwrap(), b"a file");
        assert_eq!(folder_names(folder.root()).unwrap(), [STATE_DIR]);

        // Nor is there one once the installation's folder is gone.
        let (dir, folder) = bare_installation();
        fs::remove_dir(folder.root()).unwrap();
        assert_no_state_folder(folder.lock(), &folder.state_dir());
        assert!(folder_names(dir.path()).unwrap().is_empty());
    }

    #[test]
    fn a_folder_in_the_lock_files_place_is_refused() {
        let (_dir, folder) = installation();
        let lock_file = folder.state_dir().join(LOCK_FILE);
        fs::create_dir(&lock_file).unwrap();
        match folder.lock() {
            Err(LockError::Io { path, .. }) => assert_eq!(path, lock_file),
            other => panic!("{other:?}"),
        }
        assert!(folder_names(&lock_file).unwrap().is_empty());
    }

    #[test]
    fn a_lock_whose_names_changed_meanwhile_is_let_go() {
        let (dir, folder) = installation();
        let state = folder.state_dir();
        let lock_file = state.join(LOCK_FILE);
        let held = folder.lock().unwrap();
        // Looked at again as they are, the names keep the lock.
        check_state_folder(&state).unwrap();
        check_lock_file(&lock_file, &held.file).unwrap();

        // A link that has taken either name refuses it.
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        let link = dir.path().join("link");
        link_folder(&link, &elsewhere).unwrap();
        match check_state_folder(&link) {
            Err(LockError::Links { path }) => assert_eq!(path, link),
            other => panic!("{other:?}"),
        }
        match check_lock_file(&link, &held.file) {
            Err(LockError::Links { path }) => assert_eq!(path, link),
            other => panic!("{other:?}"),
        }

        // On Unix, so does another file at the lock file's name than the one locked, as a lock
        // taken through a link that took the state folder's name and left it again would hold.
        #[cfg(unix)]
        {
            let other = dir.path().join("other");
            fs::write(&other, b"").unwrap();
            let opened = fs::File::open(&other).unwrap();
            match check_lock_file(&lock_file, &opened) {
                Err(LockError::Io { path, source }) => {
                    assert_eq!(path, lock_file);
                    assert_eq!(
                        source.to_string(),
                        "the lock file was replaced while the lock was taken"
                    );
                }
                other => panic!("{other:?}"),
            }
        }
        drop(held);
        // Nothing was made by looking.
        assert_eq!(folder_names(&state).unwrap(), [LOCK_FILE]);
    }

    #[test]
    fn each_alpha_marker_is_found_unopened() {
        let upper: Vec<String> = ALPHA_NAMES
            .iter()
            .map(|name| name.to_ascii_uppercase())
            .collect();
        let upper: Vec<&str> = upper.iter().map(String::as_str).collect();
        let mut cases: Vec<(Vec<&str>, Vec<&str>)> = Vec::new();
        for (name, shouted) in ALPHA_NAMES.into_iter().zip(upper.iter().copied()) {
            cases.push((vec![name], vec![name]));
            cases.push((vec![shouted], vec![name]));
        }
        cases.push((ALPHA_NAMES.to_vec(), ALPHA_NAMES.to_vec()));
        // Made in the reverse order, in upper case: found in their own order, as they are spelt.
        let mut reversed = upper.clone();
        reversed.reverse();
        cases.push((reversed, ALPHA_NAMES.to_vec()));

        let mut proven = true;
        for (made, found) in cases {
            let dir = tempfile::tempdir().unwrap();
            make_alpha_markers(dir.path(), &made).unwrap();
            let markers: Vec<PathBuf> = made.iter().map(|name| dir.path().join(name)).collect();
            let paths: Vec<&Path> = markers.iter().map(PathBuf::as_path).collect();
            // Any open of a marker would fail now, but for root on Unix.
            let held = hold_closed(&paths).unwrap();
            proven &= held.proven();
            assert_eq!(alpha_markers(dir.path()).unwrap(), found, "{made:?}");
            drop(held);
        }
        if !proven {
            eprintln!(
                "only the listing was checked: the proof that no marker is opened needs a user \
                 other than root, who opens entries of mode 0o000"
            );
        }
    }

    #[test]
    fn each_marker_is_named_once_in_any_case() {
        let dir = tempfile::tempdir().unwrap();
        let spellings = [
            [
                "installation.json",
                "INSTALLATION.JSON",
                "Installation.Json",
            ],
            [".gdl-update", ".GDL-UPDATE", ".Gdl-Update"],
            [
                "GauntletDarkLegacy-Update.exe",
                "GAUNTLETDARKLEGACY-UPDATE.EXE",
                "gauntletdarklegacy-update.exe",
            ],
        ];
        for [first, others @ ..] in spellings {
            make_alpha_markers(dir.path(), &[first]).unwrap();
            match make_alpha_markers(dir.path(), &others) {
                Ok(()) => {}
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
                    eprintln!(
                        "skipped: this file system takes one letter case of a name for another"
                    );
                    return;
                }
                Err(error) => panic!("{error}"),
            }
        }
        assert_eq!(folder_names(dir.path()).unwrap().len(), 9);
        assert_eq!(
            alpha_markers(dir.path()).unwrap(),
            [
                "installation.json",
                ".gdl-update",
                "GauntletDarkLegacy-Update.exe"
            ]
        );
    }

    #[test]
    fn names_like_the_markers_are_not_markers() {
        let dir = tempfile::tempdir().unwrap();
        let near_misses = [
            "installation.json.bak",
            "installation",
            "installation.jsn",
            "xinstallation.json",
            ".gdl-update.lock.old",
            "gdl-update",
            ".gdl-updates",
            ".gdl-install",
            ".gdl-installer-update-1",
            "GauntletDarkLegacy-Update.exe.old",
            "GauntletDarkLegacy-Updater",
            "GauntletDarkLegacy-Launcher.exe",
            "GauntletDarkLegacy-Launcher",
            "gauntlet.exe",
        ];
        for name in near_misses {
            fs::write(dir.path().join(name), b"").unwrap();
        }
        fs::create_dir(dir.path().join(STATE_DIR)).unwrap();
        // A marker inside a folder of the installation is no marker of the installation's folder.
        let saves = dir.path().join("saves");
        fs::create_dir(&saves).unwrap();
        make_alpha_markers(&saves, &ALPHA_NAMES).unwrap();
        assert!(alpha_markers(dir.path()).unwrap().is_empty());

        // One marker among them is found alone.
        make_alpha_markers(dir.path(), &[".gdl-install.lock"]).unwrap();
        assert_eq!(alpha_markers(dir.path()).unwrap(), [".gdl-install.lock"]);

        // An empty folder holds none, and a folder that is not there, or a file, cannot be listed.
        let empty = tempfile::tempdir().unwrap();
        assert!(alpha_markers(empty.path()).unwrap().is_empty());
        let error = alpha_markers(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        assert!(alpha_markers(&dir.path().join("gauntlet.exe")).is_err());
    }

    #[test]
    fn errors_name_their_path_and_say_what_failed() {
        let path = Path::new("install").join(STATE_DIR);
        let shown = path.display();
        let denied = || io::Error::new(io::ErrorKind::PermissionDenied, "denied");

        for (error, text) in [
            (
                FolderError::Links { path: path.clone() },
                format!("Choose a directory without links or junctions: {shown}"),
            ),
            (
                FolderError::NotDirectory { path: path.clone() },
                format!("Install folder is not a directory: {shown}"),
            ),
            (
                FolderError::Absent { path: path.clone() },
                format!("Install folder does not exist: {shown}"),
            ),
            (
                FolderError::Io {
                    path: path.clone(),
                    source: denied(),
                },
                format!("{shown}: denied"),
            ),
        ] {
            assert_eq!(error.to_string(), text);
            match &error {
                FolderError::Io { .. } => assert_eq!(error.source().unwrap().to_string(), "denied"),
                _ => assert!(error.source().is_none(), "{text}"),
            }
        }

        for (error, text) in [
            (
                LockError::Busy,
                "Another job is using this installation".to_owned(),
            ),
            (
                LockError::Links { path: path.clone() },
                format!("Choose a directory without links or junctions: {shown}"),
            ),
            (
                LockError::NoStateFolder { path: path.clone() },
                format!("The installation has no launcher state folder: {shown}"),
            ),
            (
                LockError::Io {
                    path: path.clone(),
                    source: denied(),
                },
                format!("{shown}: denied"),
            ),
        ] {
            assert_eq!(error.to_string(), text);
            match &error {
                LockError::Io { .. } => assert_eq!(error.source().unwrap().to_string(), "denied"),
                _ => assert!(error.source().is_none(), "{text}"),
            }
        }

        // A link check's links and the system's refusals keep their kinds, and anything else it
        // came to report would be kept whole as the system's refusal of the folder.
        let link = Path::new("link");
        match FolderError::of_links_check(FilesError::Link { path: link.into() }, &path) {
            FolderError::Links { path: found } => assert_eq!(found, link),
            other => panic!("{other:?}"),
        }
        let refused = FilesError::Io {
            path: link.into(),
            source: denied(),
        };
        match FolderError::of_links_check(refused, &path) {
            FolderError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, link);
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        let unstaged = FilesError::NotRoot { name: "x".into() };
        match FolderError::of_links_check(unstaged, &path) {
            FolderError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, path);
                assert_eq!(source.to_string(), r#""x" is not a single staged root"#);
            }
            unexpected => panic!("{unexpected:?}"),
        }

        assert!(size_of::<FolderError>() <= MOST_ERROR_BYTES);
        assert!(size_of::<LockError>() <= MOST_ERROR_BYTES);
    }
}
