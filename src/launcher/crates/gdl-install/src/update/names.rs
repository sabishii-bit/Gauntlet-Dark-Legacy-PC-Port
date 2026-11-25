//! The names an update moves: those of the installed release and of the new one, held to the
//! runtime's name rules again, so that neither record can name a file of the player's; a managed
//! name that would change only its letter case; a new name over something the launcher did not
//! put there; links on the way to any of them; and which names change, stay or go.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io;
use std::path::Path;

use gdl_files::{FilesError, PAYLOAD_NAMES, RelPath, is_absent, no_links};

use super::UpdateError;
use crate::payload::{BUILD_INFO, BuildInfo, PayloadError, check_root};

/// Which runtime names an update moves, each list in byte order and every name of either release
/// in exactly one of them or counted as unchanged.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileChanges {
    /// The names both releases hold, whose bytes the new one changes: `build-info.json` always,
    /// since it records the version.
    pub replaced: Vec<RelPath>,
    /// The names only the new release holds.
    pub added: Vec<RelPath>,
    /// The names only the installed release holds.
    pub removed: Vec<RelPath>,
    /// How many names both releases hold with the same bytes, which the update leaves in place.
    pub unchanged: usize,
}

impl FileChanges {
    /// The installed names the update moves aside before it puts the new ones in place: the
    /// replaced and the removed, in byte order.
    pub fn backup(&self) -> Vec<RelPath> {
        merged(&self.replaced, &self.removed)
    }

    /// The new names the update puts in place: the replaced and the added, in byte order.
    pub fn publish(&self) -> Vec<RelPath> {
        merged(&self.replaced, &self.added)
    }
}

/// The names of `one` and `other`, in byte order.
fn merged(one: &[RelPath], other: &[RelPath]) -> Vec<RelPath> {
    let mut names: Vec<RelPath> = one.iter().chain(other).cloned().collect();
    names.sort();
    names
}

/// Judges the names an update of `installed` to `new` moves, in the installation at `root`: every
/// name of either a payload name with a runtime root; no name that changes only its letter case;
/// every new name free at `root`, its folders included, and reached through no link; and gives
/// what changes.
///
/// A record's names are its `files` and `build-info.json`, so that the receipt is never among
/// them. An installed name the runtime's rules refuse is [`UpdateError::InstalledNames`], and a
/// new one [`UpdateError::Runtime`] with the rule it breaks. A new name that equals an installed
/// one in ASCII letter case alone is [`UpdateError::CaseChange`]: on a file system that ignores
/// case it would land on the installed file, and on one that does not beside it. Then, in byte
/// order of every name of either, a link on the way to it is [`UpdateError::Links`], and for a
/// name new to the installation anything at it, or anything but a folder at one of the folders
/// above it, is [`UpdateError::Unowned`], since the launcher never writes over what it did not
/// put there.
///
/// A name in both records with the same digest is unchanged, unless `stage_executable` and it is
/// the executable, which an update then stages whatever its bytes; one with another digest is
/// replaced, as is `build-info.json`; one in the new record alone is added, and one in the
/// installed record alone removed. Nothing is written, and no file is opened.
pub(crate) fn judge_names(
    root: &Path,
    installed: &BuildInfo,
    new: &BuildInfo,
    stage_executable: bool,
) -> Result<FileChanges, UpdateError> {
    let old_names = names_of(installed, |name, _| UpdateError::InstalledNames {
        name: name.into(),
    })?;
    let new_names = names_of(new, |_, error| UpdateError::Runtime(error))?;
    check_letter_case(&old_names, &new_names)?;
    for name in old_names.union(&new_names) {
        no_links(&name.path_under(root)).map_err(|error| links_error(error, root))?;
        if !old_names.contains(name) {
            check_unowned(root, name)?;
        }
    }
    let mut changes = FileChanges {
        replaced: Vec::new(),
        added: Vec::new(),
        removed: Vec::new(),
        unchanged: 0,
    };
    for name in &new_names {
        if !old_names.contains(name) {
            changes.added.push(name.clone());
            continue;
        }
        let text = name.as_str();
        let same = text != BUILD_INFO
            && installed.files.get(text) == new.files.get(text)
            && !(stage_executable && text == new.executable);
        if same {
            changes.unchanged += 1;
        } else {
            changes.replaced.push(name.clone());
        }
    }
    changes.removed = old_names.difference(&new_names).cloned().collect();
    Ok(changes)
}

/// The names of the release record `record`, its files and `build-info.json`, each held to the
/// payload's name rules and to the roots a runtime keeps its files under; a name refused is the
/// error `refused` makes of it and the rule it breaks.
fn names_of(
    record: &BuildInfo,
    refused: impl Fn(&str, PayloadError) -> UpdateError,
) -> Result<BTreeSet<RelPath>, UpdateError> {
    let listed = record.files.keys().map(String::as_str);
    listed
        .chain([BUILD_INFO])
        .map(|text| {
            RelPath::parse(text, &PAYLOAD_NAMES)
                .map_err(|error| PayloadError::Name { error })
                .and_then(|name| check_root(&name).map(|()| name))
                .map_err(|error| refused(text, error))
        })
        .collect()
}

/// Refuses a new name that equals an installed one in ASCII letter case alone.
fn check_letter_case(
    old_names: &BTreeSet<RelPath>,
    new_names: &BTreeSet<RelPath>,
) -> Result<(), UpdateError> {
    let folded: BTreeMap<String, &RelPath> =
        old_names.iter().map(|name| (name.folded(), name)).collect();
    for name in new_names {
        if let Some(&old) = folded.get(&name.folded())
            && old != name
        {
            return Err(UpdateError::CaseChange {
                old: old.as_str().into(),
                new: name.as_str().into(),
            });
        }
    }
    Ok(())
}

/// Refuses `name`, which the installed release does not hold, when anything is at it under `root`,
/// or anything but a folder at one of the folders above it, looked at without following a link.
fn check_unowned(root: &Path, name: &RelPath) -> Result<(), UpdateError> {
    let mut path = root.to_path_buf();
    let mut components = name.components().peekable();
    while let Some(component) = components.next() {
        path.push(component);
        let folder = components.peek().is_some();
        match fs::symlink_metadata(&path) {
            Ok(metadata) if folder && metadata.is_dir() => {}
            Ok(_) => {
                return Err(UpdateError::Unowned {
                    name: name.as_str().into(),
                });
            }
            // Nothing below a missing name can be there either.
            Err(error) if is_absent(&error) => return Ok(()),
            Err(source) => return Err(UpdateError::Io { path, source }),
        }
    }
    Ok(())
}

/// The update's refusal for what a link check under `root` reported: a link and the system's
/// refusal keep their kinds, and anything else would count as the latter.
fn links_error(error: FilesError, root: &Path) -> UpdateError {
    match error {
        FilesError::Link { path } => UpdateError::Links { path },
        FilesError::Io { path, source } => UpdateError::Io { path, source },
        other => UpdateError::Io {
            path: root.to_path_buf(),
            source: io::Error::other(other),
        },
    }
}

#[cfg(test)]
mod tests {
    use std::fs;

    use gdl_files::{NameRule, PAYLOAD_NAMES, RelPath, retry_locked};
    use gdl_fixtures::{
        InstallFixture, RuntimeFiles, snapshot, symlink_file_where_allowed,
        symlink_folder_where_allowed,
    };
    use gdl_media::Sha256Digest;
    use tempfile::TempDir;

    use super::{FileChanges, judge_names};
    use crate::payload::{BuildInfo, PayloadError};
    use crate::test_platform::this_platform;
    use crate::update::UpdateError;

    /// The version the tests' installations are of.
    const INSTALLED: &str = "1.0.0-beta";

    /// The version of the release the tests update them to.
    const NEXT: &str = "1.0.0-beta.1";

    /// What the player's save holds.
    const SAVE_BYTES: &[u8] = b"user-owned";

    /// An installation of the standard runtime for the platform this test program was built for,
    /// as the launcher leaves one, in a folder of the test's own.
    fn installation() -> (TempDir, InstallFixture) {
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(this_platform().as_str(), INSTALLED);
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        (dir, fixture)
    }

    /// The release record of `runtime`, as a runtime carries it.
    fn record_of(runtime: &RuntimeFiles) -> BuildInfo {
        BuildInfo::parse(&runtime.build_info_bytes()).unwrap()
    }

    /// Judges the update of the fixture's installation to `new`, its executable staged only when
    /// it changes.
    fn judge(fixture: &InstallFixture, new: &RuntimeFiles) -> Result<FileChanges, UpdateError> {
        judge_names(
            &fixture.root,
            &record_of(&fixture.runtime),
            &record_of(new),
            false,
        )
    }

    /// `texts`, each as a name.
    fn names(texts: &[&str]) -> Vec<RelPath> {
        texts
            .iter()
            .map(|text| RelPath::parse(text, &PAYLOAD_NAMES).unwrap())
            .collect()
    }

    #[test]
    fn the_names_that_change_stay_and_go_are_told_apart() {
        let (_dir, fixture) = installation();
        let executable = this_platform().executable();
        let next = fixture.runtime.next(NEXT);
        let changes = judge(&fixture, &next).unwrap();
        assert_eq!(
            changes,
            FileChanges {
                replaced: names(&["VERSION", "build-info.json", "data/text/en.json"]),
                added: names(&["shaders/next.spv"]),
                removed: names(&["shaders/test.spv"]),
                unchanged: 3,
            }
        );
        // The backups and the publishes, each in byte order.
        assert_eq!(
            changes.backup(),
            names(&[
                "VERSION",
                "build-info.json",
                "data/text/en.json",
                "shaders/test.spv"
            ])
        );
        assert_eq!(
            changes.publish(),
            names(&[
                "VERSION",
                "build-info.json",
                "data/text/en.json",
                "shaders/next.spv"
            ])
        );

        // An executable staged whatever its bytes is replaced, though its digest is the same.
        let staged = judge_names(
            &fixture.root,
            &record_of(&fixture.runtime),
            &record_of(&next),
            true,
        )
        .unwrap();
        let mut replaced = vec![
            "VERSION",
            "build-info.json",
            "data/text/en.json",
            executable,
        ];
        replaced.sort_unstable();
        assert_eq!(staged.replaced, names(&replaced));
        assert_eq!(staged.unchanged, 2);
        assert_eq!(
            (staged.added, staged.removed),
            (changes.added, changes.removed)
        );

        // The same release again changes its record alone.
        let same = judge(&fixture, &fixture.runtime).unwrap();
        assert_eq!(same.replaced, names(&["build-info.json"]));
        assert!(same.added.is_empty() && same.removed.is_empty());
        assert_eq!(same.unchanged, fixture.runtime.files.len());
    }

    #[test]
    fn a_managed_path_that_changes_case_is_refused() {
        let (_dir, fixture) = installation();
        let mut next = fixture.runtime.next(NEXT);
        let text = next
            .files
            .iter_mut()
            .find(|(name, _)| name == "data/text/en.json")
            .unwrap();
        text.0 = "data/Text/en.json".to_owned();
        let before = snapshot(&fixture.root).unwrap();
        match judge(&fixture, &next) {
            Err(UpdateError::CaseChange { old, new }) => {
                assert_eq!(&*old, "data/text/en.json");
                assert_eq!(&*new, "data/Text/en.json");
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(snapshot(&fixture.root).unwrap(), before);
    }

    #[test]
    fn a_new_name_over_a_players_file_is_refused() {
        // A file the player made where the new release puts one.
        let (_dir, fixture) = installation();
        let newer = fixture.runtime.newer(NEXT);
        let players = fixture.root.join("shaders").join("new.spv");
        fs::write(&players, b"personal").unwrap();
        let before = snapshot(&fixture.root).unwrap();
        assert_unowned(judge(&fixture, &newer), "shaders/new.spv");
        assert_eq!(snapshot(&fixture.root).unwrap(), before);

        // A folder in its place is no more the launcher's.
        retry_locked(|| fs::remove_file(&players)).unwrap();
        fs::create_dir(&players).unwrap();
        assert_unowned(judge(&fixture, &newer), "shaders/new.spv");

        // A file where the new name needs a folder.
        let (_dir, fixture) = installation();
        let shaders = fixture.root.join("shaders");
        retry_locked(|| fs::remove_dir_all(&shaders)).unwrap();
        fs::write(&shaders, b"personal").unwrap();
        let before = snapshot(&fixture.root).unwrap();
        assert_unowned(judge(&fixture, &newer), "shaders/new.spv");
        assert_eq!(snapshot(&fixture.root).unwrap(), before);

        // With nothing there, the name is the update's to make.
        let (_dir, fixture) = installation();
        let changes = judge(&fixture, &newer).unwrap();
        assert_eq!(changes.added, names(&["shaders/new.spv"]));
    }

    /// Checks that `judged` refused the new name `name` as one over something not the launcher's.
    #[track_caller]
    fn assert_unowned(judged: Result<FileChanges, UpdateError>, name: &str) {
        match judged {
            Err(UpdateError::Unowned { name: refused }) => assert_eq!(&*refused, name),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn a_linked_runtime_file_is_refused_and_nothing_outside_is_read() {
        let (dir, fixture) = installation();
        let outside = dir.path().join("outside.json");
        fs::write(&outside, b"{}").unwrap();
        let config = fixture.root.join("data").join("config.json");
        retry_locked(|| fs::remove_file(&config)).unwrap();
        if !symlink_file_where_allowed(&config, &outside) {
            eprintln!("skipped the link to a file: this account may not make one");
            return;
        }
        let outside_before = snapshot(dir.path()).unwrap();
        match judge(&fixture, &fixture.runtime.next(NEXT)) {
            Err(UpdateError::Links { path }) => assert_eq!(path, config),
            other => panic!("{other:?}"),
        }
        // The file the link leads to, and the link, are as they were.
        assert_eq!(snapshot(dir.path()).unwrap(), outside_before);
        assert_eq!(fs::read(&outside).unwrap(), b"{}");
        assert!(fs::symlink_metadata(&config).unwrap().is_symlink());
    }

    #[test]
    fn the_records_cannot_claim_a_players_file() {
        let (_dir, fixture) = installation();
        let save = fixture.root.join("saves").join("hero.json");
        fs::write(&save, SAVE_BYTES).unwrap();
        let next = record_of(&fixture.runtime.next(NEXT));
        let claiming = |mut record: BuildInfo| {
            record
                .files
                .insert("saves/hero.json".to_owned(), Sha256Digest::of(SAVE_BYTES));
            record
        };

        // An installed record that claims the save, or names a file outside the installation.
        let installed = claiming(record_of(&fixture.runtime));
        match judge_names(&fixture.root, &installed, &next, false) {
            Err(UpdateError::InstalledNames { name }) => assert_eq!(&*name, "saves/hero.json"),
            other => panic!("{other:?}"),
        }
        let mut outside = record_of(&fixture.runtime);
        outside
            .files
            .insert("../hero.json".to_owned(), Sha256Digest::of(SAVE_BYTES));
        match judge_names(&fixture.root, &outside, &next, false) {
            Err(UpdateError::InstalledNames { name }) => assert_eq!(&*name, "../hero.json"),
            other => panic!("{other:?}"),
        }

        // A new record that claims it.
        match judge_names(
            &fixture.root,
            &record_of(&fixture.runtime),
            &claiming(next.clone()),
            false,
        ) {
            Err(UpdateError::Runtime(PayloadError::UnknownRoot { name })) => {
                assert_eq!(&*name, "saves/hero.json");
            }
            other => panic!("{other:?}"),
        }
        let mut escaping = next;
        escaping
            .files
            .insert("../hero.json".to_owned(), Sha256Digest::of(SAVE_BYTES));
        match judge_names(
            &fixture.root,
            &record_of(&fixture.runtime),
            &escaping,
            false,
        ) {
            Err(UpdateError::Runtime(PayloadError::Name { error })) => {
                assert_eq!(error.rule, NameRule::Dot);
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(fs::read(&save).unwrap(), SAVE_BYTES);
    }

    #[test]
    fn an_installation_reached_through_a_link_is_refused() {
        let (dir, fixture) = installation();
        let moved = dir.path().join("moved");
        retry_locked(|| fs::rename(&fixture.root, &moved)).unwrap();
        if !symlink_folder_where_allowed(&fixture.root, &moved) {
            eprintln!("skipped the link to a folder: this account may not make one");
            return;
        }
        match judge(&fixture, &fixture.runtime.next(NEXT)) {
            Err(UpdateError::Links { path }) => assert_eq!(path, fixture.root),
            other => panic!("{other:?}"),
        }
    }
}
