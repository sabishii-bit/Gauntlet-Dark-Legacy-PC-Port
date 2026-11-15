//! Staging: a folder of the launcher's own, made new inside the folder it installs into, where a
//! job makes everything it will put in place, and from which each finished root is put in place
//! by a rename that never replaces anything. What a job leaves in it goes when it is dropped.

use std::collections::BTreeSet;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io;
use std::path::Path;

use crate::{FilesError, NameSet, RelPath, no_links, rename_noreplace};

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
/// whatever it still holds.
#[derive(Debug)]
pub struct Staging {
    /// The folder, removed with what is left in it when the staging is dropped.
    dir: tempfile::TempDir,
}

impl Staging {
    /// Makes a new staging folder in `dest`, which must have no link on its path, named `prefix`
    /// and then [`STAGING_RANDOM_CHARS`] random ASCII letters and digits. The prefix is one of the
    /// launcher's own, such as `install-`, and holds no separator.
    pub fn create_in(dest: &Path, prefix: &str) -> Result<Staging, FilesError> {
        no_links(dest)?;
        let dir = tempfile::Builder::new()
            .prefix(prefix)
            .rand_bytes(STAGING_RANDOM_CHARS)
            .tempdir_in(dest)
            .map_err(|source| FilesError::io(dest, source))?;
        Ok(Staging { dir })
    }

    /// The staging folder, absolute.
    pub fn path(&self) -> &Path {
        self.dir.path()
    }

    /// Makes the folder `path` in the staging, and each folder above it, one at a time, where it
    /// is not there yet. Each must then be a real folder: a link is refused, and so is a file.
    pub fn create_dir(&self, path: &RelPath) -> Result<(), FilesError> {
        make_folders(&path.path_under(self.path()), path.depth())
    }

    /// Makes the file `path` in the staging, new, for writing, with the folders above it made as
    /// [`create_dir`](Staging::create_dir) makes them. Anything already at the name refuses it, a
    /// link included, which is never followed. Close the file before publishing the root it is
    /// in: Windows refuses to rename a folder while a file inside it is open.
    pub fn create_file(&self, path: &RelPath) -> Result<File, FilesError> {
        let file = path.path_under(self.path());
        make_folders(file.parent().unwrap_or(self.path()), path.depth() - 1)?;
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&file)
            .map_err(|source| FilesError::io(&file, source))
    }

    /// Puts each of `roots` in place in the folder `into`, by a rename that never replaces what
    /// is there, and records each in `journal` once it has moved. The caller holds the lock of
    /// `into`, and has closed every file it made in the staging.
    ///
    /// Before anything moves, each root must be a single name that the staging holds, exactly as
    /// it is written there, named once in any letter case, and neither the staged tree nor
    /// `into`'s path may hold a link. The roots then move in byte order, and the first failure
    /// stops the publish and is returned: a target that is there already is
    /// [`FilesError::Exists`]. The roots moved before it stay in place and recorded, and a root
    /// that moved but whose sync or record then failed stays in place unrecorded. Those not moved
    /// stay in the staging, for the caller's journal to decide whether they are moved too or what
    /// moved is taken back, until the staging is dropped.
    pub fn publish_new(
        &self,
        roots: &[RelPath],
        into: &Path,
        journal: &mut dyn Journal,
    ) -> Result<(), FilesError> {
        // A name the staging holds at its top has no separator, so a deeper name is never one.
        let staged = self.staged_names()?;
        let mut named = NameSet::default();
        for root in roots {
            let held = staged.contains(OsStr::new(root.as_str()));
            if !held || named.insert(root.clone()).is_err() {
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
            rename_noreplace(&root.path_under(self.path()), &target).map_err(|source| {
                if source.kind() == io::ErrorKind::AlreadyExists {
                    FilesError::Exists {
                        path: target.clone(),
                    }
                } else {
                    FilesError::io(&target, source)
                }
            })?;
            journal.published(root)?;
        }
        Ok(())
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

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs;
    use std::io::{self, Write};
    use std::path::{Path, PathBuf};

    use super::{Journal, STAGING_RANDOM_CHARS, Staging};
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

    /// The names in `folder`, sorted.
    fn names(folder: &Path) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(folder)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
        names.sort();
        names
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

    /// Links `link` to the folder `target` as each system can without privilege: a junction on
    /// Windows, a symbolic link elsewhere.
    fn link_folder(target: &Path, link: &Path) {
        #[cfg(windows)]
        gdl_fixtures::make_junction(link, target).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(target, link).unwrap();
    }

    /// Makes `link` a symbolic link to the file `target`, which need not exist.
    fn symlink_file(target: &Path, link: &Path) -> io::Result<()> {
        #[cfg(windows)]
        return std::os::windows::fs::symlink_file(target, link);
        #[cfg(unix)]
        return std::os::unix::fs::symlink(target, link);
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

    /// Checks that `result` refused the link at `link`.
    fn assert_link<T: std::fmt::Debug>(result: Result<T, FilesError>, link: &Path) {
        match result {
            Err(FilesError::Link { path }) => assert_eq!(path, link),
            other => panic!("{other:?}"),
        }
    }

    /// Checks that `result` is the system's refusal of an operation on `at`, and gives back what
    /// the system answered.
    fn assert_io<T: std::fmt::Debug>(result: Result<T, FilesError>, at: &Path) -> io::Error {
        match result {
            Err(FilesError::Io { path, source }) => {
                assert_eq!(path, at);
                source
            }
            other => panic!("{other:?}"),
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
            let name = staging.path().file_name().unwrap().to_str().unwrap();
            let random = name.strip_prefix(prefix).unwrap();
            assert_eq!(random.len(), STAGING_RANDOM_CHARS, "{name}");
            assert!(
                random.bytes().all(|byte| byte.is_ascii_alphanumeric()),
                "{name}"
            );
            assert!(staging.path().is_absolute());
            assert_eq!(staging.path().parent(), Some(install.state.as_path()));
            assert!(fs::symlink_metadata(staging.path()).unwrap().is_dir());
            assert!(names(staging.path()).is_empty());
        }
        assert_ne!(first.path(), second.path());
        assert_eq!(names(&install.state).len(), 3);

        // A destination with a link on its path is refused, and nothing is made through it.
        let link = install.root.join("linked");
        link_folder(&install.outside, &link);
        assert_link(Staging::create_in(&link, "install-"), &link);
        assert_link(Staging::create_in(&link.join("deeper"), "install-"), &link);
        assert!(names(&install.outside).is_empty());

        // A destination that is not there is the system's error.
        let missing = install.root.join("missing");
        let source = assert_io(Staging::create_in(&missing, "install-"), &missing);
        assert_eq!(source.kind(), io::ErrorKind::NotFound);
        assert!(fs::symlink_metadata(&missing).is_err());
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

        // A name that is taken refuses the file, whatever takes it, and is left as it was.
        for taken in ["VERSION", "data/text/en.json", "data/text", "data"] {
            let at = rel(taken).path_under(staging.path());
            assert_io(staging.create_file(&rel(taken)), &at);
        }
        let at = staging.path().join("VERSION");
        let source = assert_io(staging.create_file(&rel("VERSION")), &at);
        assert_eq!(source.kind(), io::ErrorKind::AlreadyExists);
        assert_eq!(tree(staging.path()), made);

        // A link at the name is never followed: nothing is made where it leads.
        let link = staging.path().join("shaders");
        link_folder(&install.outside, &link);
        assert_io(staging.create_file(&rel("shaders")), &link);
        assert!(names(&install.outside).is_empty());
        let kept = install.outside.join("kept.json");
        fs::write(&kept, b"kept").unwrap();
        let nowhere = install.outside.join("nowhere.json");
        for (name, target) in [("config.json", &kept), ("dangling.json", &nowhere)] {
            let link = staging.path().join(name);
            if let Err(error) = symlink_file(target, &link) {
                eprintln!("skipped the file links: this account cannot make them ({error})");
                break;
            }
            let source = assert_io(staging.create_file(&rel(name)), &link);
            assert_eq!(source.kind(), io::ErrorKind::AlreadyExists, "{name}");
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
        link_folder(&install.outside, &link);
        for name in ["shaders", "shaders/spirv", "shaders/spirv/deeper"] {
            assert_link(staging.create_dir(&rel(name)), &link);
        }
        for name in ["shaders/test.spv", "shaders/spirv/test.spv"] {
            assert_link(staging.create_file(&rel(name)), &link);
        }
        let deeper = staging.path().join("data").join("linked");
        link_folder(&install.outside, &deeper);
        assert_link(staging.create_file(&rel("data/linked/x/y.json")), &deeper);
        assert_link(staging.create_dir(&rel("data/linked/x")), &deeper);
        assert!(names(&install.outside).is_empty());

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
        assert!(names(staging.path()).is_empty());

        // Publishing nothing moves nothing.
        staging
            .publish_new(&[], &install.root, &mut journal)
            .unwrap();
        assert_eq!(journal.roots.len(), roots.len());

        // Dropped, the staging folder goes and what it published stays.
        let folder = staging.path().to_path_buf();
        drop(staging);
        assert!(fs::symlink_metadata(&folder).is_err());
        assert!(names(&install.state).is_empty());
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
        assert_eq!(names(&install.root), [".gdl-launcher"]);
        assert!(names(&install.outside).is_empty());
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
                None => link_folder(&install.outside, &link),
                Some(target) => {
                    if let Err(error) = symlink_file(target, &link) {
                        eprintln!(
                            "skipped the file links: this account cannot make them ({error})"
                        );
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

        // A link on the way to the staging itself, put where its folder was.
        let real_state = install.state.with_file_name(".gdl-launcher-real");
        fs::rename(&install.state, &real_state).unwrap();
        link_folder(&real_state, &install.state);
        let mut journal = Recorder::new(&staging, &install.root);
        let publish = staging.publish_new(&roots, &install.root, &mut journal);
        assert_link(publish, &install.state);
        clear(&install.state);
        fs::rename(&real_state, &install.state).unwrap();
        assert_unmoved(&install, &staging, &before, &journal);

        // A link on the way to the folder published into.
        let linked_root = install.root.with_file_name("linked root");
        link_folder(&install.root, &linked_root);
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
            // Named twice.
            (&["data", "VERSION", "data"][..], "data"),
        ] {
            let mut journal = Recorder::new(&staging, &install.root);
            match staging.publish_new(&rels(roots), &install.root, &mut journal) {
                Err(FilesError::NotRoot { name }) => assert_eq!(&*name, refused, "{roots:?}"),
                other => panic!("{roots:?}: {other:?}"),
            }
            assert_unmoved(&install, &staging, &before, &journal);
        }
    }

    /// Two roots that differ only in letter case, which Linux can stage side by side, would be
    /// one name on a file system that folds case, so the second is refused as named twice.
    #[cfg(target_os = "linux")]
    #[test]
    fn refuses_roots_that_differ_only_in_letter_case() {
        let install = install();
        let staging = Staging::create_in(&install.state, "install-").unwrap();
        stage(&staging, "data/config.json", b"{}");
        stage(&staging, "Data/config.json", b"{}");
        let mut journal = Recorder::new(&staging, &install.root);
        match staging.publish_new(&rels(&["data", "Data"]), &install.root, &mut journal) {
            Err(FilesError::NotRoot { name }) => assert_eq!(&*name, "Data"),
            other => panic!("{other:?}"),
        }
        assert!(journal.roots.is_empty());
        assert_eq!(names(&install.root), [".gdl-launcher"]);
        assert_eq!(names(staging.path()), ["Data", "data"]);
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
            names(&install.root),
            [".gdl-launcher", "a-first", "b-second"]
        );
        assert_eq!(fs::read(&taken).unwrap(), b"kept");
        assert_eq!(names(staging.path()), ["b-second", "c-third"]);

        // Dropped, the staging takes them with it, and nothing published goes.
        drop(staging);
        assert!(names(&install.state).is_empty());
        assert_eq!(
            names(&install.root),
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
            names(&install.root),
            [".gdl-launcher", "a-first", "b-second"]
        );
        assert_eq!(names(staging.path()), ["c-third"]);
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
                link_folder(outside, target)
            }),
            ("a link to nothing", |target, outside| {
                let gone = outside.join("gone");
                fs::create_dir(&gone).unwrap();
                link_folder(&gone, target);
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
            assert!(names(&install.outside).is_empty(), "{kind}");
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
        link_folder(&install.outside, &staging.path().join("linked"));
        let folder = staging.path().to_path_buf();
        drop(staging);
        assert!(fs::symlink_metadata(&folder).is_err());
        assert_eq!(names(&install.state), ["receipt.json"]);
        assert_eq!(names(&install.outside), ["kept.json"]);
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
        assert_eq!(names(&install.root), [".gdl-launcher"]);
        // The mode comes back before the staging goes, so that it can be removed whole.
        drop(denied);
        let folder = staging.path().to_path_buf();
        drop(staging);
        assert!(fs::symlink_metadata(folder).is_err());
    }
}
