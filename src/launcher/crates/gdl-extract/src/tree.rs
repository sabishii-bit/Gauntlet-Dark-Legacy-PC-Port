//! An extracted tree as the extraction plans and writes it. Every path the tree will hold is
//! planned before the first file is written, so that a tree that would not be the same on every
//! system is refused before anything is made: two paths a file system that folds letter case
//! would make one file, a folder spelled in two letter cases, which Windows would make one folder
//! and Linux two, or a file that is also the folder above another. Each file is then made new
//! under the tree's root in the staging, written in blocks with the job's cancel looked at before
//! each and its progress told after, synced, and closed before the next is made. No folder is made
//! but those above a file.

use std::collections::BTreeMap;
use std::collections::btree_map::Entry;
use std::io::{self, Write};

use gdl_files::{Job, NameSet, RelPath, Staging};
use gdl_media::Sha256Digest;
use sha2::{Digest, Sha256};

use crate::{ExtractError, TreeRule};

/// The separator between the components of a tree's paths.
pub(crate) const SEPARATOR: char = '/';

/// Every path an extraction will write, gathered before the first is written.
pub(crate) struct TreePlan {
    /// Every path, under its form in ASCII lower case.
    paths: NameSet,
    /// Every folder above a path, as it was first spelled, under its form in ASCII lower case.
    folders: BTreeMap<String, String>,
}

impl TreePlan {
    /// A plan of no paths.
    pub(crate) fn new() -> TreePlan {
        TreePlan {
            paths: NameSet::default(),
            folders: BTreeMap::new(),
        }
    }

    /// Adds `path` to the plan. It is refused as [`TreeRule::Duplicate`] when the plan holds it
    /// already in any ASCII letter case, and as [`TreeRule::FolderCase`] when a folder above it is
    /// planned already under another spelling.
    pub(crate) fn add(&mut self, path: &RelPath) -> Result<(), ExtractError> {
        self.paths.insert(path.clone()).map_err(|duplicate| {
            let detail = format!("duplicate case-insensitive tree path: {}", duplicate.name);
            ExtractError::tree(TreeRule::Duplicate, detail)
        })?;
        let text = path.as_str();
        for (end, _) in text.match_indices(SEPARATOR) {
            let folder = &text[..end];
            match self.folders.entry(folder.to_ascii_lowercase()) {
                Entry::Vacant(slot) => {
                    slot.insert(folder.to_owned());
                }
                Entry::Occupied(first) if first.get() != folder => {
                    let detail = format!("a folder is spelled {} and {folder}", first.get());
                    return Err(ExtractError::tree(TreeRule::FolderCase, detail));
                }
                Entry::Occupied(_) => {}
            }
        }
        Ok(())
    }

    /// Refuses the plan as [`TreeRule::Conflict`] when one of its paths is also the folder above
    /// another, in any ASCII letter case, since a file and a folder cannot share a name. The
    /// refusal names the path that would have to be both.
    pub(crate) fn finish(&self) -> Result<(), ExtractError> {
        self.paths.conflicts().map_err(|both| {
            let detail = format!("tree file/directory collision: {}", both.name);
            ExtractError::tree(TreeRule::Conflict, detail)
        })
    }
}

/// Writes the files of a tree into a staging, under the tree's root there, and tells the job's
/// progress how many of a total of bytes it has written.
pub(crate) struct TreeWriter<'a> {
    /// The staging the tree is written in.
    staging: &'a Staging,
    /// Where the tree lies in the staging.
    root: RelPath,
    /// How the writing is asked to stop, and where its progress goes.
    job: Job<'a>,
    /// The most bytes written between two looks at the job's cancel.
    block_len: usize,
    /// How many bytes have been written.
    done: u64,
    /// How many bytes are to be written, which the progress is told with each advance.
    total: u64,
}

impl<'a> TreeWriter<'a> {
    /// A writer of the tree at `root` in `staging`, which will write `total` bytes in blocks of at
    /// most `block_len`, for `job`.
    pub(crate) fn new(
        staging: &'a Staging,
        root: &RelPath,
        job: &Job<'a>,
        total: u64,
        block_len: usize,
    ) -> TreeWriter<'a> {
        TreeWriter {
            staging,
            root: root.clone(),
            job: *job,
            block_len,
            done: 0,
            total,
        }
    }

    /// Writes `bytes` as the file `path` of the tree and gives the SHA-256 digest of what it
    /// wrote. The file is made new, with the folders above it, as the staging makes a file: a name
    /// that is taken and a link on the way are refused. The bytes go out in blocks, the job's
    /// cancel looked at before the file is made and before each block and its progress told after
    /// each, and the file is synced and closed before this returns.
    pub(crate) fn write_bytes(
        &mut self,
        path: &RelPath,
        bytes: &[u8],
    ) -> Result<Sha256Digest, ExtractError> {
        self.job.cancel.check()?;
        let staged = self.root.join(path);
        let mut file = self.staging.create_file(&staged)?;
        let written = staged.path_under(self.staging.path());
        let failed = |source: io::Error| ExtractError::Io {
            path: written.clone(),
            source,
        };
        let mut digest = Sha256::new();
        for block in bytes.chunks(self.block_len) {
            self.job.cancel.check()?;
            file.write_all(block).map_err(failed)?;
            digest.update(block);
            self.done += block.len() as u64;
            self.job.progress.advance(self.done, self.total, Some(path));
        }
        file.sync_all().map_err(failed)?;
        drop(file);
        Ok(Sha256Digest(digest.finalize().into()))
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;
    use std::path::Path;

    use gdl_files::{Cancel, DISC_NAMES, FilesError, Job, NoProgress, RelPath, Staging};
    use gdl_fixtures::{link_folder, sha256_hex};
    use gdl_media::{N64Rom, Sha256Digest, SourceName};

    use super::{TreePlan, TreeWriter};
    use crate::n64::{check_n64_with, extract_n64_with};
    use crate::vectors::{
        CancelAt, Folders, Listed, Recorded, STAGING_PREFIX, SWEEP, Told, frozen, listed,
        n64_vectors, root, tree_listing,
    };
    use crate::{CheckedRom, ExtractError, TreeRule};

    /// A path of a tree, as an extraction plans and writes one.
    fn rel(text: &str) -> RelPath {
        RelPath::parse(text, &DISC_NAMES).unwrap_or_else(|error| panic!("{error}"))
    }

    /// Bytes that are not all alike.
    fn noise(len: usize) -> Vec<u8> {
        (0..len).map(|index| (index * 151 % 251) as u8).collect()
    }

    /// Checks that `result` is the tree's refusal by `rule`, in the words `detail` gives.
    #[track_caller]
    fn assert_tree_refusal(result: Result<(), ExtractError>, rule: TreeRule, detail: &str) {
        match result {
            Err(ExtractError::Tree {
                rule: refused,
                detail: said,
            }) => {
                assert_eq!(refused, rule, "{said}");
                assert_eq!(&*said, detail);
            }
            other => panic!("{other:?}"),
        }
    }

    /// The N64 vectors' default image, checked with the sweeps' settings, and the tree the
    /// extractor wrote from it.
    fn checked_default() -> (CheckedRom, Vec<Listed>) {
        let vectors = n64_vectors();
        let name = SourceName::from_units("source.z64".encode_utf16().collect());
        let rom = N64Rom::from_bytes(name, frozen(&vectors, "default")).unwrap();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let checked = check_n64_with(&rom, &SWEEP, &job).unwrap();
        (checked, listed(&vectors["trees"]["default"]))
    }

    /// How many bytes the regular files under `root` hold together, of a tree partly written.
    fn bytes_under(root: &Path) -> u64 {
        if !root.exists() {
            return 0;
        }
        let mut total = 0;
        let mut folders = vec![root.to_path_buf()];
        while let Some(folder) = folders.pop() {
            for entry in fs::read_dir(folder).unwrap() {
                let entry = entry.unwrap();
                let kind = entry.file_type().unwrap();
                if kind.is_dir() {
                    folders.push(entry.path());
                } else {
                    total += entry.metadata().unwrap().len();
                }
            }
        }
        total
    }

    #[test]
    fn files_land_under_the_root_with_their_digests() {
        let folders = Folders::new();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        // A root of two folders, as an installation's tree lies, and files of no block, of one
        // short block, of whole blocks and of several blocks and a part.
        let root = rel("n64/extracted");
        let files = [
            ("empty.bin", Vec::new()),
            ("a/one.bin", noise(5)),
            ("a/b/whole.bin", noise(32)),
            ("a/several.bin", noise(37)),
        ];
        let total: u64 = files.iter().map(|(_, bytes)| bytes.len() as u64).sum();
        let cancel = Cancel::default();
        let progress = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        let mut writer = TreeWriter::new(&staging, &root, &job, total, 16);
        for (path, bytes) in &files {
            let digest = writer.write_bytes(&rel(path), bytes).unwrap();
            assert_eq!(digest, Sha256Digest::of(bytes), "{path}");
        }

        // Each file lies under the root with what was written, and the staging holds nothing
        // else: no folder but those above a file.
        let mut expected: Vec<Listed> = files
            .iter()
            .map(|(path, bytes)| {
                let path = format!("extracted/{path}");
                (path, bytes.len() as u64, sha256_hex(bytes))
            })
            .collect();
        expected.sort();
        assert_eq!(tree_listing(&staging.path().join("n64")), expected);
        let top: Vec<_> = fs::read_dir(staging.path()).unwrap().collect();
        assert_eq!(top.len(), 1);

        // Every block was told once written, with the bytes written so far of the whole and the
        // path it belonged to: none for the empty file.
        let advance = |done: u64, item: &str| Told::Advance {
            done,
            total,
            item: Some(item.to_owned()),
        };
        let told = [
            advance(5, "a/one.bin"),
            advance(21, "a/b/whole.bin"),
            advance(37, "a/b/whole.bin"),
            advance(53, "a/several.bin"),
            advance(69, "a/several.bin"),
            advance(74, "a/several.bin"),
        ];
        assert_eq!(progress.told(), told);

        // Asked to stop, the writer makes no file more.
        cancel.cancel();
        let stopped = writer.write_bytes(&rel("late.bin"), b"late");
        assert!(
            matches!(stopped, Err(ExtractError::Cancelled)),
            "{stopped:?}"
        );
        assert!(
            !staging
                .path()
                .join("n64")
                .join("extracted")
                .join("late.bin")
                .exists()
        );
        assert_eq!(progress.told().len(), told.len());
    }

    #[test]
    fn a_folder_spelled_two_ways_is_refused() {
        // The N64 extractor's case: one folder, `Font`, given again as `font`.
        let mut plan = TreePlan::new();
        plan.add(&rel("Font/a")).unwrap();
        plan.add(&rel("Font/c")).unwrap();
        let refused = plan.add(&rel("font/b"));
        assert_tree_refusal(
            refused,
            TreeRule::FolderCase,
            "a folder is spelled Font and font",
        );

        // At any depth, each folder by its whole path; a folder spelled alike each time is one.
        let mut plan = TreePlan::new();
        plan.add(&rel("a/Font/x")).unwrap();
        plan.add(&rel("a/Font/y/z")).unwrap();
        let refused = plan.add(&rel("a/font/y"));
        assert_tree_refusal(
            refused,
            TreeRule::FolderCase,
            "a folder is spelled a/Font and a/font",
        );
        let mut plan = TreePlan::new();
        plan.add(&rel("a/Font/x")).unwrap();
        let refused = plan.add(&rel("A/Font/y"));
        assert_tree_refusal(refused, TreeRule::FolderCase, "a folder is spelled a and A");

        // Files that differ in letter case from one another but not from their folders are
        // planned, and so are names that only begin alike.
        let mut plan = TreePlan::new();
        for path in ["Font/a", "Font/B", "Fonts/a", "Font2/a", "font.bin"] {
            plan.add(&rel(path)).unwrap();
        }
        plan.finish().unwrap();
    }

    #[test]
    fn a_path_planned_twice_in_any_letter_case_is_refused() {
        // As two executable overlays named alike would be, which their reader does not hold to
        // one another.
        for (first, second) in [
            ("_program/game.bin", "_program/game.bin"),
            ("_program/game.bin", "_program/GAME.bin"),
            ("manifest.json", "Manifest.JSON"),
        ] {
            let mut plan = TreePlan::new();
            plan.add(&rel(first)).unwrap();
            let detail = format!("duplicate case-insensitive tree path: {second}");
            assert_tree_refusal(plan.add(&rel(second)), TreeRule::Duplicate, &detail);
        }
    }

    #[test]
    fn a_file_that_is_another_files_folder_is_refused() {
        // Whichever comes first, and in any letter case: the refusal names the path that would
        // have to be a file and a folder.
        let cases: [(&[&str], &str); 4] = [
            (&["a", "a/b"], "a"),
            (&["x/y/z", "x/y"], "x/y"),
            (&["A", "a/b"], "A"),
            (&["lib/x.so", "LIB"], "LIB"),
        ];
        for (paths, both) in cases {
            let mut plan = TreePlan::new();
            for path in paths {
                plan.add(&rel(path)).unwrap();
            }
            let detail = format!("tree file/directory collision: {both}");
            assert_tree_refusal(plan.finish(), TreeRule::Conflict, &detail);
        }
        // Names that only begin alike are no file and folder.
        let mut plan = TreePlan::new();
        for path in ["a", "ab/c", "a.b/c", "a b"] {
            plan.add(&rel(path)).unwrap();
        }
        plan.finish().unwrap();
    }

    #[test]
    fn a_cancel_at_any_block_leaves_only_the_staging() {
        let (checked, golden) = checked_default();
        let folders = Folders::new();

        // A whole extraction, in blocks short enough that each of the default tree's files spans
        // several, counts its advances and the bytes written by each.
        let recorded = Recorded::default();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &recorded,
        };
        extract_n64_with(&checked, &staging, &root(), &SWEEP, &job).unwrap();
        drop(staging);
        let mut written = vec![0];
        let mut phase_start = 0;
        for told in recorded.told() {
            match told {
                Told::Phase(_) => phase_start = *written.last().unwrap(),
                Told::Advance { done, .. } => written.push(phase_start + done),
            }
        }
        let advances = written.len() - 1;
        let whole: u64 = golden.iter().map(|(_, size, _)| size).sum();
        assert_eq!(written[advances], whole);
        assert!(advances > golden.len() * 2, "{advances} advances");

        // Asked to stop before any block, or after any but the last, the extraction stops before
        // its next block, with only what it wrote in the staging and nothing outside it changed;
        // asked after the last, it is done, and the tree is whole.
        for (at, &written_before) in written.iter().enumerate() {
            let before = folders.before();
            let cancel = Cancel::default();
            let progress = CancelAt::new(&cancel, at);
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
            let extracted = extract_n64_with(&checked, &staging, &root(), &SWEEP, &job);
            let tree = root().path_under(staging.path());
            if at < advances {
                assert!(
                    matches!(extracted, Err(ExtractError::Cancelled)),
                    "{at}: {extracted:?}"
                );
                assert_eq!(bytes_under(&tree), written_before, "{at}");
            } else {
                extracted.unwrap();
                assert_eq!(tree_listing(&tree), golden);
            }
            folders.assert_outside_unchanged(&before);
            drop(staging);
            folders.assert_staging_gone(&before);
        }
    }

    #[test]
    fn a_taken_name_fails_without_touching_anything_outside_the_staging() {
        let (checked, golden) = checked_default();
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        // Each file of the tree in turn, its name taken in the staging by a folder before the
        // extraction comes to it.
        for (path, _, _) in &golden {
            let before = folders.before();
            let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
            let taken = root().join(&rel(path));
            staging.create_dir(&taken).unwrap();
            let extracted = extract_n64_with(&checked, &staging, &root(), &SWEEP, &job);
            let at = taken.path_under(staging.path());
            match extracted {
                Err(ExtractError::Files(FilesError::Io {
                    path: refused,
                    source,
                })) => {
                    assert_eq!(refused, at, "{path}");
                    assert_eq!(source.kind(), io::ErrorKind::AlreadyExists, "{path}");
                }
                other => panic!("{path}: {other:?}"),
            }
            // The folder that took the name is as it was made: nothing was written into it.
            assert_eq!(fs::read_dir(&at).unwrap().count(), 0, "{path}");
            folders.assert_outside_unchanged(&before);
            drop(staging);
            folders.assert_staging_gone(&before);
        }
    }

    #[test]
    fn a_link_planted_in_the_staging_is_refused() {
        let (checked, _) = checked_default();
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let kept = fs::read_dir(&folders.kept).unwrap().count();
        // A link where the tree's root goes, and one where the first folder under it goes, each
        // to a folder outside the staging.
        for planted in ["n64", "n64/levels"] {
            let before = folders.before();
            let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
            let link = rel(planted).path_under(staging.path());
            fs::create_dir_all(link.parent().unwrap()).unwrap();
            link_folder(&link, &folders.kept).unwrap();
            let extracted = extract_n64_with(&checked, &staging, &root(), &SWEEP, &job);
            match &extracted {
                Err(error @ ExtractError::Files(FilesError::Link { path })) => {
                    assert_eq!(*path, link, "{planted}");
                    assert_eq!(error.rule(), "link");
                }
                other => panic!("{planted}: {other:?}"),
            }
            // Nothing reached the folder the link leads to.
            assert_eq!(fs::read_dir(&folders.kept).unwrap().count(), kept);
            folders.assert_outside_unchanged(&before);
            drop(staging);
            folders.assert_staging_gone(&before);
        }
        // The folder the links led to is still there, as it was.
        assert_eq!(
            fs::read(folders.kept.join("kept.txt")).unwrap(),
            b"kept".to_vec()
        );
    }
}
