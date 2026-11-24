//! An installation's folder: where the launcher keeps its own state in it, the lock a job holds
//! while it changes the installation, and the names by which a folder the alpha installer made is
//! known. Such a folder is recognised by its names alone, so the launcher never reads, and never
//! comes to rely on, a file the alpha installer wrote.

use std::ffi::OsString;
use std::fs::{self, File, OpenOptions, TryLockError};
use std::io;
use std::path::{Path, PathBuf};

use gdl_files::{ALPHA_NAMES, FilesError, absolute_lexical, is_absent, no_links, retry_locked};

#[cfg(windows)]
use crate::platform;

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
const LOCK_FILE_REPLACED: &str = "the lock file was replaced while the lock was taken";

/// Refuses `root`, an installation's folder, when it or a folder above it is a link: the rule that
/// [`InstallFolder::at`], [`InstallFolder::lock`], [`InstallFolder::installed`] and
/// [`InstallFolder::inspect`] hold an installation's folder to. gdl-files' readers and writers
/// hold each whole path they take to the same rule through [`no_links`], the folders above the
/// installation included, so a change to what this refuses above the folder must reach them too;
/// otherwise a folder accepted here would be refused once something in it is read or written.
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
    /// following a link at its name, made when it is missing and never emptied. It is removed only
    /// by [`InstallLock::give_up`], while its lock is held: were one job to remove it while another
    /// held its lock, a third could lock a new file of the same name. An open the system refuses
    /// for a moment is tried again, as a system that keeps a removed file's name until its last
    /// handle closes refuses opens of a lock file being given up meanwhile.
    ///
    /// Once the lock is taken, the state folder and the lock file are looked at again, and the
    /// file locked must be the one at the lock file's name, by the identity the system gives a
    /// file, so that a lock taken through a link that took either name meanwhile, or on a lock file
    /// given up meanwhile, is let go and refused: a system may give a removed file's name to a new
    /// file at once, while a job that opened the old one before can still lock it. The
    /// installation's folder is then held to [`check_links`] again, since a link may have taken its
    /// name, or a name above it, after the folder was given.
    pub fn lock(&self) -> Result<InstallLock, LockError> {
        let state = self.state_dir();
        check_state_folder(&state)?;
        let path = state.join(LOCK_FILE);
        let file = open_lock_file(&state, &path)?;
        match file.try_lock() {
            Ok(()) => {}
            Err(TryLockError::WouldBlock) => return Err(LockError::Busy),
            Err(TryLockError::Error(source)) => return Err(LockError::Io { path, source }),
        }
        // Dropped, and so let go, when a second look refuses it.
        let lock = InstallLock { file, state };
        check_state_folder(&lock.state)?;
        check_lock_file(&path, &lock.file)?;
        check_links(&self.root).map_err(|error| LockError::of_links_check(error, &self.root))?;
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

/// Opens the lock file at `path`, in the state folder `state`, to read and to write, since whether
/// a file open only to read can be locked differs between systems, making it when it is missing
/// and keeping what it holds. The open never follows a link at the name: Unix refuses to open one,
/// and Windows opens the link itself, so that a link there is refused as one rather than locked
/// through. An open the system refuses for a moment is tried again, as [`retry_locked`] tries one.
fn open_lock_file(state: &Path, path: &Path) -> Result<File, LockError> {
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
    let file = retry_locked(|| options.open(path)).map_err(|source| {
        // A link refused by Unix, or a junction that Windows will not open as a file, is told
        // from any other refusal by what is at the name. Nothing above the name is a state
        // folder that went once it was looked at, as one given up meanwhile goes.
        if is_link(path) {
            LockError::Links {
                path: path.to_path_buf(),
            }
        } else if is_absent(&source) {
            LockError::NoStateFolder {
                path: state.to_path_buf(),
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

/// Refuses the lock that `file` holds unless what is at the lock file's name `path` is no link and
/// is the file that `file` has open.
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
        Err(replaced(path))
    }
}

/// Refuses the lock that `file` holds unless the entry at the lock file's name `path` is the file
/// it has open, by volume and file id, the entry opened anew as itself: std gives no identity of
/// a file on Windows to compare.
#[cfg(windows)]
fn check_same_file(path: &Path, file: &File, _: &fs::Metadata) -> Result<(), LockError> {
    match platform::is_same_file(file, path) {
        Ok(true) => Ok(()),
        Ok(false) => Err(replaced(path)),
        Err(source) => Err(LockError::Io {
            path: path.to_path_buf(),
            source,
        }),
    }
}

/// The refusal of a lock whose file is no longer the one at the lock file's name `path`.
fn replaced(path: &Path) -> LockError {
    LockError::Io {
        path: path.to_path_buf(),
        source: io::Error::other(LOCK_FILE_REPLACED),
    }
}

/// Whether a link is at `path`, looked at without following it.
fn is_link(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_symlink())
}

/// An installation's lock, held until this is dropped or given up.
#[derive(Debug)]
#[must_use = "dropping it releases the lock at once"]
pub struct InstallLock {
    /// The lock file, open, which holds the lock.
    file: File,
    /// The state folder that holds the lock file. The lock keeps it, rather than being told it
    /// again, so that it can give up no state folder but its own.
    state: PathBuf,
}

impl InstallLock {
    /// Gives the installation's state folder up with its lock when the folder holds nothing but
    /// the lock file and an empty journal folder: how a job that made the state folder takes it
    /// away again. Gives `true` when the state folder is gone. Anything else in it is not the
    /// launcher's to remove, and keeps it whole, as does a lock file that another job made again
    /// meanwhile, whose folder is left to that job: both give `false`. Either way the lock is let
    /// go.
    ///
    /// When a first listing finds nothing else, the journal folder goes, only when it is a real
    /// folder and empty. The state folder is then listed again, and the lock file, alone in it, is
    /// removed while its lock is still held, so that no job ever removes a lock file whose lock it
    /// does not hold; then the lock is let go and the state folder removed, which fails, and keeps
    /// it, when another job made the lock file again in between. A name already gone counts as
    /// removed, so that a give-up cut short is finished by the next. Nothing is removed through a
    /// link: the state folder and every folder above it are held to [`no_links`] first, and the
    /// lock file must be no link and must be the file locked, so that a job that locked a file
    /// given up before it never removes, in its place, a lock file another job holds.
    pub fn give_up(self) -> Result<bool, FilesError> {
        let state = self.state.clone();
        no_links(&state)?;
        let Some(names) = state_names(&state)? else {
            return Ok(true);
        };
        if names
            .iter()
            .any(|name| name.as_os_str() != LOCK_FILE && name.as_os_str() != JOURNAL_DIR)
        {
            return Ok(false);
        }
        if names.iter().any(|name| name.as_os_str() == JOURNAL_DIR)
            && !remove_empty_folder(&state.join(JOURNAL_DIR))?
        {
            return Ok(false);
        }
        let Some(names) = state_names(&state)? else {
            return Ok(true);
        };
        if names.iter().any(|name| name.as_os_str() != LOCK_FILE) {
            return Ok(false);
        }
        if !names.is_empty() {
            let lock_file = state.join(LOCK_FILE);
            match check_lock_file(&lock_file, &self.file) {
                Ok(()) => match retry_locked(|| fs::remove_file(&lock_file)) {
                    Ok(()) => {}
                    Err(error) if is_absent(&error) => {}
                    Err(source) => {
                        return Err(FilesError::Io {
                            path: lock_file,
                            source,
                        });
                    }
                },
                Err(LockError::Io { source, .. }) if is_absent(&source) => {}
                Err(error) => return Err(give_up_error(error, &lock_file)),
            }
        }
        // Let go only once its file has no name, so that whoever opens the name from now on opens
        // another file, and whoever opened this one before finds its name gone.
        drop(self);
        match retry_locked(|| fs::remove_dir(&state)) {
            Ok(()) => Ok(true),
            Err(error) if is_absent(&error) => Ok(true),
            Err(error) if error.kind() == io::ErrorKind::DirectoryNotEmpty => Ok(false),
            Err(source) => Err(FilesError::Io {
                path: state,
                source,
            }),
        }
    }
}

/// The names the state folder `state` holds, as [`names_in`] lists them, or `None` when it is
/// gone.
fn state_names(state: &Path) -> Result<Option<Vec<OsString>>, FilesError> {
    match names_in(state) {
        Ok(names) => Ok(Some(names)),
        Err(error) if is_absent(&error) => Ok(None),
        Err(source) => Err(FilesError::Io {
            path: state.to_path_buf(),
            source,
        }),
    }
}

/// Removes the folder at `path` when it is a real folder and holds nothing, and says whether it is
/// gone: `false` when it holds something, or when a file or a link has its name, which is left as
/// it is.
fn remove_empty_folder(path: &Path) -> Result<bool, FilesError> {
    let io = |source| FilesError::Io {
        path: path.to_path_buf(),
        source,
    };
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.is_dir() => {}
        Ok(_) => return Ok(false),
        Err(error) if is_absent(&error) => return Ok(true),
        Err(source) => return Err(io(source)),
    }
    match retry_locked(|| fs::remove_dir(path)) {
        Ok(()) => Ok(true),
        Err(error) if is_absent(&error) => Ok(true),
        Err(error) if error.kind() == io::ErrorKind::DirectoryNotEmpty => Ok(false),
        Err(source) => Err(io(source)),
    }
}

/// The failure of a give-up for what the second look at the lock file `lock_file` refused: a link
/// and the system's refusals keep their kinds, and anything else it came to report would count as
/// the system's refusal of the lock file.
fn give_up_error(error: LockError, lock_file: &Path) -> FilesError {
    match error {
        LockError::Links { path } => FilesError::Link { path },
        LockError::Io { path, source } => FilesError::Io { path, source },
        other => FilesError::Io {
            path: lock_file.to_path_buf(),
            source: io::Error::other(other),
        },
    }
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
    Ok(markers_among(&names_in(root)?))
}

/// The alpha installer's names among `names`, the names a folder holds, as [`alpha_markers`]
/// gives them.
pub(crate) fn markers_among(names: &[OsString]) -> Vec<&'static str> {
    let mut held = [false; ALPHA_NAMES.len()];
    for name in names {
        if let Some(index) = ALPHA_NAMES
            .iter()
            .position(|&alpha| name.eq_ignore_ascii_case(alpha))
        {
            held[index] = true;
        }
    }
    ALPHA_NAMES
        .into_iter()
        .zip(held)
        .filter_map(|(name, held)| held.then_some(name))
        .collect()
}

/// The names the folder `path` holds, in the order the system lists them. The folder is only
/// listed: no entry in it is opened or looked at.
pub(crate) fn names_in(path: &Path) -> io::Result<Vec<OsString>> {
    fs::read_dir(path)?
        .map(|entry| entry.map(|entry| entry.file_name()))
        .collect()
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
    /// `path` is a link: the installation's folder, a folder above it, the state folder or the
    /// lock file.
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
    /// The system refused to look at, open or lock `path`; or the file locked was no longer the
    /// one at the lock file's name, and the lock was let go.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The state folder or the lock file.
        path: PathBuf,
        /// What the system answered, or why the lock was let go.
        #[source]
        source: io::Error,
    },
}

impl LockError {
    /// The refusal of the lock of the installation at `root` for what its link check reported. A
    /// link check reports links and the system's refusals; anything else it reported would count
    /// as the latter.
    fn of_links_check(error: FilesError, root: &Path) -> LockError {
        match error {
            FilesError::Link { path } => LockError::Links { path },
            FilesError::Io { path, source } => LockError::Io { path, source },
            other => LockError::Io {
                path: root.to_path_buf(),
                source: io::Error::other(other),
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::thread::{self, ScopedJoinHandle};
    use std::time::{Duration, Instant};

    use gdl_files::{ALPHA_NAMES, FilesError};
    use gdl_fixtures::{
        backdate, folder_names, hold_closed, holds_bind, link_folder, make_alpha_markers, snapshot,
        symlink_file, symlink_file_where_allowed,
    };
    use tempfile::TempDir;

    use super::{
        FolderError, InstallFolder, InstallLock, JOURNAL_DIR, LOCK_FILE, LockError, RECEIPT_FILE,
        STATE_DIR, alpha_markers, check_links, check_lock_file, check_state_folder, give_up_error,
        open_lock_file,
    };

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// How many times a test takes the lock again the moment it is released.
    const TAKES_IN_A_ROW: usize = 100;

    /// How many state folders a test gives up while another thread tries their locks throughout.
    const GIVE_UP_ROUNDS: usize = 25;

    /// How many attempts the other thread makes after a give-up returns before it stops, so that
    /// it tries the lock both during the give-up and once it is over.
    const ATTEMPTS_AFTER: usize = 3;

    /// The longest a test waits for the other thread's next attempt.
    const ATTEMPT_WAIT: Duration = Duration::from_secs(30);

    /// ERROR_LOCK_VIOLATION: what another handle's read of a locked file gives on Windows.
    #[cfg(windows)]
    const LOCK_VIOLATION: i32 = 33;

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

    /// Checks that `lock`, just taken, is on the file at the lock file's name `lock_file`, and that
    /// the state folder `state` that holds it is there.
    #[track_caller]
    fn assert_held_at_its_name(lock: &InstallLock, state: &Path, lock_file: &Path) {
        assert!(fs::symlink_metadata(state).unwrap().is_dir());
        // The file at the name is the one locked, by the identity the system gives a file.
        check_lock_file(lock_file, &lock.file).unwrap();
        // Windows also refuses another handle's read of a locked file. Once this lock is taken,
        // the only other lock is the one given up, on a file that the name no longer leads to,
        // so a refused read shows that the name leads to this one.
        #[cfg(windows)]
        {
            let error = fs::read(lock_file).unwrap_err();
            assert_eq!(error.raw_os_error(), Some(LOCK_VIOLATION), "{error}");
        }
    }

    /// Sets a flag when dropped, so that a thread told to stop by it stops however the test ends.
    struct StopOnDrop<'a>(&'a AtomicBool);

    impl Drop for StopOnDrop<'_> {
        fn drop(&mut self) {
            self.0.store(true, Ordering::SeqCst);
        }
    }

    /// Waits until `attempts` reaches `least`, or until `other` has ended, as it ends only by
    /// failing, whose failure joining it then reports.
    #[track_caller]
    fn wait_for_attempts(attempts: &AtomicUsize, least: usize, other: &ScopedJoinHandle<'_, ()>) {
        let deadline = Instant::now() + ATTEMPT_WAIT;
        while attempts.load(Ordering::SeqCst) < least && !other.is_finished() {
            assert!(
                Instant::now() < deadline,
                "the other thread made no attempt in time"
            );
            thread::yield_now();
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
        if !symlink_file_where_allowed(&lock_file, &target) {
            eprintln!("skipped the links to files: this account may not make them");
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

        // Nor when the state folder goes once it was looked at, as one given up meanwhile does:
        // the lock file's open finds nothing above its name.
        let (_dir, folder) = bare_installation();
        let state = folder.state_dir();
        match open_lock_file(&state, &state.join(LOCK_FILE)) {
            Err(LockError::NoStateFolder { path }) => assert_eq!(path, state),
            other => panic!("{other:?}"),
        }
        assert!(folder_names(folder.root()).unwrap().is_empty());
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

        // So does another file at the lock file's name than the one locked, as a lock taken
        // through a link that took the state folder's name and left it again would hold.
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
        drop(held);
        // Nothing was made by looking.
        assert_eq!(folder_names(&state).unwrap(), [LOCK_FILE]);
    }

    #[test]
    fn a_lock_taken_through_a_linked_root_is_refused() {
        // The installation's folder, and the folder above it, each moved away once the folder was
        // taken, and a link to where it went put in its place: on Windows a junction.
        for linked_above in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let above = dir.path().join("above");
            let root = above.join("install");
            fs::create_dir_all(root.join(STATE_DIR)).unwrap();
            let folder = InstallFolder::at(&root).unwrap();
            let link = if linked_above { above } else { root };
            let moved = dir.path().join("moved");
            fs::rename(&link, &moved).unwrap();
            link_folder(&link, &moved).unwrap();
            assert_lock_links(folder.lock(), &link);

            // The lock taken through the link was let go: the installation where the link leads
            // is locked at once, and holds nothing new but the lock file.
            let real = if linked_above {
                moved.join("install")
            } else {
                moved
            };
            let real = InstallFolder::at(&real).unwrap();
            drop(real.lock().unwrap());
            assert_eq!(folder_names(&real.state_dir()).unwrap(), [LOCK_FILE]);
        }
    }

    #[test]
    fn give_up_removes_only_a_state_folder_of_a_lock_alone() {
        // The lock file alone, and beside an empty journal folder: the state folder goes, and the
        // installation's folder is left, holding nothing.
        for with_journal in [false, true] {
            let (_dir, folder) = installation();
            let state = folder.state_dir();
            if with_journal {
                fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
            }
            assert!(folder.lock().unwrap().give_up().unwrap(), "{with_journal}");
            assert!(folder_names(folder.root()).unwrap().is_empty());
            // No lock is taken where the state folder was, and nothing is made in its place.
            assert_no_state_folder(folder.lock(), &state);
            assert!(folder_names(folder.root()).unwrap().is_empty());
        }

        // Anything else keeps the state folder whole, the lock file and an empty journal folder
        // included: the installation's receipt, a file of the player's, a staging, a journal
        // folder holding a journal, and a file or a link in the journal folder's place.
        type Make = fn(&Path, &Path);
        let cases: [(&str, Make); 7] = [
            ("a receipt", |state, _| {
                fs::write(state.join(RECEIPT_FILE), b"{}").unwrap();
            }),
            ("a note", |state, _| {
                fs::write(state.join("notes.txt"), b"mine").unwrap();
            }),
            ("a note beside an empty journal folder", |state, _| {
                fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
                fs::write(state.join("notes.txt"), b"mine").unwrap();
            }),
            ("a staging", |state, _| {
                fs::create_dir(state.join("install-AbC123xy")).unwrap();
            }),
            ("a journal", |state, _| {
                fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
                fs::write(state.join(JOURNAL_DIR).join("install.json"), b"{}").unwrap();
            }),
            ("a file in the journal folder's place", |state, _| {
                fs::write(state.join(JOURNAL_DIR), b"").unwrap();
            }),
            (
                "a link in the journal folder's place",
                |state, elsewhere| {
                    link_folder(&state.join(JOURNAL_DIR), elsewhere).unwrap();
                },
            ),
        ];
        for (case, make) in cases {
            let (dir, folder) = installation();
            let elsewhere = dir.path().join("elsewhere");
            fs::create_dir(&elsewhere).unwrap();
            drop(folder.lock().unwrap());
            make(&folder.state_dir(), &elsewhere);
            // Backdated, so that a name made and removed again would show.
            backdate(dir.path()).unwrap();
            let before = snapshot(dir.path()).unwrap();
            assert!(!folder.lock().unwrap().give_up().unwrap(), "{case}");
            // Nothing was removed, made or written, and the lock was let go.
            assert_eq!(snapshot(dir.path()).unwrap(), before, "{case}");
            drop(folder.lock().unwrap());
        }
    }

    #[test]
    fn give_up_removes_nothing_through_a_link_or_at_another_file() {
        // A link in the lock file's place, the file locked having been moved out of the state
        // folder: refused, the link left, and nothing made where it leads.
        let (dir, folder) = installation();
        let state = folder.state_dir();
        let lock_file = state.join(LOCK_FILE);
        let held = folder.lock().unwrap();
        fs::rename(&lock_file, dir.path().join("moved")).unwrap();
        let elsewhere = dir.path().join("elsewhere");
        fs::create_dir(&elsewhere).unwrap();
        link_folder(&lock_file, &elsewhere).unwrap();
        match held.give_up() {
            Err(FilesError::Link { path }) => assert_eq!(path, lock_file),
            other => panic!("{other:?}"),
        }
        assert!(
            fs::symlink_metadata(&lock_file)
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert!(folder_names(&elsewhere).unwrap().is_empty());
        assert_eq!(folder_names(&state).unwrap(), [LOCK_FILE]);

        // On Unix, the installation's folder moved away while the lock was held, and a link put in
        // its place: refused before anything is listed or removed through the link. Windows
        // refuses to rename a folder while a file in it is open, so no link can take the folder's
        // name while its lock is held.
        #[cfg(unix)]
        {
            let (dir, folder) = installation();
            let held = folder.lock().unwrap();
            let moved = dir.path().join("moved");
            fs::rename(folder.root(), &moved).unwrap();
            link_folder(folder.root(), &moved).unwrap();
            match held.give_up() {
                Err(FilesError::Link { path }) => assert_eq!(path, folder.root()),
                other => panic!("{other:?}"),
            }
            assert_eq!(folder_names(&moved.join(STATE_DIR)).unwrap(), [LOCK_FILE]);
        }
        #[cfg(windows)]
        {
            let (dir, folder) = installation();
            let held = folder.lock().unwrap();
            let error = fs::rename(folder.root(), dir.path().join("moved")).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied, "{error}");
            assert!(held.give_up().unwrap());
        }

        // Another file at the lock file's name than the one locked is refused and left, as it may
        // hold another job's lock; the file locked was moved away while held, which both systems
        // allow.
        let (dir, folder) = installation();
        let lock_file = folder.state_dir().join(LOCK_FILE);
        let held = folder.lock().unwrap();
        fs::rename(&lock_file, dir.path().join("moved")).unwrap();
        fs::write(&lock_file, b"another").unwrap();
        match held.give_up() {
            Err(FilesError::Io { path, source }) => {
                assert_eq!(path, lock_file);
                assert_eq!(
                    source.to_string(),
                    "the lock file was replaced while the lock was taken"
                );
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(fs::read(&lock_file).unwrap(), b"another");
        assert_eq!(folder_names(&folder.state_dir()).unwrap(), [LOCK_FILE]);
    }

    #[test]
    fn a_lock_on_a_file_given_up_meanwhile_is_let_go() {
        let (_dir, folder) = installation();
        let state = folder.state_dir();
        let lock_file = state.join(LOCK_FILE);
        let third = InstallFolder::at(folder.root()).unwrap();
        let held = folder.lock().unwrap();
        // A second launcher opens the lock file, as a lock begins, just before the holder gives
        // it up and removes its name while holding its lock.
        let opened = open_lock_file(&state, &lock_file).unwrap();
        fs::remove_file(&lock_file).unwrap();
        if fs::symlink_metadata(&lock_file).is_ok() {
            // There the name stays until the last handle closes, and every newcomer's open of it
            // is refused until then.
            eprintln!("skipped: this file system keeps a removed file's name while it is open");
            return;
        }
        // A third launcher makes the lock file again and takes the lock on it, and the holder
        // lets go of the file whose name is gone.
        let taken = third.lock().unwrap();
        drop(held);
        // The second launcher locks the file it opened, which nobody holds now, but its second
        // look finds another file at the name and lets it go.
        opened.try_lock().unwrap();
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
        // Were it to give the state folder up regardless, the third launcher's lock file, which
        // it holds, would be left where it is.
        let orphan = InstallLock {
            file: opened,
            state: state.clone(),
        };
        match orphan.give_up() {
            Err(FilesError::Io { path, source }) => {
                assert_eq!(path, lock_file);
                assert_eq!(
                    source.to_string(),
                    "the lock file was replaced while the lock was taken"
                );
            }
            other => panic!("{other:?}"),
        }
        assert_held_at_its_name(&taken, &state, &lock_file);
        assert!(matches!(folder.lock(), Err(LockError::Busy)));
        assert_eq!(folder_names(&state).unwrap(), [LOCK_FILE]);
        drop(taken);
    }

    #[test]
    fn give_up_never_leaves_a_lock_another_holds() {
        let mut kept = 0;
        for round in 0..GIVE_UP_ROUNDS {
            let (_dir, folder) = installation();
            let state = folder.state_dir();
            let lock_file = state.join(LOCK_FILE);
            let other = InstallFolder::at(folder.root()).unwrap();
            let held = folder.lock().unwrap();
            let attempts = AtomicUsize::new(0);
            let stop = AtomicBool::new(false);
            let given_up = thread::scope(|scope| {
                // Stops the other thread however this one leaves the scope, which waits for it.
                let _stop = StopOnDrop(&stop);
                // Another job's attempts, each letting go at once of a lock it took.
                let taker = scope.spawn(|| {
                    while !stop.load(Ordering::SeqCst) {
                        if let Ok(lock) = other.lock() {
                            assert_held_at_its_name(&lock, &state, &lock_file);
                            drop(lock);
                        }
                        attempts.fetch_add(1, Ordering::SeqCst);
                    }
                });
                wait_for_attempts(&attempts, 1, &taker);
                let given_up = held.give_up();
                let after = attempts.load(Ordering::SeqCst);
                wait_for_attempts(&attempts, after + ATTEMPTS_AFTER, &taker);
                stop.store(true, Ordering::SeqCst);
                if let Err(failure) = taker.join() {
                    std::panic::resume_unwind(failure);
                }
                given_up
            });
            let given_up = given_up.unwrap_or_else(|error| panic!("round {round}: {error}"));
            if given_up {
                assert!(fs::symlink_metadata(&state).is_err(), "round {round}");
            } else {
                // Kept only for a lock file the other made again, now free: the folder holds it
                // alone, or nothing where the system kept the given-up file's name until the
                // other's handle on it closed, and is given up once nobody else tries it.
                kept += 1;
                let names = folder_names(&state).unwrap();
                assert!(
                    names.is_empty() || names == [LOCK_FILE],
                    "round {round}: {names:?}"
                );
                assert!(folder.lock().unwrap().give_up().unwrap(), "round {round}");
            }
            assert!(
                folder_names(folder.root()).unwrap().is_empty(),
                "round {round}"
            );
        }
        eprintln!("{kept} of {GIVE_UP_ROUNDS} state folders kept for a lock file made again");
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

        // Asked apart from the holds, so that a hold which kept nothing out fails the test.
        let probe = tempfile::tempdir().unwrap();
        let binds = holds_bind(probe.path()).unwrap();
        for (made, found) in cases {
            let dir = tempfile::tempdir().unwrap();
            make_alpha_markers(dir.path(), &made).unwrap();
            let markers: Vec<PathBuf> = made.iter().map(|name| dir.path().join(name)).collect();
            let paths: Vec<&Path> = markers.iter().map(PathBuf::as_path).collect();
            // Any open of a marker would fail now, but for root on Unix.
            let held = hold_closed(&paths).unwrap();
            assert_eq!(held.proven(), binds, "{made:?}");
            assert_eq!(alpha_markers(dir.path()).unwrap(), found, "{made:?}");
            drop(held);
            // On Unix nothing in a folder that may only be listed can be looked at, so markers
            // found there were found by their names alone, with no metadata of any. Windows lets
            // every process read an entry's metadata, held or not.
            #[cfg(unix)]
            {
                let listed = gdl_fixtures::DeniedAccess::listing_only(&[dir.path()]).unwrap();
                let markers = alpha_markers(dir.path());
                drop(listed);
                assert_eq!(markers.unwrap(), found, "{made:?} in a folder only listed");
            }
        }
        if !binds {
            eprintln!(
                "only the listing was checked: the proof that no marker is opened or looked at \
                 needs a user other than root, who opens entries of mode 0o000"
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

        // A lock refused by the link check of its installation's folder keeps the same kinds.
        match LockError::of_links_check(FilesError::Link { path: link.into() }, &path) {
            LockError::Links { path: found } => assert_eq!(found, link),
            other => panic!("{other:?}"),
        }
        let refused = FilesError::Io {
            path: link.into(),
            source: denied(),
        };
        match LockError::of_links_check(refused, &path) {
            LockError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, link);
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        let unstaged = FilesError::NotRoot { name: "x".into() };
        match LockError::of_links_check(unstaged, &path) {
            LockError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, path);
                assert_eq!(source.to_string(), r#""x" is not a single staged root"#);
            }
            unexpected => panic!("{unexpected:?}"),
        }

        // A give-up refused by the second look at its lock file keeps a link and the system's
        // refusal as they were, and would keep anything else whole as the system's refusal.
        let lock_file = path.join(LOCK_FILE);
        match give_up_error(LockError::Links { path: link.into() }, &lock_file) {
            FilesError::Link { path: found } => assert_eq!(found, link),
            other => panic!("{other:?}"),
        }
        let refused = LockError::Io {
            path: link.into(),
            source: denied(),
        };
        match give_up_error(refused, &lock_file) {
            FilesError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, link);
                assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
            }
            other => panic!("{other:?}"),
        }
        match give_up_error(LockError::Busy, &lock_file) {
            FilesError::Io {
                path: found,
                source,
            } => {
                assert_eq!(found, lock_file);
                assert_eq!(source.to_string(), "Another job is using this installation");
            }
            other => panic!("{other:?}"),
        }

        assert!(size_of::<FolderError>() <= MOST_ERROR_BYTES);
        assert!(size_of::<LockError>() <= MOST_ERROR_BYTES);
    }
}
