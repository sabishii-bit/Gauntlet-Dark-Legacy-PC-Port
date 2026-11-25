//! Where an update takes the new release from: the launcher file that runs, which carries the
//! release it was built with, or a release the feed found, whose checksum list and launcher file
//! are downloaded into a folder of the state folder's and checked before anything else of it is
//! read. Either launcher file's sections are read against their table before anything of the
//! runtime is trusted, its runtime is held to the release it is said to carry, and the whole file
//! is copied as the new launcher copy. A release's Linux launcher file is an archive of the program
//! alone, whose one member is read out beside it and held to the rules the alpha's updater held its
//! own archive to.

use std::fmt;
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use flate2::read::GzDecoder;
use gdl_files::{BLOCK_LEN, Cancel, PAYLOAD_NAMES, RelPath, Staging, fill_from, retry_locked};
use gdl_release::{
    MAX_ASSET_LEN, Platform, Release, ReleaseError, ReleaseFiles, Version, linux_member,
};
use tar::{Archive, Entry, EntryType};

use super::journal::DOWNLOAD_PREFIX;
use super::stage::close_synced;
#[cfg(unix)]
use super::stage::{PROGRAM_MODE, set_mode};
use super::{
    DOWNLOAD, Update, UpdateError, UpdateFailure, UpdateOutcome, files_error, payload_error,
    sections_error, unchanged,
};
use crate::faults::FaultPoint;
use crate::folder::InstallLock;
use crate::install::{Changes, remove_staging};
use crate::payload::{Payload, PayloadError};
use crate::recover::Recovered;
use crate::sections::{SectionView, Sections, SectionsError};

/// What the alpha's updater said of a Linux launcher archive whose first entry is not a regular
/// file of the release's member name, of a size within its bound.
const INVALID_ARCHIVE: &str = "Invalid Linux launcher archive";

/// What it said of an archive that ends inside its member.
const TRUNCATED_ARCHIVE: &str = "Truncated launcher archive";

/// What it said of an archive that holds more than its member.
const EXTRA_ENTRY: &str = "Unexpected extra launcher archive entry";

/// How many copies of a release's launcher file a download keeps on the volume: the Linux
/// archive and the member read out of it, or the Windows program alone.
const fn download_copies(platform: Platform) -> u64 {
    match platform {
        Platform::WindowsX64 => 1,
        Platform::LinuxX64 => 2,
    }
}

/// Where an update takes the new game and launcher from.
pub enum UpdateSource<'a> {
    /// This launcher file, through the handle it opened as it started, which reads the file that
    /// started even once another has taken its path.
    Bundled(&'a mut Sections<File>),
    /// A release the feed found, whose checksum list and launcher file the update fetches through
    /// `files`: the feed, or a test's stand-in that holds the files itself.
    Release {
        /// Where the release's files are fetched from.
        files: &'a dyn ReleaseFiles,
        /// The release.
        release: &'a Release,
    },
}

impl UpdateSource<'_> {
    /// The version of the release the source brings, for the launcher of version `running`: the
    /// launcher file that runs carries the runtime of its own version alone, and a release's
    /// launcher file must carry the release's own.
    pub(super) fn version<'v>(&'v self, running: &'v Version) -> &'v Version {
        match self {
            UpdateSource::Bundled(_) => running,
            UpdateSource::Release { release, .. } => &release.version,
        }
    }
}

/// The launcher file an update stages the new release from.
pub(super) enum LauncherFile<'s> {
    /// The launcher file that runs, whose runtime is of the running launcher's version.
    Own(&'s mut Sections<File>),
    /// A release's launcher file, downloaded and checked.
    Downloaded(&'s Download<'s>),
}

/// A release's files, downloaded and checked, in a download folder of the state folder's.
pub(super) struct Download<'r> {
    /// The download folder, kept: once the update's journal is written it names the folder, and
    /// until then a failure removes it, or the next start's sweep does.
    pub(super) folder: Staging,
    /// The launcher file to stage from: the launcher asset on Windows, the archive's member on
    /// Linux.
    launcher: PathBuf,
    /// The release whose files these are.
    pub(super) release: &'r Release,
}

impl Download<'_> {
    /// The downloaded launcher file's sections, found as the launcher finds its own: a file that
    /// carries none is refused as one.
    pub(super) fn sections(&self) -> Result<Sections<File>, UpdateError> {
        Sections::of_path(&self.launcher)
            .map_err(sections_error)?
            .ok_or(UpdateError::Source(SectionsError::NoSections))
    }
}

impl Update<'_> {
    /// Updates the installation from `release`, its files fetched through `files`: downloaded and
    /// checked, then staged and swapped in as the launcher file that runs is. The download folder
    /// goes with the update's cleanup, or with whatever stops it: the journal's rollback removes
    /// it once the journal names it, and before that the update itself, with `lock` still held, so
    /// that no other launcher's sweep takes the folder meanwhile; what is left for the next start
    /// stays for it.
    pub(super) fn apply_release(
        &self,
        files: &dyn ReleaseFiles,
        release: &Release,
        lock: InstallLock,
        recovered: Recovered,
    ) -> Result<UpdateOutcome, UpdateFailure> {
        let download = self.download(files, release)?;
        let staged = match self.stage(LauncherFile::Downloaded(&download)) {
            Ok(staged) => staged,
            // The lock is let go only once the folder is taken back.
            Err(failure) => return Err(self.download_taken_back(failure, download.folder)),
        };
        // From here the journal names the download folder, and whatever stops the swap takes it
        // back from the journal before the lock is let go.
        self.swap(staged, lock, recovered)
    }

    /// Downloads `release`'s files through `files` into a download folder made in the state
    /// folder, as [`fetch`](Self::fetch) fetches them. Refused before anything is made when the
    /// release is for another platform, or when the volume of the launcher's state has too little
    /// room for its launcher file, twice on Linux for the archive and its member, and
    /// [`SPACE_MARGIN`](crate::install::SPACE_MARGIN); taken back, the folder with whatever it
    /// holds, when anything fails after.
    fn download<'r>(
        &self,
        files: &dyn ReleaseFiles,
        release: &'r Release,
    ) -> Result<Download<'r>, UpdateFailure> {
        self.begin(DOWNLOAD).map_err(unchanged)?;
        if release.platform != self.platform {
            return Err(unchanged(mismatch(&format_args!(
                "the release is for {}, not {}",
                release.platform.as_str(),
                self.platform.as_str()
            ))));
        }
        let needed = release
            .launcher
            .size
            .saturating_mul(download_copies(self.platform));
        self.check_space(needed).map_err(unchanged)?;
        let mut folder = Staging::create_in(&self.folder.state_dir(), DOWNLOAD_PREFIX)
            .map_err(|error| unchanged(files_error(error)))?;
        folder.keep();
        match self.fetch(files, release, &folder) {
            Ok(launcher) => Ok(Download {
                folder,
                launcher,
                release,
            }),
            Err(error) => Err(self.download_taken_back(unchanged(error), folder)),
        }
    }

    /// Fetches `release`'s checksum list through `files`, checked against its signature when the
    /// launcher trusts a key, then its launcher file into `folder`, held to the list's digest as
    /// to GitHub's; on Linux reads the archive's member out beside it; then passes the step. Gives
    /// the launcher file to stage from. The list and its signature are written into the folder
    /// too, as the release carries them, for whoever looks at what a download left; nothing reads
    /// them back.
    fn fetch(
        &self,
        files: &dyn ReleaseFiles,
        release: &Release,
        folder: &Staging,
    ) -> Result<PathBuf, UpdateError> {
        let checked = files
            .check_list(release, self.job.cancel)
            .map_err(release_error)?;
        write_whole(folder, &release.checksums.name, &checked.bytes)?;
        if let (Some(signature), Some(asset)) = (&checked.signature, &release.signature) {
            write_whole(folder, &asset.name, signature)?;
        }
        let asset = &release.launcher;
        let downloaded = files
            .download(asset, folder.path(), &self.job)
            .map_err(release_error)?;
        if checked.list.digest_of(&asset.name) != Some(downloaded.sha256) {
            return Err(UpdateError::Release(ReleaseError::ChecksumMismatch {
                asset: asset.name.as_str().into(),
            }));
        }
        let launcher = match self.platform {
            Platform::WindowsX64 => downloaded.path,
            Platform::LinuxX64 => linux_member_of(
                &downloaded.path,
                &linux_member(&release.version),
                folder,
                self.job.cancel,
            )?,
        };
        self.pass(FaultPoint::UpdateStage, folder.path())?;
        Ok(launcher)
    }

    /// The update's failure `failure`, once a release's download folder `folder` was made: the
    /// folder goes with whatever it holds, counted gone when the journal's rollback removed it
    /// already. What the update left for the next start, a journal that names the folder perhaps
    /// among it, stays with the folder for that start; a folder that will not go is left for its
    /// sweep.
    fn download_taken_back(&self, failure: UpdateFailure, folder: Staging) -> UpdateFailure {
        if failure.changes == Changes::Kept {
            return failure;
        }
        match remove_staging(folder) {
            Ok(()) => failure,
            Err(_) => UpdateFailure {
                changes: Changes::Kept,
                ..failure
            },
        }
    }

    /// The runtime the launcher file `sections` carries, once every section is read against its
    /// table, at most a block at a time, with the job's cancel looked at before each and its
    /// progress told after; held, as a plan holds it, to the one it is said to carry: of this
    /// program's platform, and of the running launcher's version as text for the launcher file
    /// that runs, or of `release`'s for a release's, whose game must be the installed one's too. A
    /// release's launcher file that carries another release is [`UpdateError::SourceMismatch`].
    pub(super) fn verified_runtime<'s>(
        &self,
        sections: &'s mut Sections<File>,
        release: Option<&Release>,
    ) -> Result<Payload<SectionView<'s, File>>, UpdateError> {
        sections.verify(&self.job).map_err(sections_error)?;
        let payload = sections.runtime().map_err(payload_error)?;
        let Some(release) = release else {
            payload
                .for_launcher(&self.running.to_string())
                .map_err(payload_error)?;
            return Ok(payload);
        };
        payload
            .for_launcher(&release.version.to_string())
            .map_err(|error| match error {
                PayloadError::OtherPlatform { .. } | PayloadError::OtherVersion { .. } => {
                    mismatch(&error)
                }
                other => payload_error(other),
            })?;
        let carried = &payload.build_info().executable;
        if *carried != self.installed.executable {
            return Err(mismatch(&format_args!(
                "its game is {carried}, not {}",
                self.installed.executable
            )));
        }
        Ok(payload)
    }
}

/// The refusal of a release whose files are not the ones the update asked for, for `detail`.
fn mismatch(detail: &dyn fmt::Display) -> UpdateError {
    UpdateError::SourceMismatch {
        detail: detail.to_string().into(),
    }
}

/// The update's failure for what fetching a release's files reported.
fn release_error(error: ReleaseError) -> UpdateError {
    match error {
        ReleaseError::Cancelled => UpdateError::Cancelled,
        other => UpdateError::Release(other),
    }
}

/// Writes `bytes` into the download folder `folder` as the file `name`, made new, synced and
/// closed. A name that is not one plain file name is refused as the release's asset it names,
/// [`ReleaseError::Incomplete`].
fn write_whole(folder: &Staging, name: &str, bytes: &[u8]) -> Result<(), UpdateError> {
    let file_name = RelPath::parse(name, &PAYLOAD_NAMES)
        .ok()
        .filter(|file_name| file_name.depth() == 1)
        .ok_or_else(|| UpdateError::Release(ReleaseError::Incomplete { asset: name.into() }))?;
    let path = file_name.path_under(folder.path());
    let mut file = folder.create_file(&file_name).map_err(files_error)?;
    file.write_all(bytes).map_err(|source| UpdateError::Io {
        path: path.clone(),
        source,
    })?;
    close_synced(file, &path)
}

/// A member of a Linux launcher archive, as it is read.
type Member<'a> = Entry<'a, GzDecoder<File>>;

/// Reads the one member of the Linux launcher archive at `archive`, a gzip-compressed tar archive,
/// out into the download folder `folder` as the file `member`, a program, synced and closed, at
/// most [`BLOCK_LEN`] bytes at a time with `cancel` looked at before each, and gives where it was
/// written. The archive's first entry, its name and size as the archive's own extensions give
/// them, must be a regular file named exactly `member`, of one byte to [`MAX_ASSET_LEN`], whose
/// bytes are all there, and nothing but the archive's end may follow it. Anything else is
/// [`UpdateError::Archive`], naming the entry and its kind, and nothing of the member is left in
/// the folder.
pub(super) fn linux_member_of(
    archive: &Path,
    member: &str,
    folder: &Staging,
    cancel: &Cancel,
) -> Result<PathBuf, UpdateError> {
    let opened = File::open(archive).map_err(|source| UpdateError::Io {
        path: archive.to_path_buf(),
        source,
    })?;
    let mut read = Archive::new(GzDecoder::new(opened));
    let mut entries = read
        .entries()
        .map_err(|error| refused(INVALID_ARCHIVE, &error))?;
    let entry = match entries.next() {
        Some(Ok(entry)) => entry,
        Some(Err(error)) => return Err(refused(INVALID_ARCHIVE, &error)),
        None => return Err(refused(INVALID_ARCHIVE, &"it holds no entry")),
    };
    let size = entry.size();
    if entry.header().entry_type() != EntryType::Regular
        || *entry.path_bytes() != *member.as_bytes()
        || !(1..=MAX_ASSET_LEN).contains(&size)
    {
        return Err(refused(INVALID_ARCHIVE, &described(&entry)));
    }
    let name = RelPath::parse(member, &PAYLOAD_NAMES)
        .ok()
        .filter(|name| name.depth() == 1)
        .ok_or_else(|| refused(INVALID_ARCHIVE, &format_args!("{member:?} is no file name")))?;
    cancel.check()?;
    let path = name.path_under(folder.path());
    let file = folder.create_file(&name).map_err(files_error)?;
    let written =
        write_member(entry, size, file, &path, cancel).and_then(|()| match entries.next() {
            None => Ok(()),
            Some(Ok(extra)) => Err(refused(EXTRA_ENTRY, &described(&extra))),
            Some(Err(error)) => Err(refused(INVALID_ARCHIVE, &error)),
        });
    if let Err(error) = written {
        // A member refused leaves nothing of it for anything to take. One the system will not let
        // go of goes with the folder it was written in.
        let _ = retry_locked(|| fs::remove_file(&path));
        return Err(error);
    }
    Ok(path)
}

/// Copies the `size` bytes of the archive's member `entry` into `file`, at `path`, as
/// [`linux_member_of`] says, then makes it a program on Unix and syncs and closes it.
fn write_member(
    mut entry: Member<'_>,
    size: u64,
    mut file: File,
    path: &Path,
    cancel: &Cancel,
) -> Result<(), UpdateError> {
    let written_at = |source| UpdateError::Io {
        path: path.to_path_buf(),
        source,
    };
    let mut block = vec![0; usize::try_from(size).map_or(BLOCK_LEN, |size| size.min(BLOCK_LEN))];
    let mut left = size;
    while left > 0 {
        cancel.check()?;
        let wanted = usize::try_from(left).map_or(block.len(), |left| left.min(block.len()));
        let read = fill_from(&mut entry, &mut block[..wanted]).map_err(|error| {
            let what = if error.kind() == io::ErrorKind::UnexpectedEof {
                TRUNCATED_ARCHIVE
            } else {
                INVALID_ARCHIVE
            };
            refused(what, &error)
        })?;
        if read < wanted {
            let had = size - left + read as u64;
            return Err(refused(
                TRUNCATED_ARCHIVE,
                &format_args!("{had} of the member's {size} bytes"),
            ));
        }
        file.write_all(&block[..read]).map_err(written_at)?;
        left -= read as u64;
    }
    #[cfg(unix)]
    set_mode(&file, PROGRAM_MODE).map_err(written_at)?;
    close_synced(file, path)
}

/// The refusal of a Linux launcher archive, in the alpha updater's words `what`, with `detail`.
fn refused(what: &str, detail: &dyn fmt::Display) -> UpdateError {
    UpdateError::Archive {
        detail: format!("{what}: {detail}").into(),
    }
}

/// The name, kind and size of an archive's `entry`, as a refusal names it.
fn described(entry: &Member<'_>) -> String {
    format!(
        "{:?}, a {:?} entry of {} bytes",
        String::from_utf8_lossy(&entry.path_bytes()),
        entry.header().entry_type(),
        entry.size()
    )
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    use flate2::Compression;
    use flate2::write::GzEncoder;
    use gdl_files::{Cancel, Staging};
    use gdl_fixtures::{TarEntry, tar_gz};
    use gdl_release::{MAX_ASSET_LEN, Version, linux_member};

    use super::{EXTRA_ENTRY, INVALID_ARCHIVE, TRUNCATED_ARCHIVE, linux_member_of};
    use crate::update::{DOWNLOAD_PREFIX, UpdateError};

    /// The release whose member the tests' archives hold.
    const RELEASE: &str = "1.0.0-beta.1";

    /// What the tests' member holds.
    const PROGRAM: &[u8] = b"the launcher program";

    /// The names in `folder`, sorted.
    fn names_in(folder: &Path) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(folder)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
        names.sort();
        names
    }

    /// Reads the member of the archive `bytes` into a download folder of its own, the archive
    /// beside the folder, with `cancel`; gives what that did and the folder's names after.
    fn read_member(bytes: &[u8], cancel: &Cancel) -> (Result<Vec<u8>, UpdateError>, Vec<String>) {
        let dir = tempfile::tempdir().unwrap();
        let archive = dir.path().join("launcher.tar.gz");
        fs::write(&archive, bytes).unwrap();
        let folder = Staging::create_in(dir.path(), DOWNLOAD_PREFIX).unwrap();
        let member = linux_member(&Version::parse(RELEASE).unwrap());
        let read = linux_member_of(&archive, &member, &folder, cancel).map(|path| {
            assert_eq!(path, folder.path().join(&member));
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
                assert_eq!(mode, 0o755);
            }
            fs::read(&path).unwrap()
        });
        let left = names_in(folder.path());
        (read, left)
    }

    #[test]
    fn the_linux_archive_member_rules() {
        let member = linux_member(&Version::parse(RELEASE).unwrap());
        let dotted = format!("./{member}");
        let cancel = Cancel::default();

        // The one regular file of the member's name is read out whole, a program.
        let sound = tar_gz(&[TarEntry::File {
            name: &member,
            data: PROGRAM,
        }]);
        let (read, left) = read_member(&sound, &cancel);
        assert_eq!(read.unwrap(), PROGRAM);
        assert_eq!(left, [member.as_str()]);

        // A header that promises more than its padding and the archive's two end blocks hold.
        let padded = PROGRAM.len().next_multiple_of(512) as u64;
        let refusals: Vec<(&str, Vec<u8>, &str)> = vec![
            (
                "a symbolic link",
                tar_gz(&[TarEntry::Symlink {
                    name: &member,
                    target: "outside",
                }]),
                INVALID_ARCHIVE,
            ),
            (
                "a hard link",
                tar_gz(&[TarEntry::HardLink {
                    name: &member,
                    target: "outside",
                }]),
                INVALID_ARCHIVE,
            ),
            (
                "a folder",
                tar_gz(&[TarEntry::Folder { name: &member }]),
                INVALID_ARCHIVE,
            ),
            (
                "a name that climbs out",
                tar_gz(&[TarEntry::File {
                    name: "../outside",
                    data: PROGRAM,
                }]),
                INVALID_ARCHIVE,
            ),
            (
                "the name under ./",
                tar_gz(&[TarEntry::File {
                    name: &dotted,
                    data: PROGRAM,
                }]),
                INVALID_ARCHIVE,
            ),
            (
                "another name",
                tar_gz(&[TarEntry::File {
                    name: "GauntletDarkLegacy-1.0.0-beta.1-linux-x64-setup",
                    data: PROGRAM,
                }]),
                INVALID_ARCHIVE,
            ),
            (
                "a second entry",
                tar_gz(&[
                    TarEntry::File {
                        name: &member,
                        data: PROGRAM,
                    },
                    TarEntry::File {
                        name: "unexpected",
                        data: b"more",
                    },
                ]),
                EXTRA_ENTRY,
            ),
            (
                "a member past the bound",
                tar_gz(&[TarEntry::Declared {
                    name: &member,
                    size: MAX_ASSET_LEN + 1,
                    data: PROGRAM,
                }]),
                INVALID_ARCHIVE,
            ),
            (
                "an empty member",
                tar_gz(&[TarEntry::File {
                    name: &member,
                    data: b"",
                }]),
                INVALID_ARCHIVE,
            ),
            (
                "a member that runs short",
                tar_gz(&[TarEntry::Declared {
                    name: &member,
                    size: padded + 1024 + 1,
                    data: PROGRAM,
                }]),
                TRUNCATED_ARCHIVE,
            ),
            ("an archive of no entry", tar_gz(&[]), INVALID_ARCHIVE),
            (
                "a file that is no archive",
                b"not gzip at all".to_vec(),
                INVALID_ARCHIVE,
            ),
        ];
        for (what, bytes, words) in refusals {
            let (read, left) = read_member(&bytes, &cancel);
            match read {
                Err(UpdateError::Archive { detail }) => {
                    assert!(
                        detail.starts_with(&format!("{words}: ")),
                        "{what}: {detail}"
                    );
                }
                other => panic!("{what}: {other:?}"),
            }
            assert!(left.is_empty(), "{what}: {left:?}");
        }

        // The refusal names the entry and its kind.
        let link = tar_gz(&[TarEntry::Symlink {
            name: &member,
            target: "outside",
        }]);
        match read_member(&link, &cancel).0 {
            Err(UpdateError::Archive { detail }) => assert_eq!(
                &*detail,
                format!("{INVALID_ARCHIVE}: {member:?}, a Symlink entry of 0 bytes")
            ),
            other => panic!("{other:?}"),
        }

        // A cancel stops the reading with nothing left.
        let cancelled = Cancel::default();
        cancelled.cancel();
        let (read, left) = read_member(&sound, &cancelled);
        assert!(matches!(read, Err(UpdateError::Cancelled)), "{read:?}");
        assert!(left.is_empty(), "{left:?}");
    }

    #[test]
    fn a_pax_named_member_is_read() {
        let member = linux_member(&Version::parse(RELEASE).unwrap());
        let mut builder = tar::Builder::new(Vec::new());
        builder
            .append_pax_extensions([("path", member.as_bytes())])
            .unwrap();
        let mut header = tar::Header::new_ustar();
        header.set_path("short-name").unwrap();
        header.set_size(PROGRAM.len() as u64);
        header.set_mode(0o755);
        header.set_entry_type(tar::EntryType::Regular);
        header.set_cksum();
        builder.append(&header, PROGRAM).unwrap();
        let archive = builder.into_inner().unwrap();
        let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
        std::io::Write::write_all(&mut encoder, &archive).unwrap();
        let bytes = encoder.finish().unwrap();

        let (read, left) = read_member(&bytes, &Cancel::default());
        assert_eq!(read.unwrap(), PROGRAM);
        assert_eq!(left, [member]);
    }
}
