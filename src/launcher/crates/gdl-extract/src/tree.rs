//! An extracted tree as the extraction plans and writes it. Every path the tree will hold is
//! planned before the first file is written, so that a tree that would not be the same on every
//! system is refused before anything is made: two paths a file system that folds letter case
//! would make one file, a folder spelled in two letter cases, which Windows would make one folder
//! and Linux two, or a file that is also the folder above another. Each file is then made new
//! under the tree's root in the staging, written in blocks with the job's cancel looked at before
//! each and its progress told after, synced, and closed before the next is made, whether its
//! bytes are at hand or a reader streams them. No folder is made but those above a file. A file
//! written already may be read back, only through real folders.

use std::collections::BTreeMap;
use std::collections::btree_map::Entry;
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::PathBuf;

use gdl_files::{Job, NameSet, RelPath, Staging, no_links};
use gdl_media::Sha256Digest;
use sha2::{Digest, Sha256};

use crate::{ExtractError, TreeRule};

/// The separator between the components of a tree's paths.
pub(crate) const SEPARATOR: char = '/';

/// What a reader writing through a [`TreeFile`] is told when a block is refused or stopped. The
/// writer reports why in its own terms, so these words reach no report.
const FAILED_WRITE: &str = "the write into the staged file stopped";

/// What a staged file is refused as on being read back when what holds its name is no regular
/// file, such as a folder.
const NO_REGULAR_FILE: &str = "the path names no regular file";

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
        self.write_with(path, |file| file.put(bytes))
    }

    /// Writes the file `path` of the tree with what `fill` writes into it, as a reader streams a
    /// file's bytes, and gives the SHA-256 digest of what was written. The file is made as
    /// [`write_bytes`](Self::write_bytes) makes one, and what `fill` writes goes out in blocks of
    /// at most the writer's length however it is handed over, each written as `write_bytes`
    /// writes one. A block the file refuses, or a block the job's cancel stops, fails the file as
    /// the writer reports it, whatever `fill` makes of the refusal it is given; any other failure
    /// of `fill` is its own. The file is synced and closed before this returns.
    pub(crate) fn write_with(
        &mut self,
        path: &RelPath,
        fill: impl FnOnce(&mut TreeFile<'_>) -> Result<(), ExtractError>,
    ) -> Result<Sha256Digest, ExtractError> {
        self.job.cancel.check()?;
        let created = self.staging.create_file(&self.root.join(path))?;
        let mut file = TreeFile {
            file: created,
            path: self.staged_path(path),
            item: path,
            digest: Sha256::new(),
            job: self.job,
            block_len: self.block_len,
            done: &mut self.done,
            total: self.total,
            failure: None,
        };
        let filled = fill(&mut file);
        if let Some(failure) = file.failure.take() {
            return Err(failure);
        }
        filled?;
        file.finish()
    }

    /// The file `path` of the tree, written already, open for reading, as an extraction reads back
    /// what it staged. It is reached only through real folders, as the staging made them, and
    /// only when it is a regular file: a link at its name or at a folder above it is refused, and
    /// so is a folder or anything else that holds its name, which is never opened.
    pub(crate) fn read_staged(&self, path: &RelPath) -> Result<File, ExtractError> {
        let staged = self.staged_path(path);
        no_links(&staged)?;
        let failed = |source: io::Error| ExtractError::Io {
            path: staged.clone(),
            source,
        };
        let not_regular = || failed(io::Error::new(io::ErrorKind::InvalidInput, NO_REGULAR_FILE));
        if !fs::symlink_metadata(&staged).map_err(failed)?.is_file() {
            return Err(not_regular());
        }
        let file = File::open(&staged).map_err(failed)?;
        // What took the name after it was looked at is not read either.
        if !file.metadata().map_err(failed)?.is_file() {
            return Err(not_regular());
        }
        Ok(file)
    }

    /// Where the file `path` of the tree lies in the staging, which a failure to write or read it
    /// names.
    pub(crate) fn staged_path(&self, path: &RelPath) -> PathBuf {
        self.root.join(path).path_under(self.staging.path())
    }
}

/// A file of the tree being written: the file in the staging, open for writing, with the digest of
/// what has gone into it, and the writer's progress, which each block it writes moves on. A reader
/// streams bytes into it through [`Write`], and a writer that has them at hand through
/// [`put`](TreeFile::put).
pub(crate) struct TreeFile<'b> {
    /// The file, made new in the staging.
    file: File,
    /// Where it lies, which a failure to write it names.
    path: PathBuf,
    /// Its path in the tree, which its progress names.
    item: &'b RelPath,
    /// The digest of what has been written into it.
    digest: Sha256,
    /// How the writing is asked to stop, and where its progress goes.
    job: Job<'b>,
    /// The most bytes written between two looks at the job's cancel.
    block_len: usize,
    /// How many bytes the writer has written, this file's included.
    done: &'b mut u64,
    /// How many bytes the writer is to write.
    total: u64,
    /// The failure a block written through [`Write`] met, which the writer reports in its own
    /// terms in place of what the reader makes of it.
    failure: Option<ExtractError>,
}

impl TreeFile<'_> {
    /// Writes `bytes` into the file in blocks of at most the writer's length: the job's cancel is
    /// looked at before each block, and after it the block is taken into the digest and the
    /// writer's progress told how far it has come.
    pub(crate) fn put(&mut self, bytes: &[u8]) -> Result<(), ExtractError> {
        for block in bytes.chunks(self.block_len) {
            self.put_block(block)?;
        }
        Ok(())
    }

    /// Writes one block of [`put`](TreeFile::put)'s into the file.
    fn put_block(&mut self, block: &[u8]) -> Result<(), ExtractError> {
        self.job.cancel.check()?;
        self.file
            .write_all(block)
            .map_err(|source| ExtractError::Io {
                path: self.path.clone(),
                source,
            })?;
        self.digest.update(block);
        *self.done += block.len() as u64;
        self.job
            .progress
            .advance(*self.done, self.total, Some(self.item));
        Ok(())
    }

    /// Syncs and closes the file, and gives the digest of what was written into it.
    fn finish(self) -> Result<Sha256Digest, ExtractError> {
        self.file.sync_all().map_err(|source| ExtractError::Io {
            path: self.path.clone(),
            source,
        })?;
        drop(self.file);
        Ok(Sha256Digest(self.digest.finalize().into()))
    }
}

impl Write for TreeFile<'_> {
    /// Writes one block of `bytes`, at most the writer's length, as [`TreeFile::put`] writes
    /// each. A block that fails is kept for the writer to report, and the reader is given only a
    /// stand-in for it; once one has failed, no more is written.
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.is_empty() {
            return Ok(0);
        }
        if self.failure.is_some() {
            return Err(io::Error::other(FAILED_WRITE));
        }
        let block = &bytes[..bytes.len().min(self.block_len)];
        match self.put_block(block) {
            Ok(()) => Ok(block.len()),
            Err(failure) => {
                self.failure = Some(failure);
                Err(io::Error::other(FAILED_WRITE))
            }
        }
    }

    /// Nothing is held back from the file: each block is written as it is given.
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::fs::{self, File};
    use std::io::{self, Read, Write};

    use gdl_files::{Cancel, DISC_NAMES, FilesError, Job, RelPath, Staging};
    use gdl_fixtures::{DiscSet, Edc, link_folder, sha256_hex};
    use gdl_media::{MediaError, N64Rom, Sha256Digest, SourceName};
    use sha2::{Digest, Sha256};

    use super::{FAILED_WRITE, TreeFile, TreePlan, TreeWriter};
    use crate::dreamcast::extract_dreamcast_with;
    use crate::n64::{check_n64_with, extract_n64_with};
    use crate::vectors::{
        CancelAt, DISC_SWEEP, Folders, Listed, Recorded, STAGING_PREFIX, SWEEP, Told, bytes_under,
        dreamcast_root, dreamcast_vectors, frozen, listed, n64_vectors, quiet, root, tree_listing,
        written_after_each_advance,
    };
    use crate::{CheckedRom, ExtractError, NativeRule, TreeRule, check_dreamcast};

    /// A path of a tree, as an extraction plans and writes one.
    fn rel(text: &str) -> RelPath {
        RelPath::parse(text, &DISC_NAMES).unwrap_or_else(|error| panic!("{error}"))
    }

    /// Bytes that are not all alike.
    fn noise(len: usize) -> Vec<u8> {
        (0..len).map(|index| (index * 151 % 251) as u8).collect()
    }

    /// What a reader makes of a write its writer refused, as a disc's copy does: a failure of its
    /// own, naming the disc file it was copying.
    fn refused_by_the_writer(source: io::Error) -> ExtractError {
        let file = rel("SIZES.ROM");
        MediaError::Write { file, source }.into()
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
        let checked = check_n64_with(&rom, &SWEEP, &quiet(&cancel)).unwrap();
        (checked, listed(&vectors["trees"]["default"]))
    }

    /// Checks that `extract`, run into a new staging in which the file `path` of the tree at
    /// `root` is taken by a folder before the extraction comes to it, fails as the staging refuses
    /// that name, writes nothing into the folder, and changes nothing outside the staging, which
    /// goes once dropped.
    #[track_caller]
    fn assert_taken_name_fails(
        folders: &Folders,
        root: &RelPath,
        path: &str,
        extract: impl FnOnce(&Staging) -> Result<(), ExtractError>,
    ) {
        let before = folders.before();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let taken = root.join(&rel(path));
        staging.create_dir(&taken).unwrap();
        let at = taken.path_under(staging.path());
        match extract(&staging) {
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
        let written = written_after_each_advance(&recorded.told());
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
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        // Each file of each expansion's tree in turn, its name taken in the staging by a folder
        // before the extraction comes to it: the N64 default image's, from one check, and the
        // Dreamcast full set's, from one check whose disc each extraction reads again.
        let (checked, golden) = checked_default();
        assert!(!golden.is_empty());
        for (path, _, _) in &golden {
            assert_taken_name_fails(&folders, &root(), path, |staging| {
                extract_n64_with(&checked, staging, &root(), &SWEEP, &job).map(drop)
            });
        }
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let mut disc = check_dreamcast(&sheet, &job).unwrap();
        let golden = listed(&dreamcast_vectors()["sets"]["full"]["tree"]);
        assert!(!golden.is_empty());
        let disc_root = dreamcast_root();
        for (path, _, _) in &golden {
            assert_taken_name_fails(&folders, &disc_root, path, |staging| {
                extract_dreamcast_with(&mut disc, staging, &disc_root, &DISC_SWEEP, &job).map(drop)
            });
        }
    }

    #[test]
    fn streamed_files_land_in_blocks_with_their_digests() {
        let folders = Folders::new();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let root = rel("dreamcast");
        let bytes = noise(57);
        let cancel = Cancel::default();
        let progress = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        let total = bytes.len() as u64 + 5;
        let mut writer = TreeWriter::new(&staging, &root, &job, total, 16);
        // A reader hands its bytes over in pieces of its own: each piece no longer than a block
        // goes out as one block, a longer one in blocks, and an empty one not at all.
        let pieces = [7, 40, 0, 10];
        assert_eq!(pieces.iter().sum::<usize>(), bytes.len());
        let streamed = writer
            .write_with(&rel("disc/AUDIO/X.STR"), |file| {
                let mut at = 0;
                for piece in pieces {
                    let piece = &bytes[at..at + piece];
                    file.write_all(piece).map_err(refused_by_the_writer)?;
                    at += piece.len();
                }
                file.flush().map_err(refused_by_the_writer)
            })
            .unwrap();
        assert_eq!(streamed, Sha256Digest::of(&bytes));
        // A writer that has its bytes at hand puts them in blocks as well.
        let put = writer
            .write_with(&rel("disc/DISK.ROM"), |file| file.put(b"hello"))
            .unwrap();
        assert_eq!(put, Sha256Digest::of(b"hello"));

        // Each file lies under the root with what was written.
        let tree = root.path_under(staging.path());
        let expected = vec![
            ("disc/AUDIO/X.STR".to_owned(), 57, sha256_hex(&bytes)),
            ("disc/DISK.ROM".to_owned(), 5, sha256_hex(b"hello")),
        ];
        assert_eq!(tree_listing(&tree), expected);
        // Every block was told once written, with the bytes written so far of the whole.
        let advance = |done: u64, item: &str| Told::Advance {
            done,
            total,
            item: Some(item.to_owned()),
        };
        let told = [
            advance(7, "disc/AUDIO/X.STR"),
            advance(23, "disc/AUDIO/X.STR"),
            advance(39, "disc/AUDIO/X.STR"),
            advance(47, "disc/AUDIO/X.STR"),
            advance(57, "disc/AUDIO/X.STR"),
            advance(62, "disc/DISK.ROM"),
        ];
        assert_eq!(progress.told(), told);
    }

    #[test]
    fn a_streamed_file_fails_as_its_writer_reports_it() {
        let folders = Folders::new();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let root = rel("dreamcast");

        // Asked to stop once the first block is written: the next block is refused, and so is
        // every write after it. The file fails as the writer stops, whatever the reader makes of
        // the refusal it was given, and holds only the first block.
        let cancel = Cancel::default();
        let progress = CancelAt::new(&cancel, 1);
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        let mut writer = TreeWriter::new(&staging, &root, &job, 80, 16);
        let mut refusals = Vec::new();
        let stopped = writer.write_with(&rel("disc/SIZES.ROM"), |file| {
            for _ in 0..2 {
                let refused = file.write_all(&noise(40)).unwrap_err();
                refusals.push(refused.kind());
            }
            Err(refused_by_the_writer(io::ErrorKind::Other.into()))
        });
        assert!(
            matches!(stopped, Err(ExtractError::Cancelled)),
            "{stopped:?}"
        );
        assert_eq!(refusals.len(), 2);
        let staged = rel("disc/SIZES.ROM").path_under(&root.path_under(staging.path()));
        assert_eq!(fs::read(&staged).unwrap(), noise(40)[..16]);

        // A failure of the reader's own, which no write met, is its own.
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let mut writer = TreeWriter::new(&staging, &root, &job, 5, 16);
        let failed = writer.write_with(&rel("native/a.wad"), |file| {
            file.put(b"hel")?;
            Err(ExtractError::native(NativeRule::DiskTruncated, "cut short"))
        });
        match failed {
            Err(error @ ExtractError::Native { .. }) => assert_eq!(error.rule(), "diskTruncated"),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn a_block_the_system_refuses_is_kept_with_its_answer() {
        // A file open only for reading, so that the system refuses every block written to it,
        // as it refuses one on a full disk.
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("DISK.ROM");
        fs::write(&path, b"").unwrap();
        let item = rel("disc/DISK.ROM");
        let cancel = Cancel::default();
        let mut done = 0;
        let mut file = TreeFile {
            file: File::open(&path).unwrap(),
            path: path.clone(),
            item: &item,
            digest: Sha256::new(),
            job: quiet(&cancel),
            block_len: 16,
            done: &mut done,
            total: 5,
            failure: None,
        };
        // The reader is given a stand-in; the writer keeps the system's own answer, naming the
        // staged file, so that what reports the failure can tell a full disk from other refusals.
        let refused = file.write_all(b"hello").unwrap_err();
        assert_eq!(refused.to_string(), FAILED_WRITE);
        match file.failure.take() {
            Some(ExtractError::Io {
                path: failed,
                source,
            }) => {
                assert_eq!(failed, path);
                assert!(source.raw_os_error().is_some(), "{source:?}");
            }
            other => panic!("{other:?}"),
        }
        // Nothing went into the file, and no progress was told.
        drop(file);
        assert_eq!(done, 0);
        assert_eq!(fs::read(&path).unwrap(), b"");
    }

    #[test]
    fn staged_files_are_read_back_only_through_real_folders() {
        let folders = Folders::new();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let root = rel("dreamcast");
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let mut writer = TreeWriter::new(&staging, &root, &job, 15, 16);
        for (path, bytes) in [
            ("disc/DISK.ROM", b"hello"),
            ("disc/SIZES.ROM", b"table"),
            ("other/x.bin", b"other"),
        ] {
            writer.write_bytes(&rel(path), bytes).unwrap();
        }
        let staged = |path: &str| writer.staged_path(&rel(path));
        assert_eq!(
            staged("disc/DISK.ROM"),
            root.path_under(staging.path())
                .join("disc")
                .join("DISK.ROM")
        );

        // What was written reads back as it was written.
        let mut read = Vec::new();
        let mut file = writer.read_staged(&rel("disc/DISK.ROM")).unwrap();
        file.read_to_end(&mut read).unwrap();
        assert_eq!(read, b"hello");
        drop(file);

        // A name nothing holds fails as the system answers, naming where it was looked for; a
        // folder at the name is no file to read back, and is never opened.
        match writer.read_staged(&rel("disc/none.bin")) {
            Err(ExtractError::Io { path, source }) => {
                assert_eq!(path, staged("disc/none.bin"));
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("{other:?}"),
        }
        fs::create_dir(staged("disc/folder")).unwrap();
        match writer.read_staged(&rel("disc/folder")) {
            Err(ExtractError::Io { path, source }) => {
                assert_eq!(path, staged("disc/folder"));
                assert_eq!(source.kind(), io::ErrorKind::InvalidInput);
            }
            other => panic!("{other:?}"),
        }

        // A link at the name, or at a folder above it, is refused, and what it leads to is not
        // read: each leads to the folder outside the staging that holds `kept.txt`.
        fs::remove_file(staged("disc/SIZES.ROM")).unwrap();
        link_folder(&staged("disc/SIZES.ROM"), &folders.kept).unwrap();
        fs::remove_dir_all(staged("other")).unwrap();
        link_folder(&staged("other"), &folders.kept).unwrap();
        for (path, link) in [
            ("disc/SIZES.ROM", "disc/SIZES.ROM"),
            ("other/kept.txt", "other"),
        ] {
            match writer.read_staged(&rel(path)) {
                Err(ExtractError::Files(FilesError::Link { path: found })) => {
                    assert_eq!(found, staged(link), "{path}");
                }
                other => panic!("{path}: {other:?}"),
            }
        }
        drop(writer);
        drop(staging);
        assert_eq!(
            fs::read(folders.kept.join("kept.txt")).unwrap(),
            b"kept".to_vec()
        );
    }

    #[test]
    fn a_link_planted_in_the_staging_is_refused() {
        let (checked, _) = checked_default();
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
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
