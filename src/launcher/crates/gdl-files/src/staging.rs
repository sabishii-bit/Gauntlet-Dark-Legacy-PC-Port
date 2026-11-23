//! Staging: a folder of the launcher's own, made new inside the folder it installs into, where a
//! job makes everything it will put in place, and from which each finished root is put in place
//! by a rename that never replaces anything. What a job leaves in it goes when it is dropped,
//! unless the job keeps it for the journal that names it, which takes it back by its name.

use std::collections::BTreeSet;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io;
use std::path::{Path, PathBuf};

#[cfg(unix)]
use crate::rename::{SyncFolder, folder_of, sync_folder};
use crate::{
    FilesError, PAYLOAD_NAMES, RelPath, check_component, no_links, rename_noreplace, retry_locked,
};

/// How many random ASCII letters and digits follow a staging folder's prefix, as in
/// `install-AbC123xy`.
pub const STAGING_RANDOM_CHARS: usize = 8;

/// Where a publish records each root once it is in place, so that a job stopped part-way through
/// knows which roots moved.
pub trait Journal {
    /// `root` has been renamed into place. An error stops the publish.
    fn published(&mut self, root: &RelPath) -> Result<(), FilesError>;
}

/// A folder made new inside the folder a job installs into, on the same volume, where the job
/// makes what it will publish. Every name made in it has passed its kind's rules, every file is
/// made new and no file or folder is made through a link. Dropping it removes the folder with
/// whatever it still holds, unless it is kept: a job keeps the staging its journal names, so that
/// whatever stops the job, a panic included, leaves the folder for the journal's recovery, and
/// then removes it itself once the journal no longer needs it.
#[derive(Debug)]
pub struct Staging {
    /// The folder, absolute.
    path: PathBuf,
    /// Whether the folder stays when the staging is dropped.
    kept: bool,
}

impl Staging {
    /// Makes a new staging folder in `dest`, which must have no link on its path, named `prefix`
    /// and then [`STAGING_RANDOM_CHARS`] random ASCII letters and digits. The prefix is one of the
    /// launcher's own, such as `install-`: one ASCII name that [`check_component`] passes, so that
    /// the staging lies in `dest` itself. Any other prefix is refused before anything is made, as
    /// an [`io::ErrorKind::InvalidInput`] error at `dest`. The staging is not kept.
    pub fn create_in(dest: &Path, prefix: &str) -> Result<Staging, FilesError> {
        check_prefix(prefix).map_err(|refused| FilesError::io(dest, refused))?;
        no_links(dest)?;
        let path = tempfile::Builder::new()
            .prefix(prefix)
            .rand_bytes(STAGING_RANDOM_CHARS)
            .tempdir_in(dest)
            .map_err(|source| FilesError::io(dest, source))?
            .keep();
        Ok(Staging { path, kept: false })
    }

    /// Takes back the staging folder `name` in `dest`, one that [`create_in`](Staging::create_in)
    /// made there with `prefix` and that a journal names. The prefix must be one `create_in` takes,
    /// and `name` that prefix followed by [`STAGING_RANDOM_CHARS`] ASCII letters and digits: any
    /// other is refused before anything is looked at, as an [`io::ErrorKind::InvalidInput`] error
    /// at `dest/name`. Neither the folder nor any folder above it may be a link, and the folder
    /// must be there and be a real folder: nothing there is the system's
    /// [`io::ErrorKind::NotFound`], which a job undoing what it made counts as a staging already
    /// removed. The staging comes back kept, since a journal names it.
    pub fn reopen(dest: &Path, prefix: &str, name: &str) -> Result<Staging, FilesError> {
        // Made absolute as a staging made in `dest` is, so that both name one folder alike.
        let path = if dest.is_absolute() {
            dest.join(name)
        } else {
            std::env::current_dir()
                .map_err(|source| FilesError::io(dest, source))?
                .join(dest)
                .join(name)
        };
        check_prefix(prefix)
            .and_then(|()| check_staging_name(prefix, name))
            .map_err(|refused| FilesError::io(&path, refused))?;
        no_links(&path)?;
        refuse_unless_folder(&path)?;
        Ok(Staging { path, kept: true })
    }

    /// The staging folder's name, such as `install-AbC123xy`: what a journal records, to take the
    /// staging back by with [`reopen`](Staging::reopen).
    pub fn name(&self) -> &str {
        self.path
            .file_name()
            .and_then(OsStr::to_str)
            .expect("a staging's name is its ASCII prefix and random part")
    }

    /// The staging folder, absolute.
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Keeps the folder when the staging is dropped. A job keeps its staging from the moment its
    /// journal names it, so that whatever stops the job leaves the folder for the journal's
    /// recovery; only [`remove`](Staging::remove) takes it away then.
    pub fn keep(&mut self) {
        self.kept = true;
    }

    /// Removes the staging folder with whatever it still holds, kept or not, and reports what
    /// stopped the removal, which a drop passes over. A link in it goes as the link alone: what it
    /// leads to is never followed. On Windows a removal that a scanner's hold on a file refuses is
    /// tried again as [`retry_locked`] tries it. What a failed removal leaves stays where it is:
    /// the staging is not removed again as it goes.
    pub fn remove(mut self) -> Result<(), FilesError> {
        self.kept = true;
        retry_locked(|| fs::remove_dir_all(&self.path))
            .map_err(|source| FilesError::io(&self.path, source))
    }

    /// Makes the folder `path` in the staging, and each folder above it, one at a time, where it
    /// is not there yet. Each must then be a real folder: a link is refused, and so is a file.
    pub fn create_dir(&self, path: &RelPath) -> Result<(), FilesError> {
        make_folders(&path.path_under(self.path()), path.depth())
    }

    /// Makes the file `path` in the staging, new, for writing, with the folders above it made as
    /// [`create_dir`](Staging::create_dir) makes them. Anything already at the name refuses it, a
    /// link included, which is never followed, and the refusal is
    /// [`io::ErrorKind::AlreadyExists`] without a system error code, the same on every system,
    /// whatever holds the name. Close the file before publishing the root it is in: Windows
    /// refuses to rename a folder while a file inside it is open.
    pub fn create_file(&self, path: &RelPath) -> Result<File, FilesError> {
        let file = path.path_under(self.path());
        make_folders(file.parent().unwrap_or(self.path()), path.depth() - 1)?;
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&file)
            .map_err(|source| {
                // Windows answers a name that a folder or a junction holds as access denied.
                let taken = fs::symlink_metadata(&file).is_ok();
                FilesError::io(&file, if taken { name_taken() } else { source })
            })
    }

    /// Puts each of `roots` in place in the folder `into`, by a rename that never replaces what
    /// is there, and records each in `journal` once it has moved. The caller holds the lock of
    /// `into`, and has closed every file it made in the staging.
    ///
    /// Before anything moves, each root must be a single name that the staging holds, exactly as
    /// it is written there, named once, exactly, and neither the staged tree nor `into`'s path may
    /// hold a link. Roots that differ only in letter case are two roots: a file system that keeps
    /// such names apart has staged both, and on one that folds letter case the staging holds only
    /// one spelling, so the other is no staged name. The roots then move in byte order, and the
    /// first failure stops the publish and is returned: a target that is there already is
    /// [`FilesError::Exists`]. The roots moved before it stay in place and recorded, and a root
    /// that moved but whose sync or record then failed stays in place unrecorded. Those not moved
    /// stay in the staging, for the caller's journal to decide whether they are moved too or what
    /// moved is taken back with [`unpublish`](Staging::unpublish), until the staging goes.
    pub fn publish_new(
        &self,
        roots: &[RelPath],
        into: &Path,
        journal: &mut dyn Journal,
    ) -> Result<(), FilesError> {
        // A name the staging holds at its top has no separator, so a deeper name is never one.
        let staged = self.staged_names()?;
        let mut named = BTreeSet::new();
        for root in roots {
            let held = staged.contains(OsStr::new(root.as_str()));
            if !held || !named.insert(root.as_str()) {
                return Err(FilesError::NotRoot {
                    name: root.as_str().into(),
                });
            }
        }
        refuse_links_in(self.path())?;
        no_links(into)?;
        let mut order: Vec<&RelPath> = roots.iter().collect();
        order.sort();
        for root in order {
            let target = root.path_under(into);
            rename_noreplace(&root.path_under(self.path()), &target)
                .map_err(|source| moved_to(&target, source))?;
            journal.published(root)?;
        }
        Ok(())
    }

    /// Takes the root `root` back into the staging from the folder `from`, where
    /// [`publish_new`](Staging::publish_new) put it, by a rename that never replaces what is there,
    /// as a job undoes a publish it will not finish. The caller holds the lock of `from`.
    ///
    /// `root` must be a single name, else [`FilesError::NotRoot`], and neither `from`'s path nor
    /// the staging's may hold a link. A name the staging holds again refuses the move as
    /// [`FilesError::Exists`] at the staged path, and any other failure, a root not at `from`
    /// included, is the system's, reported for the staged path too.
    pub fn unpublish(&self, root: &RelPath, from: &Path) -> Result<(), FilesError> {
        if root.depth() != 1 {
            return Err(FilesError::NotRoot {
                name: root.as_str().into(),
            });
        }
        no_links(from)?;
        no_links(self.path())?;
        let staged = root.path_under(self.path());
        rename_noreplace(&root.path_under(from), &staged)
            .map_err(|source| moved_to(&staged, source))
    }

    /// Syncs every folder of the staged tree, the staging's own included, each after every folder
    /// below it, and then the folder that holds the staging, so that a crash once the job has gone
    /// on cannot lose a name made in one, nor the staging's own name: on Unix a folder's entries
    /// reach the disk only when the folder itself is synced, and syncing a folder does not make
    /// its entry in the folder above it durable. A file system that cannot sync folders counts
    /// its folders as synced, as [`rename_noreplace`] counts them. No link is followed. On Windows
    /// this does nothing: NTFS journals its folders, and a folder opened only to read cannot be
    /// flushed.
    pub fn sync_folders(&self) -> Result<(), FilesError> {
        #[cfg(unix)]
        return sync_staged(self.path(), &File::sync_all);
        #[cfg(windows)]
        return Ok(());
    }

    /// The names at the top of the staging, as they are written there.
    fn staged_names(&self) -> Result<BTreeSet<OsString>, FilesError> {
        let folder = self.path();
        let entries = fs::read_dir(folder).map_err(|source| FilesError::io(folder, source))?;
        entries
            .map(|entry| {
                entry
                    .map(|entry| entry.file_name())
                    .map_err(|source| FilesError::io(folder, source))
            })
            .collect()
    }
}

impl Drop for Staging {
    fn drop(&mut self) {
        if !self.kept {
            // A drop cannot report a failure: a job that must know removes the staging itself.
            let _ = fs::remove_dir_all(&self.path);
        }
    }
}

/// Refuses a staging prefix that is not one safe name, as an [`io::ErrorKind::InvalidInput`]
/// error, since it would put the staging beside the folder it is made in, under it or in its
/// place.
fn check_prefix(prefix: &str) -> io::Result<()> {
    check_component(prefix, &PAYLOAD_NAMES)
        .map_err(|refused| io::Error::new(io::ErrorKind::InvalidInput, refused))
}

/// Refuses `name` unless it is `prefix` followed by a staging's random part, as an
/// [`io::ErrorKind::InvalidInput`] error.
fn check_staging_name(prefix: &str, name: &str) -> io::Result<()> {
    if name.strip_prefix(prefix).is_some_and(is_random_part) {
        return Ok(());
    }
    Err(io::Error::new(
        io::ErrorKind::InvalidInput,
        format!("{name:?} is not {prefix:?} and {STAGING_RANDOM_CHARS} ASCII letters and digits"),
    ))
}

/// Whether `part` is a staging's random part: [`STAGING_RANDOM_CHARS`] ASCII letters and digits.
fn is_random_part(part: &str) -> bool {
    part.len() == STAGING_RANDOM_CHARS && part.bytes().all(|byte| byte.is_ascii_alphanumeric())
}

/// What a rename to `target` that the system refused reports: a target that is there already
/// as [`FilesError::Exists`], anything else as the system's error for `target`.
fn moved_to(target: &Path, source: io::Error) -> FilesError {
    if source.kind() == io::ErrorKind::AlreadyExists {
        FilesError::Exists {
            path: target.to_path_buf(),
        }
    } else {
        FilesError::io(target, source)
    }
}

/// The refusal of a name that something holds already: a kind of its own and no system code, the
/// same on every system.
fn name_taken() -> io::Error {
    io::Error::new(io::ErrorKind::AlreadyExists, "the name is taken")
}

/// Makes `path` and the folders above it, `count` folders in all, outermost first, as
/// [`make_folder`] makes each.
fn make_folders(path: &Path, count: usize) -> Result<(), FilesError> {
    let folders: Vec<&Path> = path.ancestors().take(count).collect();
    folders.into_iter().rev().try_for_each(make_folder)
}

/// Makes the folder `folder` when it is not there, then refuses it unless it is a real folder:
/// what was there already may be a link, or a file.
fn make_folder(folder: &Path) -> Result<(), FilesError> {
    match fs::create_dir(folder) {
        Err(error) if error.kind() != io::ErrorKind::AlreadyExists => {
            return Err(FilesError::io(folder, error));
        }
        _ => {}
    }
    refuse_unless_folder(folder)
}

/// Refuses `folder` unless it is a real folder, looked at without following a link: a link is
/// [`FilesError::Link`], a file [`io::ErrorKind::NotADirectory`], and nothing there the system's
/// error.
fn refuse_unless_folder(folder: &Path) -> Result<(), FilesError> {
    let metadata = fs::symlink_metadata(folder).map_err(|source| FilesError::io(folder, source))?;
    if metadata.file_type().is_symlink() {
        Err(FilesError::Link {
            path: folder.to_path_buf(),
        })
    } else if metadata.is_dir() {
        Ok(())
    } else {
        Err(FilesError::io(folder, io::ErrorKind::NotADirectory.into()))
    }
}

/// Refuses `top` when it, or a folder above it, is a link, and when anything inside it is one.
/// Every entry is looked at as it is, and no link is ever followed.
fn refuse_links_in(top: &Path) -> Result<(), FilesError> {
    no_links(top)?;
    let mut folders = vec![top.to_path_buf()];
    while let Some(folder) = folders.pop() {
        let entries = fs::read_dir(&folder).map_err(|source| FilesError::io(&folder, source))?;
        for entry in entries {
            let entry = entry.map_err(|source| FilesError::io(&folder, source))?;
            let path = entry.path();
            let kind = entry
                .file_type()
                .map_err(|source| FilesError::io(&path, source))?;
            if kind.is_symlink() {
                return Err(FilesError::Link { path });
            }
            if kind.is_dir() {
                folders.push(path);
            }
        }
    }
    Ok(())
}

/// Syncs the staged tree under `staging` with `sync`, as [`sync_tree`] syncs it, and then the
/// folder that holds the staging, as [`Staging::sync_folders`] does. The first failure stops it.
#[cfg(unix)]
fn sync_staged(staging: &Path, sync: SyncFolder<'_>) -> Result<(), FilesError> {
    sync_tree(staging, sync)?;
    let holder = folder_of(staging);
    sync_folder(holder, sync).map_err(|source| FilesError::io(holder, source))
}

/// Syncs `top` and every folder under it with `sync`: they are listed top down, each after the
/// folder above it, then synced in the reverse order, deepest first. Only real folders are
/// entered; a link is never followed. The first failure stops it.
#[cfg(unix)]
fn sync_tree(top: &Path, sync: SyncFolder<'_>) -> Result<(), FilesError> {
    let mut folders = vec![top.to_path_buf()];
    let mut listed = 0;
    while let Some(folder) = folders.get(listed).cloned() {
        listed += 1;
        let entries = fs::read_dir(&folder).map_err(|source| FilesError::io(&folder, source))?;
        for entry in entries {
            let entry = entry.map_err(|source| FilesError::io(&folder, source))?;
            let path = entry.path();
            let kind = entry
                .file_type()
                .map_err(|source| FilesError::io(&path, source))?;
            if kind.is_dir() {
                folders.push(path);
            }
        }
    }
    folders.iter().rev().try_for_each(|folder| {
        sync_folder(folder, sync).map_err(|source| FilesError::io(folder, source))
    })
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::ffi::OsStr;
    use std::fs;
    use std::io::{self, Write};
    use std::path::{Path, PathBuf};

    use gdl_fixtures::{folder_names, link_folder, symlink_file_where_allowed};

    use super::{Journal, STAGING_RANDOM_CHARS, Staging};
    use crate::error::{assert_io, assert_link};
    use crate::{FilesError, PAYLOAD_NAMES, RelPath};

    /// What is at a path in a tree, to compare before and after.
    #[derive(Debug, PartialEq, Eq)]
    enum Entry {
        Folder,
        File(Vec<u8>),
        Link,
    }

    /// Everything under `top`, by its path from `top` with `/` between names, looked at without
    /// following any link.
    fn tree(top: &Path) -> BTreeMap<String, Entry> {
        let mut found = BTreeMap::new();
        let mut folders = vec![(top.to_path_buf(), String::new())];
        while let Some((folder, above)) = folders.pop() {
            for entry in fs::read_dir(&folder).unwrap() {
                let entry = entry.unwrap();
                let name = format!("{above}{}", entry.file_name().into_string().unwrap());
                let kind = entry.file_type().unwrap();
                let what = if kind.is_symlink() {
                    Entry::Link
                } else if kind.is_dir() {
                    folders.push((entry.path(), format!("{name}/")));
                    Entry::Folder
                } else {
                    Entry::File(fs::read(entry.path()).unwrap())
                };
                found.insert(name, what);
            }
        }
        found
    }

    fn rel(text: &str) -> RelPath {
        RelPath::parse(text, &PAYLOAD_NAMES).unwrap_or_else(|error| panic!("{error}"))
    }

    fn rels(texts: &[&str]) -> Vec<RelPath> {
        texts.iter().map(|text| rel(text)).collect()
    }

    /// Makes the file `path` in `staging` as a job does: new, written, synced and closed.
    fn stage(staging: &Staging, path: &str, bytes: &[u8]) {
        let mut file = staging.create_file(&rel(path)).unwrap();
        file.write_all(bytes).unwrap();
        file.sync_all().unwrap();
    }

    /// Removes what is at `path`: a folder with what it holds, and a link as the link alone,
    /// never what it leads to.
    fn clear(path: &Path) {
        let kind = fs::symlink_metadata(path).unwrap().file_type();
        #[cfg(windows)]
        let folder_link = std::os::windows::fs::FileTypeExt::is_symlink_dir(&kind);
        #[cfg(unix)]
        let folder_link = false;
        if kind.is_dir() {
            fs::remove_dir_all(path).unwrap();
        } else if folder_link {
            fs::remove_dir(path).unwrap();
        } else {
            fs::remove_file(path).unwrap();
        }
    }

    /// A journal that writes down each root it is told of, and checks that it is told only once
    /// the root has left the staging for its place.
    struct Recorder<'a> {
        /// The staging the roots leave.
        staging: &'a Path,
        /// The folder they go to.
        into: &'a Path,
        /// The roots told of, in order.
        roots: Vec<String>,
    }

    impl<'a> Recorder<'a> {
        fn new(staging: &'a Staging, into: &'a Path) -> Recorder<'a> {
            Recorder {
                staging: staging.path(),
                into,
                roots: Vec::new(),
            }
        }
    }

    impl Journal for Recorder<'_> {
        fn published(&mut self, root: &RelPath) -> Result<(), FilesError> {
            let placed = root.path_under(self.into);
            assert!(
                fs::symlink_metadata(&placed).is_ok(),
                "{}",
                placed.display()
            );
            let staged = root.path_under(self.staging);
            assert!(
                fs::symlink_metadata(&staged).is_err(),
                "{}",
                staged.display()
            );
            self.roots.push(root.as_str().to_owned());
            Ok(())
        }
    }

    /// A folder to install into, with its state folder made, beside a folder that nothing the
    /// tests make may reach.
    struct Install {
        /// The folder all of it lies in, removed with it.
        _dir: tempfile::TempDir,
        /// The folder installed into.
        root: PathBuf,
        /// Its `.gdl-launcher` folder, where stagings are made.
        state: PathBuf,
        /// A folder outside, which links lead to.
        outside: PathBuf,
    }

    fn install() -> Install {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("Gauntlet Dark Legacy");
        let state = root.join(".gdl-launcher");
        fs::create_dir_all(&state).unwrap();
        let outside = dir.path().join("outside");
        fs::create_dir(&outside).unwrap();
        Install {
            _dir: dir,
            root,
            state,
            outside,
        }
    }

    #[test]
    fn stagings_are_made_new_in_their_destination() {
        let install = install();
        let first = Staging::create_in(&install.state, "install-").unwrap();
        let second = Staging::create_in(&install.state, "install-").unwrap();
        let n64 = Staging::create_in(&install.state, "stage-n64-").unwrap();
        assert_eq!(STAGING_RANDOM_CHARS, 8);
        for (staging, prefix) in [
            (&first, "install-"),
            (&second, "install-"),
            (&n64, "stage-n64-"),
        ] {
            let name = staging.name();
            assert_eq!(staging.path().file_name(), Some(OsStr::new(name)));
            let random = name.strip_prefix(prefix).unwrap();
            assert_eq!(random.len(), STAGING_RANDOM_CHARS, "{name}");
            assert!(
                random.bytes().all(|byte| byte.is_ascii_alphanumeric()),
                "{name}"
            );
            assert!(staging.path().is_absolute());
            assert_eq!(staging.path().parent(), Some(install.state.as_path()));
            assert!(fs::symlink_metadata(staging.path()).unwrap().is_dir());
            assert!(folder_names(staging.path()).unwrap().is_empty());
        }
        assert_ne!(first.path(), second.path());
        assert_eq!(folder_names(&install.state).unwrap().len(), 3);

        // A destination with a link on its path is refused, and nothing is made through it.
        let link = install.root.join("linked");
        link_folder(&link, &install.outside).unwrap();
        assert_link(Staging::create_in(&link, "install-"), &link);
        assert_link(Staging::create_in(&link.join("deeper"), "install-"), &link);
        assert!(folder_names(&install.outside).unwrap().is_empty());

        // A destination that is not there is the system's error.
        let missing = install.root.join("missing");
        let source = assert_io(Staging::create_in(&missing, "install-"), &missing);
        assert_eq!(source.kind(), io::ErrorKind::NotFound);
        assert!(fs::symlink_metadata(&missing).is_err());

        // A prefix that is not one safe name would put the staging elsewhere, beside the
        // destination, under it or in place of it, and is refused before anything is made.
        let folders = [
            install.root.parent().unwrap(),
            install.root.as_path(),
            install.state.as_path(),
            install.outside.as_path(),
        ];
        let lists = || folders.map(|folder| folder_names(folder).unwrap());
        let before = lists();
        let absolute = install.outside.join("x-");
        for prefix in [
            "../escaped-",
            "..",
            ".",
            "",
            "a/b-",
            r"a\b-",
            "C:",
            "con",
            "install-\u{e9}",
            absolute.to_str().unwrap(),
        ] {
            let source = assert_io(Staging::create_in(&install.state, prefix), &install.state);
            assert_eq!(source.kind(), io::ErrorKind::InvalidInput, "{prefix:?}");
        }
        assert_eq!(lists(), before);
    }

    #[test]
    fn creates_files_exclusively() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/text/en.json", b"{}");
        let made: BTreeMap<String, Entry> = [
            ("VERSION".to_owned(), Entry::File(b"1.0.0-beta\n".to_vec())),
            ("data".to_owned(), Entry::Folder),
            ("data/text".to_owned(), Entry::Folder),
            ("data/text/en.json".to_owned(), Entry::File(b"{}".to_vec())),
        ]
        .into();
        assert_eq!(tree(staging.path()), made);

        // A name that is taken refuses the file, whatever takes it, the same way on every system,
        // and is left as it was.
        let assert_taken = |result: Result<fs::File, FilesError>, at: &Path| {
            let source = assert_io(result, at);
            assert_eq!(source.kind(), io::ErrorKind::AlreadyExists, "{source}");
            assert_eq!(source.raw_os_error(), None, "{source}");
        };
        for taken in ["VERSION", "data/text/en.json", "data/text", "data"] {
            let at = rel(taken).path_under(staging.path());
            assert_taken(staging.create_file(&rel(taken)), &at);
        }
        assert_eq!(tree(staging.path()), made);

        // A link at the name is never followed: nothing is made where it leads.
        let link = staging.path().join("shaders");
        link_folder(&link, &install.outside).unwrap();
        assert_taken(staging.create_file(&rel("shaders")), &link);
        assert!(folder_names(&install.outside).unwrap().is_empty());
        let kept = install.outside.join("kept.json");
        fs::write(&kept, b"kept").unwrap();
        let nowhere = install.outside.join("nowhere.json");
        for (name, target) in [("config.json", &kept), ("dangling.json", &nowhere)] {
            let link = staging.path().join(name);
            if !symlink_file_where_allowed(&link, target) {
                eprintln!("skipped the file links: this account may not make them");
                break;
            }
            assert_taken(staging.create_file(&rel(name)), &link);
        }
        assert_eq!(fs::read(&kept).unwrap(), b"kept");
        assert!(fs::symlink_metadata(&nowhere).is_err());

        // A file made is the caller's to write.
        let mut file = staging.create_file(&rel("portable.flag")).unwrap();
        file.write_all(b"portable").unwrap();
        drop(file);
        let flag = staging.path().join("portable.flag");
        assert_eq!(fs::read(flag).unwrap(), b"portable");
    }

    /// A staging taken back by its name is the folder that was made, and goes on as that one would
    /// have: it makes files, publishes and is removed.
    #[test]
    fn a_reopened_staging_publishes_and_removes() {
        let install = install();
        let mut staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/config.json", b"{}");
        staging.create_dir(&rel("saves")).unwrap();
        let folder = staging.path().to_path_buf();
        let name = staging.name().to_owned();
        // A job keeps the staging its journal names, and may then stop at any moment.
        staging.keep();
        drop(staging);

        let staging = Staging::reopen(&install.state, "install-", &name).unwrap();
        assert_eq!(staging.path(), folder);
        assert_eq!(staging.name(), name);
        stage(&staging, "shaders/test.spv", b"shader");
        staging.sync_folders().unwrap();
        let staged = tree(staging.path());
        let mut journal = Recorder::new(&staging, &install.root);
        let roots = rels(&["VERSION", "data", "saves", "shaders"]);
        staging
            .publish_new(&roots, &install.root, &mut journal)
            .unwrap();
        assert_eq!(journal.roots, ["VERSION", "data", "saves", "shaders"]);
        assert!(folder_names(staging.path()).unwrap().is_empty());

        // Removed, it goes, and what it published stays. Once gone, its name finds nothing.
        staging.remove().unwrap();
        assert!(fs::symlink_metadata(&folder).is_err());
        assert!(folder_names(&install.state).unwrap().is_empty());
        let mut published = tree(&install.root);
        assert_eq!(published.remove(".gdl-launcher"), Some(Entry::Folder));
        assert_eq!(published, staged);
        let source = assert_io(Staging::reopen(&install.state, "install-", &name), &folder);
        assert_eq!(source.kind(), io::ErrorKind::NotFound);

        // A staging that still holds a tree goes whole, a link in it as the link alone.
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "Gauntlet/PDATA/WAR.WAD", b"war");
        staging.create_dir(&rel("data/text")).unwrap();
        let kept = install.outside.join("kept.json");
        fs::write(&kept, b"kept").unwrap();
        link_folder(&staging.path().join("linked"), &install.outside).unwrap();
        let folder = staging.path().to_path_buf();
        staging.remove().unwrap();
        assert!(fs::symlink_metadata(&folder).is_err());
        assert!(folder_names(&install.state).unwrap().is_empty());
        assert_eq!(folder_names(&install.outside).unwrap(), ["kept.json"]);
        assert_eq!(fs::read(&kept).unwrap(), b"kept");
    }

    #[test]
    fn reopen_refuses_a_name_not_its_own() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        // A folder stands at each name refused for what it is called, so that the name alone
        // refuses it: another prefix, the prefix in another letter case, a random part of seven
        // or nine, and eight bytes that are not all ASCII letters and digits.
        let misnamed = [
            "stage-AbC123xy",
            "INSTALL-AbC123xy",
            "install-AbC123x",
            "install-AbC123xyz",
            "install-AbC123x-",
            "install-AbC12\u{e9}y",
        ];
        for name in misnamed {
            fs::create_dir(install.state.join(name)).unwrap();
        }
        let folders = [
            install.root.as_path(),
            install.state.as_path(),
            install.outside.as_path(),
        ];
        let lists = || folders.map(|folder| folder_names(folder).unwrap());
        let before = lists();
        for name in misnamed {
            let at = install.state.join(name);
            let source = assert_io(Staging::reopen(&install.state, "install-", name), &at);
            assert_eq!(source.kind(), io::ErrorKind::InvalidInput, "{name:?}");
        }
        // A prefix that is not one safe name is refused, as a staging is never made with it, even
        // for a name that it begins.
        for (prefix, name) in [
            ("../escaped-", "../escaped-AbC123xy"),
            ("", "AbC123xy"),
            ("a/b-", "a/b-AbC123xy"),
            ("con", "conAbC123xy"),
        ] {
            let at = install.state.join(name);
            let source = assert_io(Staging::reopen(&install.state, prefix, name), &at);
            assert_eq!(source.kind(), io::ErrorKind::InvalidInput, "{prefix:?}");
        }
        assert_eq!(lists(), before);

        // What stands at a staging's name must be a real folder: a link is refused, and so is a
        // file, and both are left as they are.
        let link = install.state.join("install-LinkLink");
        link_folder(&link, &install.outside).unwrap();
        let reopened = Staging::reopen(&install.state, "install-", "install-LinkLink");
        assert_link(reopened, &link);
        let file = install.state.join("install-FileFile");
        fs::write(&file, b"file").unwrap();
        let reopened = Staging::reopen(&install.state, "install-", "install-FileFile");
        let source = assert_io(reopened, &file);
        assert_eq!(source.kind(), io::ErrorKind::NotADirectory);
        assert_eq!(fs::read(&file).unwrap(), b"file");
        assert!(
            fs::symlink_metadata(&link)
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert!(folder_names(&install.outside).unwrap().is_empty());

        // Nothing there is the system's answer, as for a staging already removed.
        let missing = install.state.join("install-Missing1");
        let source = assert_io(
            Staging::reopen(&install.state, "install-", "install-Missing1"),
            &missing,
        );
        assert_eq!(source.kind(), io::ErrorKind::NotFound);
        assert!(fs::symlink_metadata(&missing).is_err());

        // A link above the staging refuses it too.
        let linked = install.root.join("linked state");
        link_folder(&linked, &install.state).unwrap();
        assert_link(
            Staging::reopen(&linked, "install-", staging.name()),
            &linked,
        );

        // The staging's own name takes it back.
        let reopened = Staging::reopen(&install.state, "install-", staging.name()).unwrap();
        assert_eq!(reopened.path(), staging.path());
    }

    /// A removal that the system refuses is reported, for the staging, and what it could not
    /// remove stays.
    #[cfg(windows)]
    #[test]
    fn remove_reports_what_it_could_not_remove() {
        use std::thread;
        use std::time::{Duration, Instant};

        use gdl_fixtures::hold_closed;

        use crate::{RETRIED_WINDOWS_ERRORS, RETRY_WINDOW};

        /// How long a stand-in for a scanner holds a file when its hold passes: a few retries.
        const HOLD: Duration = Duration::from_millis(300);

        let install = install();
        // A hold that passes is waited out, and the staging goes.
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/config.json", b"{}");
        let held = staging.path().join("data").join("config.json");
        let hold = hold_closed(&[&held]).unwrap();
        assert!(hold.proven());
        let started = Instant::now();
        let closer = thread::spawn(move || {
            thread::sleep(HOLD);
            drop(hold);
        });
        staging.remove().unwrap();
        assert!(started.elapsed() >= HOLD);
        closer.join().unwrap();
        assert!(folder_names(&install.state).unwrap().is_empty());

        // A hold that lasts, on a file shared with no one, is reported once the retries are over.
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/config.json", b"{}");
        let folder = staging.path().to_path_buf();
        let name = staging.name().to_owned();
        let held = folder.join("data").join("config.json");
        let hold = hold_closed(&[&held]).unwrap();
        let started = Instant::now();
        let source = assert_io(staging.remove(), &folder);
        assert!(started.elapsed() >= RETRY_WINDOW, "{source}");
        let code = source.raw_os_error();
        let retried = code.is_some_and(|code| RETRIED_WINDOWS_ERRORS.contains(&code));
        assert!(retried, "{source}");
        drop(hold);
        assert_eq!(fs::read(&held).unwrap(), b"{}");
        assert_eq!(folder_names(&install.state).unwrap(), [name]);
    }

    /// A removal that the system refuses is reported, for the staging, and what it could not
    /// remove stays.
    #[cfg(unix)]
    #[test]
    fn remove_reports_what_it_could_not_remove() {
        use std::os::unix::fs::PermissionsExt;

        /// Reading and searching but no writing: no name in such a folder can be removed.
        const READ_AND_SEARCH: u32 = 0o555;

        /// Gives a folder back its owner's whole access when dropped, before the staging or the
        /// `TempDir` above it goes, which could not empty it otherwise.
        struct AccessRestored<'a>(&'a Path);

        impl Drop for AccessRestored<'_> {
            fn drop(&mut self) {
                let _ = fs::set_permissions(self.0, fs::Permissions::from_mode(0o700));
            }
        }

        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/config.json", b"{}");
        let folder = staging.path().to_path_buf();
        let name = staging.name().to_owned();
        let locked = folder.join("data");
        let _restored = AccessRestored(&locked);
        fs::set_permissions(&locked, fs::Permissions::from_mode(READ_AND_SEARCH)).unwrap();
        if fs::File::create_new(locked.join("probe")).is_ok() {
            eprintln!("skipped: this user writes in folders of mode 0o555, as root does");
            return;
        }
        let source = assert_io(staging.remove(), &folder);
        assert_eq!(source.kind(), io::ErrorKind::PermissionDenied, "{source}");
        assert_eq!(fs::read(locked.join("config.json")).unwrap(), b"{}");
        assert_eq!(folder_names(&install.state).unwrap(), [name]);
    }

    #[test]
    fn a_kept_staging_survives_its_drop() {
        let install = install();
        let mut staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/config.json", b"{}");
        let folder = staging.path().to_path_buf();
        let name = staging.name().to_owned();
        let staged = tree(&folder);
        staging.keep();
        drop(staging);
        assert_eq!(tree(&folder), staged);

        // One taken back by its name comes back kept, since a journal names it.
        let reopened = Staging::reopen(&install.state, "install-", &name).unwrap();
        drop(reopened);
        assert_eq!(tree(&folder), staged);
        assert_eq!(folder_names(&install.state).unwrap(), [name.as_str()]);

        // Only its removal takes it away.
        let reopened = Staging::reopen(&install.state, "install-", &name).unwrap();
        reopened.remove().unwrap();
        assert!(folder_names(&install.state).unwrap().is_empty());
    }

    #[test]
    fn unpublish_takes_a_root_back_and_never_replaces() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/config.json", b"{}");
        stage(&staging, "data/text/en.json", b"{\"a\": 1}");
        let staged = tree(staging.path());
        let roots = rels(&["VERSION", "data"]);
        let mut journal = Recorder::new(&staging, &install.root);
        staging
            .publish_new(&roots, &install.root, &mut journal)
            .unwrap();

        // Taken back last first, each root comes back whole and leaves its place.
        for root in ["data", "VERSION"] {
            staging.unpublish(&rel(root), &install.root).unwrap();
        }
        assert_eq!(tree(staging.path()), staged);
        assert_eq!(folder_names(&install.root).unwrap(), [".gdl-launcher"]);

        // Published again, a root whose name the staging holds once more, as a file or as a
        // folder, is never put back over it, and both stay as they are.
        let mut journal = Recorder::new(&staging, &install.root);
        staging
            .publish_new(&roots, &install.root, &mut journal)
            .unwrap();
        stage(&staging, "VERSION", b"another");
        staging.create_dir(&rel("data")).unwrap();
        let before = (tree(staging.path()), tree(&install.root));
        for root in ["VERSION", "data"] {
            match staging.unpublish(&rel(root), &install.root) {
                Err(FilesError::Exists { path }) => assert_eq!(path, staging.path().join(root)),
                other => panic!("{root}: {other:?}"),
            }
            assert_eq!(
                (tree(staging.path()), tree(&install.root)),
                before,
                "{root}"
            );
        }

        // A name deeper than a root is no root, and a root that is not there is the system's
        // answer, for the staged path.
        match staging.unpublish(&rel("data/config.json"), &install.root) {
            Err(FilesError::NotRoot { name }) => assert_eq!(&*name, "data/config.json"),
            other => panic!("{other:?}"),
        }
        let at = staging.path().join("shaders");
        let source = assert_io(staging.unpublish(&rel("shaders"), &install.root), &at);
        assert_eq!(source.kind(), io::ErrorKind::NotFound);
        assert_eq!((tree(staging.path()), tree(&install.root)), before);

        // A link on the way to the folder taken from, or to the staging, refuses the move before
        // anything moves. The state folder holds files just written and read, which a scanner
        // may still hold, so its moves wait as the launcher's own do.
        fs::remove_file(staging.path().join("VERSION")).unwrap();
        let linked_root = install.root.with_file_name("linked root");
        link_folder(&linked_root, &install.root).unwrap();
        let unpublished = staging.unpublish(&rel("VERSION"), &linked_root);
        assert_link(unpublished, &linked_root);
        let real_state = install.state.with_file_name(".gdl-launcher-real");
        crate::retry_locked(|| fs::rename(&install.state, &real_state)).unwrap();
        link_folder(&install.state, &real_state).unwrap();
        let unpublished = staging.unpublish(&rel("VERSION"), &install.root);
        assert_link(unpublished, &install.state);
        clear(&install.state);
        crate::retry_locked(|| fs::rename(&real_state, &install.state)).unwrap();
        let version = install.root.join("VERSION");
        assert_eq!(fs::read(&version).unwrap(), b"1.0.0-beta\n");

        // With its name free and no link on the way, the root comes back.
        staging.unpublish(&rel("VERSION"), &install.root).unwrap();
        assert!(fs::symlink_metadata(&version).is_err());
        let staged_version = staging.path().join("VERSION");
        assert_eq!(fs::read(staged_version).unwrap(), b"1.0.0-beta\n");
    }

    #[cfg(unix)]
    #[test]
    fn sync_folders_reaches_every_folder() {
        use std::cell::RefCell;
        use std::os::unix::fs::MetadataExt;

        use super::sync_staged;

        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/text/en.json", b"{}");
        stage(&staging, "Gauntlet/PDATA/WAR.WAD", b"war");
        staging.create_dir(&rel("saves")).unwrap();
        // A link to a folder that holds a folder: neither is the staging's, and neither is synced.
        fs::create_dir(install.outside.join("beyond")).unwrap();
        link_folder(&staging.path().join("linked"), &install.outside).unwrap();
        let inode = |path: &Path| fs::metadata(path).unwrap().ino();
        let mut depths = BTreeMap::from([(inode(staging.path()), 0)]);
        for name in ["Gauntlet", "Gauntlet/PDATA", "data", "data/text", "saves"] {
            let folder = rel(name).path_under(staging.path());
            depths.insert(inode(&folder), rel(name).depth());
        }
        // The folder that holds the staging, synced last, so that the staging's own name lasts.
        let holder = inode(&install.state);

        // Every folder of the tree once, each after every folder below it, then the holder.
        let synced = RefCell::new(Vec::new());
        let record = |folder: &fs::File| {
            synced.borrow_mut().push(folder.metadata()?.ino());
            folder.sync_all()
        };
        sync_staged(staging.path(), &record).unwrap();
        let mut synced = synced.into_inner();
        assert_eq!(synced.pop(), Some(holder), "{synced:?}");
        let mut once = synced.clone();
        once.sort_unstable();
        assert!(once.iter().eq(depths.keys()), "{synced:?}");
        let order: Vec<usize> = synced.iter().map(|folder| depths[folder]).collect();
        assert!(
            order.is_sorted_by(|deeper, shallower| deeper >= shallower),
            "{order:?}"
        );
        // The system's own sync does the same.
        staging.sync_folders().unwrap();

        // A file system that cannot sync folders answers EINVAL, or EBADF on some systems, and
        // its folders count as synced. Any other answer is the answer, for the deepest folder,
        // and stops the syncs.
        for (code, refused) in [
            (libc::EINVAL, false),
            (libc::EBADF, false),
            (libc::EIO, true),
        ] {
            let tried = RefCell::new(Vec::new());
            let answer = |folder: &fs::File| {
                tried.borrow_mut().push(folder.metadata()?.ino());
                Err(io::Error::from_raw_os_error(code))
            };
            let result = sync_staged(staging.path(), &answer);
            let tried = tried.into_inner();
            if !refused {
                result.unwrap();
                assert_eq!(tried.len(), depths.len() + 1, "{code}");
                continue;
            }
            match result {
                Err(FilesError::Io { path, source }) => {
                    assert_eq!(source.raw_os_error(), Some(code));
                    assert_eq!(tried, [inode(&path)]);
                    assert_eq!(depths[&tried[0]], 2, "{}", path.display());
                }
                other => panic!("{code}: {other:?}"),
            }
        }

        // The system's own sync walks the staged tree too: a folder in it that cannot be opened
        // stops it there.
        let text = rel("data/text").path_under(staging.path());
        let denied = gdl_fixtures::DeniedAccess::deny(&[&text]).unwrap();
        if fs::read_dir(&text).is_ok() {
            eprintln!(
                "skipped the denied folder: this user opens folders of mode 0o000, as root does"
            );
        } else {
            match staging.sync_folders() {
                Err(FilesError::Io { path, source }) => {
                    assert_eq!(path, text);
                    assert_eq!(source.kind(), io::ErrorKind::PermissionDenied, "{source}");
                }
                other => panic!("{other:?}"),
            }
        }
        drop(denied);
        staging.sync_folders().unwrap();
    }

    /// A folder that the system will not let the launcher write in refuses a file for that
    /// reason, which is not a name that is taken.
    #[cfg(unix)]
    #[test]
    fn a_denial_is_no_taken_name() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        staging.create_dir(&rel("locked")).unwrap();
        let locked = staging.path().join("locked");
        let denied = gdl_fixtures::DeniedAccess::deny(&[&locked]).unwrap();
        if fs::read_dir(&locked).is_ok() {
            eprintln!("skipped: this user writes in folders of mode 0o000, as root does");
            return;
        }
        let at = locked.join("config.json");
        let source = assert_io(staging.create_file(&rel("locked/config.json")), &at);
        assert_eq!(source.kind(), io::ErrorKind::PermissionDenied, "{source}");
        assert_eq!(source.raw_os_error(), Some(libc::EACCES), "{source}");
        drop(denied);
        assert!(folder_names(&locked).unwrap().is_empty());
    }

    #[test]
    fn creates_folders_one_component_at_a_time() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        staging.create_dir(&rel("saves")).unwrap();
        staging.create_dir(&rel("data/text/fr")).unwrap();
        let made: BTreeMap<String, Entry> = ["data", "data/text", "data/text/fr", "saves"]
            .into_iter()
            .map(|name| (name.to_owned(), Entry::Folder))
            .collect();
        assert_eq!(tree(staging.path()), made);
        // A folder that is there already is fine, at any depth.
        for again in ["saves", "data", "data/text", "data/text/fr"] {
            staging.create_dir(&rel(again)).unwrap();
        }
        assert_eq!(tree(staging.path()), made);

        // A link planted on the way is refused where it stands, and nothing is made through it,
        // however deep the name below it runs.
        let link = staging.path().join("shaders");
        link_folder(&link, &install.outside).unwrap();
        for name in ["shaders", "shaders/spirv", "shaders/spirv/deeper"] {
            assert_link(staging.create_dir(&rel(name)), &link);
        }
        for name in ["shaders/test.spv", "shaders/spirv/test.spv"] {
            assert_link(staging.create_file(&rel(name)), &link);
        }
        let deeper = staging.path().join("data").join("linked");
        link_folder(&deeper, &install.outside).unwrap();
        assert_link(staging.create_file(&rel("data/linked/x/y.json")), &deeper);
        assert_link(staging.create_dir(&rel("data/linked/x")), &deeper);
        assert!(folder_names(&install.outside).unwrap().is_empty());

        // A file on the way is no folder.
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        let version = staging.path().join("VERSION");
        for result in [
            staging.create_dir(&rel("VERSION/x")),
            staging.create_file(&rel("VERSION/x.json")).map(drop),
        ] {
            let source = assert_io(result, &version);
            assert_eq!(source.kind(), io::ErrorKind::NotADirectory);
        }
        assert_eq!(fs::read(&version).unwrap(), b"1.0.0-beta\n");
    }

    #[test]
    fn publishes_roots_in_order_and_records_each() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/config.json", b"{}");
        stage(&staging, "data/text/en.json", b"{\"a\": 1}");
        stage(&staging, "shaders/test.spv", b"shader");
        stage(&staging, "Gauntlet/PDATA/WAR.WAD", b"war");
        staging.create_dir(&rel("saves")).unwrap();
        let staged = tree(staging.path());

        let mut journal = Recorder::new(&staging, &install.root);
        let roots = rels(&["shaders", "VERSION", "saves", "Gauntlet", "data"]);
        staging
            .publish_new(&roots, &install.root, &mut journal)
            .unwrap();
        // In byte order, which puts capitals first.
        assert_eq!(
            journal.roots,
            ["Gauntlet", "VERSION", "data", "saves", "shaders"]
        );
        let mut published = tree(&install.root);
        assert_eq!(published.remove(".gdl-launcher"), Some(Entry::Folder));
        let staging_name = staging.path().file_name().unwrap().to_str().unwrap();
        let staging_entry = format!(".gdl-launcher/{staging_name}");
        assert_eq!(published.remove(&staging_entry), Some(Entry::Folder));
        assert_eq!(published, staged);
        assert!(folder_names(staging.path()).unwrap().is_empty());

        // Publishing nothing moves nothing.
        staging
            .publish_new(&[], &install.root, &mut journal)
            .unwrap();
        assert_eq!(journal.roots.len(), roots.len());

        // Dropped, the staging folder goes and what it published stays.
        let folder = staging.path().to_path_buf();
        drop(staging);
        assert!(fs::symlink_metadata(&folder).is_err());
        assert!(folder_names(&install.state).unwrap().is_empty());
        let mut published = tree(&install.root);
        assert_eq!(published.remove(".gdl-launcher"), Some(Entry::Folder));
        assert_eq!(published, staged);
    }

    /// Checks that a refused publish moved nothing: the staging holds what it held, links aside,
    /// nothing reached the install folder or the folder outside, and the journal heard nothing.
    fn assert_unmoved(
        install: &Install,
        staging: &Staging,
        before: &BTreeMap<String, Entry>,
        journal: &Recorder<'_>,
    ) {
        assert!(journal.roots.is_empty());
        assert_eq!(folder_names(&install.root).unwrap(), [".gdl-launcher"]);
        assert!(folder_names(&install.outside).unwrap().is_empty());
        let mut now = tree(staging.path());
        now.retain(|_, entry| *entry != Entry::Link);
        assert_eq!(&now, before);
    }

    #[test]
    fn refuses_a_link_planted_in_staging() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/config.json", b"{}");
        stage(&staging, "shaders/spirv/test.spv", b"shader");
        let before = tree(staging.path());
        let roots = rels(&["data", "shaders"]);

        // Inside a root, deeper inside one, beside the roots, and at a root itself; to a folder,
        // to a file, and to nothing, which is a link all the same.
        let config = staging.path().join("data").join("config.json");
        let gone = install.outside.join("gone");
        let planted: [(&str, &[&str], Option<&Path>); 6] = [
            ("data/text", &["data", "shaders"], None),
            ("shaders/spirv/linked", &["data", "shaders"], None),
            ("lib", &["data", "shaders"], None),
            ("lib", &["lib"], None),
            ("data/linked.json", &["data", "shaders"], Some(&config)),
            ("dangling.json", &["data", "shaders"], Some(&gone)),
        ];
        for (name, published, file) in planted {
            let link = rel(name).path_under(staging.path());
            match file {
                None => link_folder(&link, &install.outside).unwrap(),
                Some(target) => {
                    if !symlink_file_where_allowed(&link, target) {
                        eprintln!("skipped the file links: this account may not make them");
                        continue;
                    }
                }
            }
            let mut journal = Recorder::new(&staging, &install.root);
            let publish = staging.publish_new(&rels(published), &install.root, &mut journal);
            assert_link(publish, &link);
            assert_unmoved(&install, &staging, &before, &journal);
            clear(&link);
        }

        // A link on the way to the staging itself, put where its folder was. The folder holds
        // files just written and read, which a scanner may still hold, so its moves wait as the
        // launcher's own do.
        let real_state = install.state.with_file_name(".gdl-launcher-real");
        crate::retry_locked(|| fs::rename(&install.state, &real_state)).unwrap();
        link_folder(&install.state, &real_state).unwrap();
        let mut journal = Recorder::new(&staging, &install.root);
        let publish = staging.publish_new(&roots, &install.root, &mut journal);
        assert_link(publish, &install.state);
        clear(&install.state);
        crate::retry_locked(|| fs::rename(&real_state, &install.state)).unwrap();
        assert_unmoved(&install, &staging, &before, &journal);

        // A link on the way to the folder published into.
        let linked_root = install.root.with_file_name("linked root");
        link_folder(&linked_root, &install.root).unwrap();
        let mut journal = Recorder::new(&staging, &linked_root);
        let publish = staging.publish_new(&roots, &linked_root, &mut journal);
        assert_link(publish, &linked_root);
        assert_unmoved(&install, &staging, &before, &journal);

        // With the links in the staging gone, the same roots publish.
        let mut journal = Recorder::new(&staging, &install.root);
        staging
            .publish_new(&roots, &install.root, &mut journal)
            .unwrap();
        assert_eq!(journal.roots, ["data", "shaders"]);
    }

    #[test]
    fn refuses_a_root_that_is_not_a_single_staged_name() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/config.json", b"{}");
        let before = tree(staging.path());
        for (roots, refused) in [
            // Deeper than a root, though the folder above it is one.
            (&["data/config.json"][..], "data/config.json"),
            (&["VERSION", "data/config.json"][..], "data/config.json"),
            // Not staged, or staged in another letter case, which every system tells apart here.
            (&["shaders"][..], "shaders"),
            (&["VERSION", "data", "missing"][..], "missing"),
            (&["Data"][..], "Data"),
            (&["version"][..], "version"),
        ] {
            let mut journal = Recorder::new(&staging, &install.root);
            match staging.publish_new(&rels(roots), &install.root, &mut journal) {
                Err(FilesError::NotRoot { name }) => assert_eq!(&*name, refused, "{roots:?}"),
                other => panic!("{roots:?}: {other:?}"),
            }
            assert_unmoved(&install, &staging, &before, &journal);
        }
    }

    #[test]
    fn a_root_named_twice_is_refused() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "data/config.json", b"{}");
        let before = tree(staging.path());
        for (roots, refused) in [
            // The same name again, at once or after another root.
            (&["data", "data"][..], "data"),
            (&["data", "VERSION", "data"][..], "data"),
            (&["VERSION", "data", "VERSION"][..], "VERSION"),
            // Again in another letter case, which the staging does not hold as it was made with
            // one spelling alone, on every system.
            (&["data", "Data"][..], "Data"),
            (&["version", "VERSION"][..], "version"),
        ] {
            let mut journal = Recorder::new(&staging, &install.root);
            match staging.publish_new(&rels(roots), &install.root, &mut journal) {
                Err(FilesError::NotRoot { name }) => assert_eq!(&*name, refused, "{roots:?}"),
                other => panic!("{roots:?}: {other:?}"),
            }
            assert_unmoved(&install, &staging, &before, &journal);
        }

        // Named once each, the same roots publish.
        let mut journal = Recorder::new(&staging, &install.root);
        staging
            .publish_new(&rels(&["data", "VERSION"]), &install.root, &mut journal)
            .unwrap();
        assert_eq!(journal.roots, ["VERSION", "data"]);
        assert!(folder_names(staging.path()).unwrap().is_empty());
    }

    /// Two roots that differ only in letter case, which a file system that keeps such names apart
    /// stages side by side, are two roots and both publish, as a Linux runtime's program
    /// `gauntlet` does beside the disc's folder `Gauntlet`.
    #[cfg(target_os = "linux")]
    #[test]
    fn roots_differing_only_in_case_publish_where_the_system_keeps_both() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "gauntlet", b"program");
        stage(&staging, "Gauntlet/PDATA/WAR.WAD", b"war");
        assert_eq!(
            folder_names(staging.path()).unwrap(),
            ["Gauntlet", "gauntlet"]
        );
        let staged = tree(staging.path());

        let mut journal = Recorder::new(&staging, &install.root);
        staging
            .publish_new(
                &rels(&["gauntlet", "Gauntlet"]),
                &install.root,
                &mut journal,
            )
            .unwrap();
        // In byte order, which puts capitals first.
        assert_eq!(journal.roots, ["Gauntlet", "gauntlet"]);
        let mut published = tree(&install.root);
        assert_eq!(published.remove(".gdl-launcher"), Some(Entry::Folder));
        let staging_name = staging.path().file_name().unwrap().to_str().unwrap();
        let staging_entry = format!(".gdl-launcher/{staging_name}");
        assert_eq!(published.remove(&staging_entry), Some(Entry::Folder));
        assert_eq!(published, staged);
        assert!(folder_names(staging.path()).unwrap().is_empty());
    }

    #[test]
    fn a_failed_publish_keeps_the_unpublished_roots_until_dropped() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        for root in ["a-first", "b-second", "c-third"] {
            stage(&staging, &format!("{root}/{root}.json"), root.as_bytes());
        }
        // Something has come to the second root's name since the job planned.
        let taken = install.root.join("b-second");
        fs::write(&taken, b"kept").unwrap();
        let mut journal = Recorder::new(&staging, &install.root);
        let roots = rels(&["c-third", "b-second", "a-first"]);
        match staging.publish_new(&roots, &install.root, &mut journal) {
            Err(FilesError::Exists { path }) => assert_eq!(path, taken),
            other => panic!("{other:?}"),
        }
        // The first root is in place and recorded; the others wait in the staging.
        assert_eq!(journal.roots, ["a-first"]);
        assert_eq!(
            folder_names(&install.root).unwrap(),
            [".gdl-launcher", "a-first", "b-second"]
        );
        assert_eq!(fs::read(&taken).unwrap(), b"kept");
        assert_eq!(
            folder_names(staging.path()).unwrap(),
            ["b-second", "c-third"]
        );

        // Dropped, the staging takes them with it, and nothing published goes.
        drop(staging);
        assert!(folder_names(&install.state).unwrap().is_empty());
        assert_eq!(
            folder_names(&install.root).unwrap(),
            [".gdl-launcher", "a-first", "b-second"]
        );
        let placed = install.root.join("a-first").join("a-first.json");
        assert_eq!(fs::read(placed).unwrap(), b"a-first");
        assert_eq!(fs::read(&taken).unwrap(), b"kept");
    }

    /// A journal that fails to record the root it is told of after `recorded` others.
    struct FailingJournal {
        /// How many roots it records before it fails.
        recorded: usize,
        /// The roots it was told of, in order, the one it failed on included.
        told: Vec<String>,
    }

    impl Journal for FailingJournal {
        fn published(&mut self, root: &RelPath) -> Result<(), FilesError> {
            self.told.push(root.as_str().to_owned());
            if self.told.len() > self.recorded {
                let full = io::Error::from(io::ErrorKind::StorageFull);
                return Err(FilesError::io(Path::new("journal/install.json"), full));
            }
            Ok(())
        }
    }

    #[test]
    fn a_journal_that_fails_stops_the_publish() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        for root in ["a-first", "b-second", "c-third"] {
            stage(&staging, &format!("{root}/{root}.json"), root.as_bytes());
        }
        let mut journal = FailingJournal {
            recorded: 1,
            told: Vec::new(),
        };
        let roots = rels(&["a-first", "b-second", "c-third"]);
        let publish = staging.publish_new(&roots, &install.root, &mut journal);
        let source = assert_io(publish, Path::new("journal/install.json"));
        assert_eq!(source.kind(), io::ErrorKind::StorageFull);
        // The root it failed to record has moved; the one after it has not.
        assert_eq!(journal.told, ["a-first", "b-second"]);
        assert_eq!(
            folder_names(&install.root).unwrap(),
            [".gdl-launcher", "a-first", "b-second"]
        );
        assert_eq!(folder_names(staging.path()).unwrap(), ["c-third"]);
    }

    /// Puts something at a root's target, the first path, given a folder outside, the second.
    type Occupy = fn(&Path, &Path);

    #[test]
    fn publishing_refuses_an_existing_target() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/config.json", b"{}");
        let before = tree(staging.path());
        let target = install.root.join("data");
        let occupiers: [(&str, Occupy); 5] = [
            ("a file", |target, _| fs::write(target, b"kept").unwrap()),
            ("an empty folder", |target, _| {
                fs::create_dir(target).unwrap()
            }),
            ("a folder holding a file", |target, _| {
                fs::create_dir(target).unwrap();
                fs::write(target.join("kept.json"), b"kept").unwrap();
            }),
            ("a link to a folder", |target, outside| {
                link_folder(target, outside).unwrap()
            }),
            ("a link to nothing", |target, outside| {
                let gone = outside.join("gone");
                fs::create_dir(&gone).unwrap();
                link_folder(target, &gone).unwrap();
                fs::remove_dir(&gone).unwrap();
            }),
        ];
        for (kind, occupy) in occupiers {
            occupy(&target, &install.outside);
            let occupied = tree(&install.root);
            let mut journal = Recorder::new(&staging, &install.root);
            match staging.publish_new(&rels(&["data"]), &install.root, &mut journal) {
                Err(FilesError::Exists { path }) => assert_eq!(path, target, "{kind}"),
                other => panic!("{kind}: {other:?}"),
            }
            assert!(journal.roots.is_empty(), "{kind}");
            assert_eq!(tree(staging.path()), before, "{kind}");
            assert_eq!(tree(&install.root), occupied, "{kind}");
            assert!(folder_names(&install.outside).unwrap().is_empty(), "{kind}");
            clear(&target);
        }
        // Once the name is free again, the root goes there.
        let mut journal = Recorder::new(&staging, &install.root);
        staging
            .publish_new(&rels(&["data"]), &install.root, &mut journal)
            .unwrap();
        assert_eq!(journal.roots, ["data"]);
    }

    #[test]
    fn a_move_the_system_refuses_is_reported_for_its_target() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/config.json", b"{}");
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        let before = tree(staging.path());
        // The folder to publish into is not there, so the first move fails, and stops the rest.
        let missing = install.root.join("missing");
        let mut journal = Recorder::new(&staging, &missing);
        let publish = staging.publish_new(&rels(&["data", "VERSION"]), &missing, &mut journal);
        let source = assert_io(publish, &missing.join("VERSION"));
        assert_eq!(source.kind(), io::ErrorKind::NotFound);
        assert!(journal.roots.is_empty());
        assert_eq!(tree(staging.path()), before);
        assert!(fs::symlink_metadata(&missing).is_err());
    }

    #[test]
    fn a_dropped_staging_leaves_nothing() {
        let install = install();
        fs::write(install.state.join("receipt.json"), b"{}\n").unwrap();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "VERSION", b"1.0.0-beta\n");
        stage(&staging, "Gauntlet/PDATA/WAR.WAD", b"war");
        staging.create_dir(&rel("saves")).unwrap();
        staging.create_dir(&rel("data/text")).unwrap();
        // A link left in it goes as a link: what it leads to is kept.
        let kept = install.outside.join("kept.json");
        fs::write(&kept, b"kept").unwrap();
        link_folder(&staging.path().join("linked"), &install.outside).unwrap();
        let folder = staging.path().to_path_buf();
        drop(staging);
        assert!(fs::symlink_metadata(&folder).is_err());
        assert_eq!(folder_names(&install.state).unwrap(), ["receipt.json"]);
        assert_eq!(folder_names(&install.outside).unwrap(), ["kept.json"]);
        assert_eq!(fs::read(&kept).unwrap(), b"kept");
    }

    /// A staged folder that the system will not let the launcher list cannot be shown to be free
    /// of links, so nothing is published.
    #[cfg(unix)]
    #[test]
    fn a_folder_that_cannot_be_listed_is_not_published() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/text/en.json", b"{}");
        let locked = staging.path().join("data").join("text");
        let denied = gdl_fixtures::DeniedAccess::deny(&[&locked]).unwrap();
        if fs::read_dir(&locked).is_ok() {
            eprintln!("skipped: this user lists folders of mode 0o000, as root does");
            return;
        }
        let mut journal = Recorder::new(&staging, &install.root);
        let publish = staging.publish_new(&rels(&["data"]), &install.root, &mut journal);
        let source = assert_io(publish, &locked);
        assert_eq!(source.kind(), io::ErrorKind::PermissionDenied);
        assert!(journal.roots.is_empty());
        assert_eq!(folder_names(&install.root).unwrap(), [".gdl-launcher"]);
        // The mode comes back before the staging goes, so that it can be removed whole.
        drop(denied);
        let folder = staging.path().to_path_buf();
        drop(staging);
        assert!(fs::symlink_metadata(folder).is_err());
    }
}
