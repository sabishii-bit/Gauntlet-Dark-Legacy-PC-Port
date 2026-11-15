//! The rules a game runtime's zip is held to before the launcher takes a file from it: an archive
//! as the release scripts write one, whose names cannot leave the installation, name only the
//! runtime's own files and folders, and clash with no other name on a case-insensitive file
//! system; whose release record names the version and the platform it was built for and lists
//! every other file by its SHA-256 digest; and whose files hold exactly what that record lists.

mod build_info;
mod rows;

use std::collections::BTreeSet;
use std::fmt;
use std::io::{self, Read, Seek, Write};

use gdl_files::{BLOCK_LEN, Cancelled, Job, NameError, NameSet, PhaseId, Progress, RelPath};
use gdl_release::Platform;
use sha2::{Digest, Sha256};
use zip::ZipArchive;

use crate::sections::SectionsError;

pub use build_info::BuildInfo;
pub(crate) use build_info::sha256_of;

/// The most entries a runtime may have.
pub const MAX_PAYLOAD_ENTRIES: usize = 20_000;

/// The most bytes a runtime's entries may declare in all, uncompressed.
pub const MAX_PAYLOAD_BYTES: u64 = 2 << 30;

/// The most bytes `build-info.json` may declare.
pub const MAX_BUILD_INFO_LEN: u64 = 4 << 20;

/// The most bytes the `VERSION` file may declare.
pub const MAX_VERSION_FILE_LEN: u64 = 128;

/// The release record at the top of a runtime, which lists every other file by its digest.
pub const BUILD_INFO: &str = "build-info.json";

/// The file at the top of a runtime that holds its version.
pub const VERSION_FILE: &str = "VERSION";

/// The file at the top of a runtime that makes the game keep its settings and saves beside it.
pub const PORTABLE_FLAG: &str = "portable.flag";

/// The game's settings defaults, which every runtime has.
pub const DEFAULT_CONFIG: &str = "data/config.json";

/// The files a runtime may have at its top, beside its platform's executable.
pub const ROOT_FILES: [&str; 3] = [VERSION_FILE, BUILD_INFO, PORTABLE_FLAG];

/// The folders a runtime may have at its top. Only a Linux runtime has `lib`.
pub const ROOT_FOLDERS: [&str; 4] = ["shaders", DATA_FOLDER, "licenses", LIB_FOLDER];

/// The suffix, in any ASCII letter case, of the libraries a Windows runtime has at its top.
pub const LIBRARY_SUFFIX: &str = ".dll";

/// The suffix, in any ASCII letter case, of every file under `data`.
pub const DATA_SUFFIX: &str = ".json";

/// The folder of the settings defaults and text tables, which hold nothing but JSON.
const DATA_FOLDER: &str = "data";

/// The folder of the libraries a Linux runtime carries, where a Windows runtime keeps its own at
/// its top.
const LIB_FOLDER: &str = "lib";

/// The bytes of a SHA-256 digest.
pub(crate) const SHA256_LEN: usize = 32;

/// One file of a runtime zip, as its row in the central directory declares it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PayloadEntry {
    /// The file's name, which keeps to the payload's rules.
    pub name: RelPath,
    /// How many bytes the file declares uncompressed.
    pub size: u64,
    /// The file's place in the archive, which the zip crate numbers its entries by.
    pub index: usize,
}

/// A runtime zip held to the payload's rules: the archive, the entries its central directory
/// declares and the release record it carries, each checked against the others. The bytes of the
/// other files are checked as they are copied out.
#[derive(Debug)]
pub struct Payload<R> {
    /// The archive, as the zip crate reads it.
    archive: ZipArchive<R>,
    /// The archive's entries, in the order of its central directory.
    entries: Vec<PayloadEntry>,
    /// The release record `build-info.json` holds.
    build_info: BuildInfo,
    /// The SHA-256 digest of `build-info.json` as it was read and checked, which every copy of it
    /// must have.
    build_info_sha256: [u8; SHA256_LEN],
}

impl<R: Read + Seek> Payload<R> {
    /// Reads the runtime zip in `reader` and holds it to the payload's rules, refusing it for the
    /// first it breaks, as the variants of [`PayloadError`] name them. `build-info.json` and
    /// `VERSION` are read whole here, the other files only as they are copied.
    pub fn open(mut reader: R) -> Result<Payload<R>, PayloadError> {
        let entries = rows::read_entries(&mut reader)?;
        for entry in &entries {
            check_root(&entry.name)?;
        }
        check_names_apart(&entries)?;
        let record = check_sizes(&entries)?;
        let mut archive = check_with_zip(reader, &entries)?;
        let bytes = read_whole(&mut archive, &entries[record])?;
        let build_info = BuildInfo::parse(&bytes)?;
        for entry in &entries {
            check_platform_root(&entry.name, build_info.platform)?;
        }
        check_inventory(&build_info, &entries)?;
        check_version_file(&mut archive, &entries, &build_info)?;
        Ok(Payload {
            archive,
            entries,
            build_info,
            build_info_sha256: Sha256::digest(&bytes).into(),
        })
    }

    /// The release record the runtime carries.
    pub fn build_info(&self) -> &BuildInfo {
        &self.build_info
    }

    /// The archive's entries, in the order of its central directory.
    pub fn entries(&self) -> &[PayloadEntry] {
        &self.entries
    }

    /// How many bytes the archive's entries declare in all, uncompressed: what installing it
    /// writes.
    pub fn declared_size(&self) -> u64 {
        total_size(&self.entries)
    }

    /// Refuses the runtime unless it is the one a launcher of `launcher_version` installs: built
    /// for the platform this program was built for, and of exactly that version, as text, build
    /// metadata and all.
    pub fn for_launcher(&self, launcher_version: &str) -> Result<(), PayloadError> {
        let platform = self.build_info.platform;
        if Platform::of_this_build() != Some(platform) {
            return Err(PayloadError::OtherPlatform { platform });
        }
        if self.build_info.version != launcher_version {
            return Err(PayloadError::OtherVersion {
                found: self.build_info.version.as_str().into(),
                expected: launcher_version.into(),
            });
        }
        Ok(())
    }

    /// Copies the `index`th entry's bytes into `to`, at most [`BLOCK_LEN`] at a time, and gives
    /// back how many it copied. Before each block it looks at the job's cancel, and after it tells
    /// the job's progress how many of the entry's declared bytes are done. The entry must hold
    /// exactly as many bytes as it declares, with the checksum the archive gives it and the
    /// SHA-256 digest its release record lists, or for `build-info.json` the digest of the record
    /// read when the runtime was opened; whatever the zip crate or the decompressor cannot read
    /// is damage too. What was written before a refusal, never more than one byte past the
    /// entry's declared size, is the caller's to throw away.
    ///
    /// Panics when `index` is not that of one of [`entries`](Self::entries).
    pub fn copy_entry(
        &mut self,
        index: usize,
        to: &mut dyn Write,
        job: &Job<'_>,
    ) -> Result<u64, PayloadError> {
        let entry = &self.entries[index];
        let expected = if entry.name.as_str() == BUILD_INFO {
            &self.build_info_sha256
        } else {
            &self.build_info.files[entry.name.as_str()]
        };
        let file = self
            .archive
            .by_index(index)
            .map_err(|error| damaged(&entry.name, error))?;
        // One byte past the declared size, so that a longer entry is refused with no more written,
        // whatever size the zip crate reads for it.
        let mut file = file.take(entry.size + 1);
        let mut block = block_for(entry.size);
        let mut hasher = Sha256::new();
        let mut done = 0;
        loop {
            job.cancel
                .check()
                .map_err(|Cancelled| PayloadError::Cancelled)?;
            let read = fill(&mut file, &mut block).map_err(|error| damaged(&entry.name, error))?;
            if read > 0 {
                let bytes = &block[..read];
                hasher.update(bytes);
                to.write_all(bytes)?;
                done += read as u64;
                job.progress.advance(done, entry.size, Some(&entry.name));
            }
            // A block left short is one whose last read found the entry's end, which is where the
            // zip crate checks the entry's checksum.
            if read < block.len() {
                break;
            }
        }
        check_len(entry, done)?;
        let digest: [u8; SHA256_LEN] = hasher.finalize().into();
        if digest != *expected {
            return Err(damaged(
                &entry.name,
                "its SHA-256 digest is not the one its release record gives it",
            ));
        }
        Ok(done)
    }

    /// Reads every file the release record lists, in the archive's order, as
    /// [`copy_entry`](Self::copy_entry) copies one, and refuses the runtime for the first that is
    /// not what the record and the archive say. The job's progress is told how many of all those
    /// files' declared bytes are done.
    pub fn verify(&mut self, job: &Job<'_>) -> Result<(), PayloadError> {
        let listed: Vec<usize> = self
            .entries
            .iter()
            .filter(|entry| entry.name.as_str() != BUILD_INFO)
            .map(|entry| entry.index)
            .collect();
        let total = listed.iter().map(|&index| self.entries[index].size).sum();
        let mut before = 0;
        for index in listed {
            let progress = Running {
                job: job.progress,
                before,
                total,
            };
            let part = Job {
                cancel: job.cancel,
                progress: &progress,
            };
            before += self.copy_entry(index, &mut io::sink(), &part)?;
        }
        Ok(())
    }
}

/// The progress of one file of a job over many, told to the job's own progress as part of the
/// whole: the bytes of the files before it and its own, out of the job's total.
struct Running<'a> {
    /// The job's own progress.
    job: &'a dyn Progress,
    /// The bytes of the files done before this one.
    before: u64,
    /// The bytes of every file of the job.
    total: u64,
}

impl Progress for Running<'_> {
    fn phase(&self, phase: PhaseId) {
        self.job.phase(phase);
    }

    fn advance(&self, done: u64, _: u64, item: Option<&RelPath>) {
        self.job.advance(self.before + done, self.total, item);
    }
}

/// A block for copying an entry of `size` bytes: no longer than [`BLOCK_LEN`] or than the entry,
/// and never empty, so that the read that finds the entry's end, and so its checksum, is made.
fn block_for(size: u64) -> Vec<u8> {
    let len = usize::try_from(size).map_or(BLOCK_LEN, |size| size.clamp(1, BLOCK_LEN));
    vec![0; len]
}

/// Reads from `reader` until `block` is full or the reader ends, and gives back how many bytes it
/// holds.
fn fill(reader: &mut impl Read, block: &mut [u8]) -> io::Result<usize> {
    let mut filled = 0;
    while filled < block.len() {
        match reader.read(&mut block[filled..]) {
            Ok(0) => break,
            Ok(read) => filled += read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(filled)
}

/// Reads the whole of `entry`, whose declared size is already held to its limit, through the zip
/// crate, which checks its checksum, and refuses it as damaged unless it holds exactly what it
/// declares.
fn read_whole<R: Read + Seek>(
    archive: &mut ZipArchive<R>,
    entry: &PayloadEntry,
) -> Result<Vec<u8>, PayloadError> {
    let file = archive
        .by_index(entry.index)
        .map_err(|error| damaged(&entry.name, error))?;
    let mut bytes = Vec::new();
    // One byte past the declared size, so that a longer entry is seen without being read whole.
    file.take(entry.size + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| damaged(&entry.name, error))?;
    check_len(entry, bytes.len() as u64)?;
    Ok(bytes)
}

/// Refuses `entry` as damaged unless the `held` bytes read from it are as many as it declares.
fn check_len(entry: &PayloadEntry, held: u64) -> Result<(), PayloadError> {
    if held != entry.size {
        return Err(damaged(
            &entry.name,
            format_args!("it holds {held} bytes and declares {}", entry.size),
        ));
    }
    Ok(())
}

/// The refusal of the entry `name`, whose bytes are not what the archive or the release record
/// says they are, for the reason `detail` gives.
fn damaged(name: &RelPath, detail: impl fmt::Display) -> PayloadError {
    PayloadError::Damaged {
        name: name.as_str().into(),
        detail: detail.to_string().into(),
    }
}

/// The sum of the sizes the entries declare.
fn total_size(entries: &[PayloadEntry]) -> u64 {
    entries.iter().map(|entry| entry.size).sum()
}

/// Refuses a name that does not lie where a runtime keeps its files: one of [`ROOT_FILES`] or a
/// platform's executable at the top, a file under one of [`ROOT_FOLDERS`], or a library at the
/// top, each root in its exact case, and under `data` only JSON. Which platform may have which of
/// these is judged once the release record names the platform.
fn check_root(name: &RelPath) -> Result<(), PayloadError> {
    let root = name.first();
    let whole = || -> Box<str> { name.as_str().into() };
    let is_file = ROOT_FILES.contains(&root)
        || Platform::ALL
            .into_iter()
            .any(|platform| platform.executable() == root);
    if is_file {
        if name.depth() > 1 {
            return Err(PayloadError::FileAsFolder { name: whole() });
        }
        return Ok(());
    }
    if ROOT_FOLDERS.contains(&root) {
        if name.depth() == 1 {
            return Err(PayloadError::FolderAsFile { name: whole() });
        }
        if root == DATA_FOLDER && !ends_with_ignoring_case(name.as_str(), DATA_SUFFIX) {
            return Err(PayloadError::DataNotJson { name: whole() });
        }
        return Ok(());
    }
    if name.depth() == 1 && ends_with_ignoring_case(root, LIBRARY_SUFFIX) {
        return Ok(());
    }
    Err(PayloadError::UnknownRoot { name: whole() })
}

/// Refuses a name that only a runtime for another platform than `platform` has, now that the
/// release record names it: another platform's executable, a library at the top on Linux, and
/// anything under `lib` on Windows.
fn check_platform_root(name: &RelPath, platform: Platform) -> Result<(), PayloadError> {
    let whole = || -> Box<str> { name.as_str().into() };
    let another_executable = Platform::ALL
        .into_iter()
        .any(|other| other != platform && other.executable() == name.as_str());
    if another_executable {
        return Err(PayloadError::Executable { name: whole() });
    }
    let elsewhere = match platform {
        Platform::WindowsX64 => name.first() == LIB_FOLDER,
        Platform::LinuxX64 => {
            name.depth() == 1 && ends_with_ignoring_case(name.as_str(), LIBRARY_SUFFIX)
        }
    };
    if elsewhere {
        return Err(PayloadError::PlatformRoot { name: whole() });
    }
    Ok(())
}

/// Whether `name` ends with `suffix` in any ASCII letter case.
fn ends_with_ignoring_case(name: &str, suffix: &str) -> bool {
    name.len()
        .checked_sub(suffix.len())
        .and_then(|start| name.as_bytes().get(start..))
        .is_some_and(|end| end.eq_ignore_ascii_case(suffix.as_bytes()))
}

/// Refuses two entries of one name, in this or another ASCII letter case, which would be one file
/// on a case-insensitive file system, and an entry that is also the folder above another.
fn check_names_apart(entries: &[PayloadEntry]) -> Result<(), PayloadError> {
    let mut names = NameSet::default();
    for entry in entries {
        names
            .insert(entry.name.clone())
            .map_err(|error| PayloadError::Duplicate { name: error.name })?;
    }
    names
        .conflicts()
        .map_err(|error| PayloadError::Conflict { name: error.name })
}

/// Refuses entries that declare more than [`MAX_PAYLOAD_BYTES`] in all, and a runtime without a
/// `build-info.json` of at most [`MAX_BUILD_INFO_LEN`]. Gives back where `build-info.json` lies
/// among the entries.
fn check_sizes(entries: &[PayloadEntry]) -> Result<usize, PayloadError> {
    let bytes = total_size(entries);
    if bytes > MAX_PAYLOAD_BYTES {
        return Err(PayloadError::TooLarge { bytes });
    }
    let record = entries
        .iter()
        .position(|entry| entry.name.as_str() == BUILD_INFO)
        .ok_or_else(|| PayloadError::Missing {
            name: BUILD_INFO.into(),
        })?;
    let size = entries[record].size;
    if size > MAX_BUILD_INFO_LEN {
        return Err(PayloadError::BuildInfoTooLarge { bytes: size });
    }
    Ok(record)
}

/// Has the zip crate read the archive, and refuses it unless the crate reads the same entries, as
/// many, in the same order and of the same sizes, and finds every local header in agreement with
/// its row and no two entries overlapping. Gives back the archive as the crate reads it.
fn check_with_zip<R: Read + Seek>(
    reader: R,
    entries: &[PayloadEntry],
) -> Result<ZipArchive<R>, PayloadError> {
    let mut archive = ZipArchive::new(reader).map_err(|error| PayloadError::Rows {
        detail: format!("the zip crate refuses the archive: {error}").into(),
    })?;
    let names = entries.iter().map(|entry| entry.name.as_str().as_bytes());
    if !archive.file_names_raw().eq(names) {
        return Err(PayloadError::Rows {
            detail: "the zip crate reads other entries than the central directory's rows".into(),
        });
    }
    // The size the crate reads for an entry is what it bounds the entry's bytes by, so it must be
    // the one the payload's limits were held to.
    for entry in entries {
        let size = archive.by_index_data(entry.index).map(|data| data.size());
        if size.ok() != Some(entry.size) {
            return Err(PayloadError::Rows {
                detail: format!(
                    "the zip crate reads another size for {} than its row declares",
                    entry.name.as_str()
                )
                .into(),
            });
        }
    }
    archive
        .validate_header_consistency()
        .map_err(|error| PayloadError::LocalHeader {
            detail: error.to_string().into(),
        })?;
    Ok(archive)
}

/// Refuses a release record that does not list exactly the archive's entries but itself, by their
/// exact names.
fn check_inventory(build_info: &BuildInfo, entries: &[PayloadEntry]) -> Result<(), PayloadError> {
    let held: BTreeSet<&str> = entries
        .iter()
        .map(|entry| entry.name.as_str())
        .filter(|name| *name != BUILD_INFO)
        .collect();
    let unlisted = held
        .iter()
        .copied()
        .find(|name| !build_info.files.contains_key(*name));
    let absent = || {
        build_info
            .files
            .keys()
            .map(String::as_str)
            .find(|name| !held.contains(name))
    };
    match unlisted.or_else(absent) {
        Some(name) => Err(PayloadError::Inventory { name: name.into() }),
        None => Ok(()),
    }
}

/// Refuses a `VERSION` file that declares more than [`MAX_VERSION_FILE_LEN`] bytes, or does not
/// hold, in UTF-8, the release record's version with nothing but ASCII whitespace around it.
fn check_version_file<R: Read + Seek>(
    archive: &mut ZipArchive<R>,
    entries: &[PayloadEntry],
    build_info: &BuildInfo,
) -> Result<(), PayloadError> {
    let entry = entries
        .iter()
        .find(|entry| entry.name.as_str() == VERSION_FILE)
        .ok_or_else(|| PayloadError::Missing {
            name: VERSION_FILE.into(),
        })?;
    if entry.size > MAX_VERSION_FILE_LEN {
        return Err(PayloadError::VersionFile);
    }
    let bytes = read_whole(archive, entry)?;
    match std::str::from_utf8(&bytes) {
        Ok(text) if text.trim_ascii() == build_info.version => Ok(()),
        _ => Err(PayloadError::VersionFile),
    }
}

/// Why a runtime zip is refused: the rule it breaks, and the values that break it, as technical
/// detail for logs. [`PayloadError::rule`] names the rule.
#[derive(Debug, thiserror::Error)]
pub enum PayloadError {
    /// The archive does not end in a ZIP end record without a comment, as a runtime's does.
    #[error("Not a runtime zip: it does not end in a ZIP end record without a comment")]
    Archive,
    /// The archive has ZIP64 records or values, which no runtime needs.
    #[error("The runtime zip uses ZIP64, which no runtime needs")]
    Zip64,
    /// The end record or the central directory is not one a runtime could have.
    #[error("The runtime zip's central directory is damaged: {detail}")]
    Rows {
        /// What is wrong with it.
        detail: Box<str>,
    },
    /// The end record counts more than [`MAX_PAYLOAD_ENTRIES`] entries.
    #[error("Release payload is too large: {count} entries")]
    TooManyEntries {
        /// The entries it counts.
        count: usize,
    },
    /// The entries declare more than [`MAX_PAYLOAD_BYTES`] in all.
    #[error("Release payload is too large: {bytes} bytes")]
    TooLarge {
        /// The bytes they declare.
        bytes: u64,
    },
    /// An entry is encrypted.
    #[error("Encrypted release entry: {name:?}")]
    Encrypted {
        /// The entry's name, as its row gives it.
        name: Box<str>,
    },
    /// An entry is compressed by a method other than deflate.
    #[error("Release entry {name:?} is compressed by method {method}, not stored or deflated")]
    Method {
        /// The entry's name, as its row gives it.
        name: Box<str>,
        /// Its method.
        method: u16,
    },
    /// An entry gives its sizes and checksum after its data, not in its headers.
    #[error("Release entry {name:?} gives its sizes after its data")]
    DataDescriptor {
        /// The entry's name, as its row gives it.
        name: Box<str>,
    },
    /// A local header disagrees with its row, or two entries overlap, as the zip crate finds.
    #[error("A local header disagrees with the central directory: {detail}")]
    LocalHeader {
        /// What the zip crate found.
        detail: Box<str>,
    },
    /// An entry's name breaks the payload's name rules.
    #[error("Invalid release path: {error}")]
    Name {
        /// The name and the rule it breaks.
        #[source]
        error: NameError,
    },
    /// An entry is a folder.
    #[error("Duplicate, directory or linked release entry: {name:?}")]
    Directory {
        /// The entry's name, as its row gives it.
        name: Box<str>,
    },
    /// An entry's mode is that of a link or of another file that is not a regular one.
    #[error("Duplicate, directory or linked release entry: {name:?}")]
    Link {
        /// The entry's name, as its row gives it.
        name: Box<str>,
    },
    /// Two entries have one name, in this or another ASCII letter case.
    #[error("Duplicate release entry: {name}")]
    Duplicate {
        /// The later of the two.
        name: Box<str>,
    },
    /// An entry's name is also the folder above another's.
    #[error("File/directory conflict in release payload: {name}")]
    Conflict {
        /// The name that would have to be both.
        name: Box<str>,
    },
    /// An entry lies under no root a runtime has, in its exact case.
    #[error("Unexpected release payload file: {name}")]
    UnknownRoot {
        /// The entry's name.
        name: Box<str>,
    },
    /// An entry lies under a name that a runtime has only as a file at its top.
    #[error("Unexpected release payload directory: {name}")]
    FileAsFolder {
        /// The entry's name.
        name: Box<str>,
    },
    /// An entry is a file of the name that a runtime has only as a folder at its top.
    #[error("Release payload folder held as a file: {name}")]
    FolderAsFile {
        /// The entry's name.
        name: Box<str>,
    },
    /// An entry lies where only a runtime for another platform keeps its files: under `lib` on
    /// Windows, or a library at the top on Linux.
    #[error("Release payload file of another platform's runtime: {name}")]
    PlatformRoot {
        /// The entry's name.
        name: Box<str>,
    },
    /// An entry under `data` is not JSON.
    #[error("Non-configuration file in release data: {name}")]
    DataNotJson {
        /// The entry's name.
        name: Box<str>,
    },
    /// A file every runtime has is missing.
    #[error("Incomplete release payload: no {name}")]
    Missing {
        /// The file's name.
        name: Box<str>,
    },
    /// `build-info.json` declares more than [`MAX_BUILD_INFO_LEN`].
    #[error("Release metadata is too large: {bytes} bytes")]
    BuildInfoTooLarge {
        /// The bytes it declares.
        bytes: u64,
    },
    /// `build-info.json` is not a JSON object, a field is not a text, or the inventory is not an
    /// object of at most [`MAX_PAYLOAD_ENTRIES`] files.
    #[error("Invalid release metadata: {detail}")]
    BuildInfo {
        /// What is wrong with it.
        detail: Box<str>,
    },
    /// The release record's version is not a semantic version.
    #[error("Invalid semantic release version: {text:?}")]
    Version {
        /// The version as the record gives it.
        text: Box<str>,
    },
    /// The release record names no platform a release is built for.
    #[error("Release metadata names no platform a release is built for: {text:?}")]
    Platform {
        /// The platform as the record gives it.
        text: Box<str>,
    },
    /// The release record names an executable that is not its platform's, or the runtime holds
    /// another platform's.
    #[error("Release executable {name:?} is not the release platform's")]
    Executable {
        /// The executable's name, as the record or the entry gives it.
        name: Box<str>,
    },
    /// A digest in the release record is not a SHA-256 digest in lower-case hexadecimal.
    #[error("Invalid release digest: {name}")]
    Digest {
        /// The name of the file it is given for.
        name: Box<str>,
    },
    /// The release record does not list exactly the archive's other entries.
    #[error("Release file inventory does not match its payload: {name}")]
    Inventory {
        /// A name that one of them has and the other does not.
        name: Box<str>,
    },
    /// `VERSION` declares more than [`MAX_VERSION_FILE_LEN`] bytes, or does not hold the release
    /// record's version.
    #[error("Release VERSION disagrees with its metadata")]
    VersionFile,
    /// An entry's bytes are not what the archive or the release record says they are.
    #[error("Damaged release payload: {name}: {detail}")]
    Damaged {
        /// The entry's name.
        name: Box<str>,
        /// What is wrong with its bytes.
        detail: Box<str>,
    },
    /// The runtime was built for another platform than the launcher.
    #[error(
        "This installer was built for a different operating system: the runtime is for {}",
        platform.as_str()
    )]
    OtherPlatform {
        /// The platform the runtime was built for.
        platform: Platform,
    },
    /// The runtime is of another version than the launcher that is to install it.
    #[error("The runtime's version {found:?} is not the launcher's version {expected:?}")]
    OtherVersion {
        /// The runtime's version, as its release record gives it.
        found: Box<str>,
        /// The launcher's version.
        expected: Box<str>,
    },
    /// The job was asked to stop before the runtime was read whole.
    #[error("cancelled")]
    Cancelled,
    /// The launcher file's sections were refused as the runtime was taken from them, as when the
    /// file carries no runtime zip. The refusal is reported as the sections' own: its rule, its
    /// text and its source.
    #[error(transparent)]
    Sections(#[from] SectionsError),
    /// The archive could not be read, or a copy of a file written.
    #[error("The runtime zip cannot be read or copied: {0}")]
    Io(#[from] io::Error),
}

impl PayloadError {
    /// The rule the runtime breaks, as a stable camelCase name for reports and logs.
    pub fn rule(&self) -> &'static str {
        match self {
            PayloadError::Archive => "archive",
            PayloadError::Zip64 => "zip64",
            PayloadError::Rows { .. } => "rows",
            PayloadError::TooManyEntries { .. } | PayloadError::TooLarge { .. } => "tooLarge",
            PayloadError::Encrypted { .. } => "encrypted",
            PayloadError::Method { .. } => "method",
            PayloadError::DataDescriptor { .. } => "dataDescriptor",
            PayloadError::LocalHeader { .. } => "localHeader",
            PayloadError::Name { .. } => "name",
            PayloadError::Directory { .. } => "directory",
            PayloadError::Link { .. } => "link",
            PayloadError::Duplicate { .. } => "duplicate",
            PayloadError::Conflict { .. } => "conflict",
            PayloadError::UnknownRoot { .. } => "unknownRoot",
            PayloadError::FileAsFolder { .. } => "fileRoot",
            PayloadError::FolderAsFile { .. } => "folderRoot",
            PayloadError::PlatformRoot { .. } => "platformRoot",
            PayloadError::DataNotJson { .. } => "dataNotJson",
            PayloadError::Missing { .. } => "incomplete",
            PayloadError::BuildInfoTooLarge { .. } => "buildInfoTooLarge",
            PayloadError::BuildInfo { .. } => "buildInfo",
            PayloadError::Version { .. } => "version",
            PayloadError::Platform { .. } => "platform",
            PayloadError::Executable { .. } => "executable",
            PayloadError::Digest { .. } => "digest",
            PayloadError::Inventory { .. } => "inventory",
            PayloadError::VersionFile => "versionFile",
            PayloadError::Damaged { .. } => "damaged",
            PayloadError::OtherPlatform { .. } => "otherPlatform",
            PayloadError::OtherVersion { .. } => "otherVersion",
            PayloadError::Cancelled => "cancelled",
            PayloadError::Sections(error) => error.rule(),
            PayloadError::Io(_) => "io",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::error::Error;
    use std::io::{self, Cursor, Read, Seek, SeekFrom, Write};
    use std::panic;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};

    use gdl_files::{
        BLOCK_LEN, Cancel, Job, NameError, NameRule, NoProgress, PAYLOAD_NAMES, PhaseId, Progress,
        RelPath,
    };
    use gdl_fixtures::{RawEntry, RawZip, RuntimeFiles};
    use gdl_release::Platform;
    use serde_json::{Map, Value, json};
    use sha2::{Digest, Sha256};
    use zip::ZipArchive;

    use super::{
        BUILD_INFO, DEFAULT_CONFIG, MAX_BUILD_INFO_LEN, MAX_PAYLOAD_BYTES, MAX_PAYLOAD_ENTRIES,
        MAX_VERSION_FILE_LEN, Payload, PayloadEntry, PayloadError, VERSION_FILE, check_with_zip,
    };
    use crate::SectionsError;

    /// The platforms a release is built for, as the release scripts name them.
    pub(super) const WINDOWS: &str = "windows-x64";
    pub(super) const LINUX: &str = "linux-x64";

    /// The version the test runtimes carry.
    const VERSION: &str = "1.0.0-beta";

    /// The standard runtime's shader, whose declared size the tests change.
    pub(super) const SHADER: &str = "shaders/test.spv";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The runtime of the installer's own tests, for `platform`.
    pub(super) fn standard(platform: &str) -> RuntimeFiles {
        RuntimeFiles::standard(platform, VERSION)
    }

    /// Opens a runtime zip held in memory.
    pub(super) fn open(zip: Vec<u8>) -> Result<Payload<Cursor<Vec<u8>>>, PayloadError> {
        Payload::open(Cursor::new(zip))
    }

    /// A job nobody asks to stop and nobody follows.
    pub(super) fn quiet(cancel: &Cancel) -> Job<'_> {
        Job {
            cancel,
            progress: &NoProgress,
        }
    }

    /// Opens a runtime zip held in memory, then reads every file its release record lists.
    pub(super) fn open_and_verify(zip: Vec<u8>) -> Result<Payload<Cursor<Vec<u8>>>, PayloadError> {
        let mut payload = open(zip)?;
        payload.verify(&quiet(&Cancel::default()))?;
        Ok(payload)
    }

    /// The SHA-256 digest of `data`, as a release record writes it.
    pub(super) fn hex_sha256(data: &[u8]) -> String {
        Sha256::digest(data)
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect()
    }

    /// The inventory `runtime`'s release record lists.
    pub(super) fn listing(runtime: &RuntimeFiles) -> Map<String, Value> {
        runtime.build_info()["files"]
            .as_object()
            .expect("a release record lists its files")
            .clone()
    }

    /// The standard Linux runtime, changed before its archive is written, with the inventory that
    /// `change` leaves unless it gives the record an inventory of its own.
    fn listed(change: impl FnOnce(&mut RuntimeFiles, &mut Map<String, Value>)) -> Vec<u8> {
        let mut runtime = standard(LINUX);
        let mut files = listing(&runtime);
        change(&mut runtime, &mut files);
        runtime.extra.entry("files").or_insert(Value::Object(files));
        runtime.zip()
    }

    /// Where the entry named `name` lies among the payload's entries.
    fn place(payload: &Payload<impl Read + Seek>, name: &str) -> usize {
        payload
            .entries()
            .iter()
            .position(|entry| entry.name.as_str() == name)
            .unwrap_or_else(|| panic!("no entry {name}"))
    }

    /// The entry of `raw` named `name`.
    pub(super) fn entry<'a>(raw: &'a mut RawZip, name: &str) -> &'a mut RawEntry {
        raw.entries
            .iter_mut()
            .find(|entry| entry.name == name.as_bytes())
            .unwrap_or_else(|| panic!("no entry {name}"))
    }

    /// The standard runtime's archive for `platform`, its shader declaring the size that brings
    /// what every entry declares to `total`.
    pub(super) fn declaring_in_all(platform: &str, total: u64) -> Vec<u8> {
        let mut raw = standard(platform).raw();
        let others: u64 = raw
            .entries
            .iter()
            .filter(|entry| entry.name != SHADER.as_bytes())
            .map(|entry| u64::from(entry.declared_size.expect("deflated entries declare")))
            .sum();
        entry(&mut raw, SHADER).declared_size = Some(u32::try_from(total - others).unwrap());
        raw.to_bytes()
    }

    /// The standard runtime's archive for `platform`, whose `build-info.json` takes exactly `len`
    /// bytes, padded by a key the record does not know, and is held as it is.
    pub(super) fn build_info_of_len(platform: &str, len: u64) -> Vec<u8> {
        const PADDING: &str = "padding";
        let mut runtime = standard(platform);
        runtime
            .extra
            .insert(PADDING.to_owned(), String::new().into());
        let unpadded = runtime.build_info_bytes().len() as u64;
        let padding = "x".repeat(usize::try_from(len - unpadded).unwrap());
        runtime.extra.insert(PADDING.to_owned(), padding.into());
        let build_info = runtime.build_info_bytes();
        assert_eq!(build_info.len() as u64, len);
        // The record's entry replaced, so that its megabytes are not deflated.
        let mut raw = standard(platform).raw();
        *entry(&mut raw, BUILD_INFO) = RawEntry::stored(BUILD_INFO, &build_info);
        raw.to_bytes()
    }

    /// The runtime's archive with every entry stored as it is, which builds fast when there are
    /// many.
    fn stored(runtime: &RuntimeFiles) -> Vec<u8> {
        let mut files: Vec<&(String, Vec<u8>)> = runtime.files.iter().collect();
        files.sort();
        let mut entries: Vec<RawEntry> = files
            .into_iter()
            .map(|(name, data)| RawEntry::stored(name, data))
            .collect();
        entries.push(RawEntry::stored(BUILD_INFO, &runtime.build_info_bytes()));
        RawZip {
            entries,
            ..RawZip::default()
        }
        .to_bytes()
    }

    /// Each file of `runtime` and its size, in the order release.py writes them: by name, then
    /// `build-info.json`.
    fn written_order(runtime: &RuntimeFiles) -> Vec<(String, u64)> {
        let mut files: Vec<(String, u64)> = runtime
            .files
            .iter()
            .map(|(name, data)| (name.clone(), data.len() as u64))
            .collect();
        files.sort();
        files.push((
            BUILD_INFO.to_owned(),
            runtime.build_info_bytes().len() as u64,
        ));
        files
    }

    fn rel(text: &str) -> RelPath {
        RelPath::parse(text, &PAYLOAD_NAMES).unwrap()
    }

    #[test]
    fn the_standard_runtime_opens_with_its_entries_in_order() {
        for platform in [WINDOWS, LINUX] {
            let runtime = standard(platform);
            let zip = runtime.zip();
            let payload = open(zip.clone()).unwrap_or_else(|error| panic!("{platform}: {error}"));
            let found: Vec<(String, u64)> = payload
                .entries()
                .iter()
                .map(|entry| (entry.name.as_str().to_owned(), entry.size))
                .collect();
            assert_eq!(found, written_order(&runtime), "{platform}");
            // Each entry's index is where the zip crate keeps it.
            let archive = ZipArchive::new(Cursor::new(zip)).unwrap();
            for (place, entry) in payload.entries().iter().enumerate() {
                assert_eq!(entry.index, place, "{platform}");
                let name = archive.name_for_index(entry.index).unwrap().unwrap();
                assert_eq!(name, entry.name.as_str(), "{platform}");
            }
        }
        // Entries held as they are keep to the rules as well as deflated ones.
        let runtime = standard(LINUX);
        let payload = open(stored(&runtime)).unwrap();
        assert_eq!(payload.entries().len(), written_order(&runtime).len());
    }

    #[test]
    fn declared_size_is_the_sum_of_entry_sizes() {
        let runtime = standard(LINUX);
        let sum: u64 = written_order(&runtime).iter().map(|(_, size)| size).sum();
        let payload = open(runtime.zip()).unwrap();
        assert_eq!(payload.declared_size(), sum);
        let sizes: u64 = payload.entries().iter().map(|entry| entry.size).sum();
        assert_eq!(payload.declared_size(), sizes);

        // The sizes are those the rows declare, not the bytes the archive holds.
        let declared = 1 << 30;
        let mut raw = runtime.raw();
        entry(&mut raw, SHADER).declared_size = Some(declared);
        let payload = open(raw.to_bytes()).unwrap();
        let shader_len = b"shader".len() as u64;
        assert_eq!(
            payload.declared_size(),
            sum - shader_len + u64::from(declared)
        );
        let shader = payload
            .entries()
            .iter()
            .find(|entry| entry.name.as_str() == SHADER)
            .unwrap();
        assert_eq!(shader.size, u64::from(declared));
    }

    #[test]
    fn the_entry_limit_is_inclusive() {
        // The release scripts' limit.
        assert_eq!(MAX_PAYLOAD_ENTRIES, 20_000);
        // The standard runtime, then empty shaders up to the limit, each in the inventory, so
        // that nothing but the count tells the two archives apart.
        let mut runtime = standard(LINUX);
        let shaders = MAX_PAYLOAD_ENTRIES - runtime.files.len() - 1;
        runtime.files.extend(
            (0..shaders).map(|index| (format!("shaders/{index:05}.spv"), Vec::<u8>::new())),
        );
        let payload = open(stored(&runtime)).unwrap();
        assert_eq!(payload.entries().len(), MAX_PAYLOAD_ENTRIES);

        runtime
            .files
            .push(("shaders/one-more.spv".to_owned(), Vec::new()));
        match open(stored(&runtime)) {
            Err(PayloadError::TooManyEntries { count }) => {
                assert_eq!(count, MAX_PAYLOAD_ENTRIES + 1);
            }
            other => panic!("{:?}", other.map(|payload| payload.entries().len())),
        }
    }

    #[test]
    fn the_size_limits_are_inclusive() {
        // The release scripts' limits: 2 GiB in all, 4 MiB of release record, and the 128 bytes
        // of VERSION that the updater reads.
        assert_eq!(MAX_PAYLOAD_BYTES, 2 * 1024 * 1024 * 1024);
        assert_eq!(MAX_BUILD_INFO_LEN, 4 * 1024 * 1024);
        assert_eq!(MAX_VERSION_FILE_LEN, 128);
        let payload = open(declaring_in_all(LINUX, MAX_PAYLOAD_BYTES)).unwrap();
        assert_eq!(payload.declared_size(), MAX_PAYLOAD_BYTES);
        match open(declaring_in_all(LINUX, MAX_PAYLOAD_BYTES + 1)) {
            Err(PayloadError::TooLarge { bytes }) => assert_eq!(bytes, MAX_PAYLOAD_BYTES + 1),
            other => panic!("{:?}", other.map(|payload| payload.declared_size())),
        }

        let payload = open(build_info_of_len(LINUX, MAX_BUILD_INFO_LEN)).unwrap();
        let build_info = payload
            .entries()
            .iter()
            .find(|entry| entry.name.as_str() == BUILD_INFO)
            .unwrap();
        assert_eq!(build_info.size, MAX_BUILD_INFO_LEN);
        match open(build_info_of_len(LINUX, MAX_BUILD_INFO_LEN + 1)) {
            Err(PayloadError::BuildInfoTooLarge { bytes }) => {
                assert_eq!(bytes, MAX_BUILD_INFO_LEN + 1);
            }
            other => panic!("{:?}", other.map(|payload| payload.declared_size())),
        }
    }

    #[test]
    fn the_first_rule_broken_is_reported() {
        let changed = |change: &dyn Fn(&mut RawZip)| {
            let mut raw = standard(LINUX).raw();
            change(&mut raw);
            raw.to_bytes()
        };
        let disagreeing = |raw: &mut RawZip| {
            entry(raw, "data/config.json").local_name = Some(b"data/cOnfig.json".to_vec());
        };
        // Archives that break two rules, each refused for the one checked first.
        for (what, zip, rule) in [
            (
                "an encrypted entry of another method",
                changed(&|raw| {
                    entry(raw, SHADER).flags |= 0x0001;
                    entry(raw, SHADER).method = 12;
                }),
                "encrypted",
            ),
            (
                "an entry of another method and an unsafe name",
                changed(&|raw| {
                    entry(raw, SHADER).method = 12;
                    entry(raw, SHADER).name = b"shaders/CON".to_vec();
                }),
                "method",
            ),
            (
                "an unsafe name in a row before one of another method",
                changed(&|raw| {
                    entry(raw, "data/config.json").name = b"data/CON".to_vec();
                    entry(raw, SHADER).method = 12;
                }),
                "name",
            ),
            (
                "a folder's DOS bit on a link's mode",
                changed(&|raw| {
                    entry(raw, SHADER).external_attributes = (0o120777 << 16) | 0x10;
                }),
                "directory",
            ),
            (
                "a duplicate in a row before one under an unknown root",
                changed(&|raw| {
                    entry(raw, "data/text/en.json").name = b"data/Config.json".to_vec();
                    entry(raw, SHADER).name = b"saves/x.json".to_vec();
                }),
                "unknownRoot",
            ),
            (
                "a duplicate beside a file that is the folder above another",
                changed(&|raw| {
                    raw.entries
                        .insert(0, RawEntry::deflated("shaders/x/y.spv", b"shader"));
                    entry(raw, SHADER).name = b"shaders/x".to_vec();
                    entry(raw, "data/text/en.json").name = b"data/Config.json".to_vec();
                }),
                "duplicate",
            ),
            (
                "declared sizes over the limit without build-info.json",
                changed(&|raw| {
                    entry(raw, SHADER).declared_size = Some(u32::MAX);
                    raw.entries.pop();
                }),
                "tooLarge",
            ),
            (
                "no build-info.json and a local header that disagrees",
                changed(&|raw| {
                    disagreeing(raw);
                    raw.entries.pop();
                }),
                "incomplete",
            ),
            (
                "a build-info.json over its limit and a local header that disagrees",
                changed(&|raw| {
                    disagreeing(raw);
                    let limit = u32::try_from(MAX_BUILD_INFO_LEN).unwrap();
                    entry(raw, BUILD_INFO).declared_size = Some(limit + 1);
                }),
                "buildInfoTooLarge",
            ),
        ] {
            match open(zip) {
                Err(error) => assert_eq!(error.rule(), rule, "{what}: {error}"),
                Ok(_) => panic!("{what}: accepted"),
            }
        }

        // The same for the release record, and for what the archive holds beside it.
        let digest = || Value::String(hex_sha256(b"crafted"));
        let upper = |files: &mut Map<String, Value>| {
            let digest = hex_sha256(b"{}").to_uppercase();
            files.insert(DEFAULT_CONFIG.to_owned(), Value::String(digest));
        };
        let with_entry = |name: &str| {
            changed(&|raw| {
                let record = raw.entries.len() - 1;
                raw.entries
                    .insert(record, RawEntry::deflated(name, b"crafted"));
            })
        };
        for (what, zip, rule) in [
            (
                "a local header that disagrees and a release record that is not JSON",
                changed(&|raw| {
                    disagreeing(raw);
                    *entry(raw, BUILD_INFO) = RawEntry::deflated(BUILD_INFO, b"not JSON\n");
                }),
                "localHeader",
            ),
            (
                "a commit that is no text and the version of a tag",
                listed(|runtime, _| {
                    runtime.extra.insert("commit".to_owned(), json!(5));
                    runtime.version = "v1.0.0-beta".to_owned();
                }),
                "buildInfo",
            ),
            (
                "the version of a tag and a platform no release is built for",
                listed(|runtime, _| {
                    runtime.version = "v1.0.0-beta".to_owned();
                    runtime.platform = "another-os".to_owned();
                }),
                "version",
            ),
            (
                "a platform no release is built for and an executable that is no platform's",
                listed(|runtime, _| {
                    runtime.platform = "another-os".to_owned();
                    runtime.executable = "game".to_owned();
                }),
                "platform",
            ),
            (
                "Windows' executable named on Linux and an inventory that is no object",
                listed(|runtime, _| {
                    runtime.executable = "gauntlet.exe".to_owned();
                    runtime.extra.insert("files".to_owned(), json!([]));
                }),
                "executable",
            ),
            (
                "an unsafe name in the inventory and VERSION missing from it",
                listed(|_, files| {
                    files.insert("data/CON.json".to_owned(), digest());
                    files.remove(VERSION_FILE);
                }),
                "name",
            ),
            (
                "a name above another in the inventory and VERSION missing from it",
                listed(|_, files| {
                    files.insert(format!("{SHADER}/x"), digest());
                    files.remove(VERSION_FILE);
                }),
                "conflict",
            ),
            (
                "VERSION missing from the inventory and a digest in upper case",
                listed(|_, files| {
                    files.remove(VERSION_FILE);
                    upper(files);
                }),
                "incomplete",
            ),
            (
                "a digest in upper case and an inventory that leaves a file out",
                listed(|_, files| {
                    upper(files);
                    files.remove("data/text/en.json");
                }),
                "digest",
            ),
            (
                "a commit that is no text and a library only Windows runtimes have",
                listed(|runtime, files| {
                    runtime.extra.insert("commit".to_owned(), json!(5));
                    runtime
                        .files
                        .push(("SDL3.dll".to_owned(), b"crafted".to_vec()));
                    files.insert("SDL3.dll".to_owned(), digest());
                }),
                "buildInfo",
            ),
            (
                "Windows' executable in the archive and not in the inventory",
                with_entry("gauntlet.exe"),
                "executable",
            ),
            (
                "a library only Windows runtimes have in the archive and not in the inventory",
                with_entry("SDL3.dll"),
                "platformRoot",
            ),
            (
                "an inventory that leaves a file out and a VERSION that disagrees",
                listed(|runtime, files| {
                    files.remove("data/text/en.json");
                    let version = runtime
                        .files
                        .iter_mut()
                        .find(|(name, _)| name == VERSION_FILE)
                        .unwrap();
                    version.1 = b"1.0.1\n".to_vec();
                }),
                "inventory",
            ),
        ] {
            match open(zip) {
                Err(error) => assert_eq!(error.rule(), rule, "{what}: {error}"),
                Ok(_) => panic!("{what}: accepted"),
            }
        }
    }

    /// A reader of an archive that fails every read starting before `from`, as a disk does that
    /// cannot read part of a file.
    struct Unreadable {
        archive: Cursor<Vec<u8>>,
        from: u64,
    }

    impl Read for Unreadable {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            if self.archive.position() < self.from {
                return Err(io::Error::other("unreadable sector"));
            }
            self.archive.read(buffer)
        }
    }

    impl Seek for Unreadable {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.archive.seek(to)
        }
    }

    #[test]
    fn a_reader_that_fails_is_an_io_error() {
        let zip = standard(LINUX).zip();
        // Failing at the end record, and failing in the central directory once the end record
        // and the place of a ZIP64 locator have been read.
        let tail = 42;
        for from in [u64::MAX, zip.len() as u64 - tail] {
            let reader = Unreadable {
                archive: Cursor::new(zip.clone()),
                from,
            };
            match Payload::open(reader) {
                Err(error @ PayloadError::Io(_)) => {
                    assert_eq!(error.rule(), "io");
                    assert_eq!(error.source().unwrap().to_string(), "unreadable sector");
                }
                other => panic!("{:?}", other.map(|payload| payload.entries().len())),
            }
        }
        // Reading only from there on fails nothing.
        let reader = Unreadable {
            archive: Cursor::new(zip),
            from: 0,
        };
        Payload::open(reader).unwrap();
    }

    #[test]
    fn the_zip_crate_must_read_the_rows_entries() {
        let zip = standard(LINUX).zip();
        let entries = open(zip.clone()).unwrap().entries().to_vec();
        check_with_zip(Cursor::new(zip.clone()), &entries).unwrap();

        // An entry the crate does not read, one too few, another name or size in an entry's
        // place, and two entries in each other's.
        let mut more = entries.clone();
        more.push(PayloadEntry {
            name: rel("data/more.json"),
            size: 2,
            index: entries.len(),
        });
        let mut fewer = entries.clone();
        fewer.pop();
        let mut renamed = entries.clone();
        renamed[1].name = rel("data/other.json");
        let mut resized = entries.clone();
        resized[1].size += 1;
        let mut reordered = entries.clone();
        reordered.swap(0, 1);
        for (what, claimed) in [
            ("more", more),
            ("fewer", fewer),
            ("renamed", renamed),
            ("resized", resized),
            ("reordered", reordered),
        ] {
            let error = check_with_zip(Cursor::new(zip.clone()), &claimed).unwrap_err();
            assert_eq!(error.rule(), "rows", "{what}: {error}");
        }
        // An archive the crate refuses.
        let error = check_with_zip(Cursor::new(b"not a zip".to_vec()), &[]).unwrap_err();
        assert_eq!(error.rule(), "rows", "{error}");
    }

    #[test]
    fn accepts_the_fixture_payload_on_each_platform() {
        let this_build = Platform::of_this_build();
        for platform in Platform::ALL {
            let runtime = standard(platform.as_str());
            let payload = open_and_verify(runtime.zip())
                .unwrap_or_else(|error| panic!("{platform:?}: {error}"));
            let info = payload.build_info();
            assert_eq!(info.version, VERSION);
            assert_eq!(info.commit, "test-commit");
            assert_eq!(info.platform, platform);
            assert_eq!(info.executable, platform.executable());
            let digests: BTreeMap<String, [u8; 32]> = runtime
                .files
                .iter()
                .map(|(name, data)| (name.clone(), Sha256::digest(data).into()))
                .collect();
            assert_eq!(info.files, digests, "{platform:?}");
            assert!(info.other.is_empty(), "{platform:?}");

            // A launcher takes only the runtime of its own platform and its exact version.
            for launcher in [
                VERSION,
                "1.0.0",
                "1.0.0-beta+build.1",
                "v1.0.0-beta",
                "1.0.0-beta\n",
            ] {
                let expected = if this_build != Some(platform) {
                    Err("otherPlatform")
                } else if launcher == VERSION {
                    Ok(())
                } else {
                    Err("otherVersion")
                };
                let outcome = payload.for_launcher(launcher).map_err(|error| error.rule());
                assert_eq!(outcome, expected, "{platform:?} for {launcher:?}");
            }
        }
        // The two CI systems are both ones a release is built for.
        let Some(this_build) = this_build else {
            eprintln!("skipped: no release is built for this system");
            return;
        };
        let payload = open(standard(this_build.as_str()).zip()).unwrap();
        match payload.for_launcher("1.0.0-beta+build.1") {
            Err(PayloadError::OtherVersion { found, expected }) => {
                assert_eq!((&*found, &*expected), (VERSION, "1.0.0-beta+build.1"));
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn refuses_disc_assets_and_traversal() {
        for (path, refused) in [
            ("Gauntlet/PDATA/WAR.WAD", None),
            ("carddemo/icon.tpl", None),
            ("../escaped", Some(NameRule::Dot)),
            ("data/../escaped", Some(NameRule::Dot)),
        ] {
            // A zip of that one file, as zipfile's writestr makes it.
            let zip = RawZip {
                entries: vec![RawEntry::stored(path, b"not allowed")],
                ..RawZip::default()
            };
            match (open(zip.to_bytes()), refused) {
                (Err(PayloadError::UnknownRoot { name }), None) => assert_eq!(&*name, path),
                (Err(PayloadError::Name { error }), Some(rule)) => {
                    assert_eq!((&*error.name, error.rule), (path, rule));
                }
                (other, _) => panic!("{path}: {:?}", other.map(|payload| payload.entries().len())),
            }
        }
    }

    #[test]
    fn a_damaged_entry_fails_verification() {
        // The runtime written again with every file held as it is, as zipfile writes it, and
        // data/config.json's bytes changed under the release record that lists the old ones.
        let runtime = standard(LINUX);
        let mut files = runtime.files.clone();
        files.sort();
        let mut entries: Vec<RawEntry> = files
            .iter()
            .map(|(name, data)| match name.as_str() {
                DEFAULT_CONFIG => RawEntry::stored(name, b"damaged"),
                _ => RawEntry::stored(name, data),
            })
            .collect();
        entries.push(RawEntry::stored(BUILD_INFO, &runtime.build_info_bytes()));
        let zip = RawZip {
            entries,
            ..RawZip::default()
        };
        let mut payload = open(zip.to_bytes()).unwrap();
        match payload.verify(&quiet(&Cancel::default())) {
            Err(error @ PayloadError::Damaged { .. }) => {
                assert_eq!(error.rule(), "damaged");
                assert_eq!(
                    error.to_string(),
                    "Damaged release payload: data/config.json: its SHA-256 digest is not the \
                     one its release record gives it"
                );
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn refuses_another_platform() {
        // A platform no release is built for, refused as the runtime is opened.
        match open(RuntimeFiles::standard("another-os", VERSION).zip()) {
            Err(error @ PayloadError::Platform { .. }) => {
                assert_eq!(error.rule(), "platform");
                assert_eq!(
                    error.to_string(),
                    r#"Release metadata names no platform a release is built for: "another-os""#
                );
            }
            other => panic!("{:?}", other.map(|payload| payload.entries().len())),
        }
        // A platform a release is built for, but not this build's, refused as it is taken.
        let Some(this_build) = Platform::of_this_build() else {
            eprintln!("skipped: no release is built for this system");
            return;
        };
        let elsewhere = Platform::ALL
            .into_iter()
            .find(|platform| *platform != this_build)
            .unwrap();
        let payload = open_and_verify(standard(elsewhere.as_str()).zip()).unwrap();
        match payload.for_launcher(VERSION) {
            Err(PayloadError::OtherPlatform { platform }) => assert_eq!(platform, elsewhere),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn refuses_an_inventory_that_claims_saves() {
        let saves = "saves/hero.json";
        // Listed in the release record alone, and held in the archive too.
        let listed_alone = listed(|_, files| {
            files.insert(saves.to_owned(), Value::String(hex_sha256(b"user-owned")));
        });
        let mut runtime = standard(LINUX);
        runtime
            .files
            .push((saves.to_owned(), b"user-owned".to_vec()));
        for zip in [listed_alone, runtime.zip()] {
            match open(zip) {
                Err(PayloadError::UnknownRoot { name }) => assert_eq!(&*name, saves),
                other => panic!("{:?}", other.map(|payload| payload.entries().len())),
            }
        }
    }

    /// A destination with room for `room` bytes, which then fails as a full disk does.
    struct Full {
        room: usize,
        written: Vec<u8>,
    }

    impl Write for Full {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
            if self.room == 0 {
                return Err(io::Error::new(io::ErrorKind::StorageFull, "no room left"));
            }
            let taken = bytes.len().min(self.room);
            self.room -= taken;
            self.written.extend_from_slice(&bytes[..taken]);
            Ok(taken)
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    /// A reader of an archive that fails every read once it is told to, as a disk does that stops
    /// answering after the archive was opened.
    struct Failing {
        archive: Cursor<Vec<u8>>,
        failing: Arc<AtomicBool>,
    }

    impl Read for Failing {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            if self.failing.load(Ordering::Relaxed) {
                return Err(io::Error::other("unreadable sector"));
            }
            self.archive.read(buffer)
        }
    }

    impl Seek for Failing {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.archive.seek(to)
        }
    }

    #[test]
    fn copy_entry_copies_each_file_as_the_runtime_holds_it() {
        let runtime = standard(WINDOWS);
        let record = runtime.build_info_bytes();
        let mut payload = open(runtime.zip()).unwrap();
        let cancel = Cancel::default();
        for index in 0..payload.entries().len() {
            let name = payload.entries()[index].name.as_str().to_owned();
            let expected = runtime
                .files
                .iter()
                .find(|(file, _)| *file == name)
                .map_or(&record[..], |(_, data)| data);
            let progress = Recorded::default();
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let mut copy = Vec::new();
            let copied = payload.copy_entry(index, &mut copy, &job).unwrap();
            assert_eq!(copy, expected, "{name}");
            assert_eq!(copied, expected.len() as u64, "{name}");
            // Its progress is told the entry's own bytes, done of declared.
            let size = expected.len() as u64;
            let steps: Vec<_> = (size > 0)
                .then(|| (size, size, name.clone()))
                .into_iter()
                .collect();
            assert_eq!(progress.steps(), steps, "{name}");
        }

        // A copy of build-info.json must be the record that was read and checked when the
        // runtime was opened.
        let at = place(&payload, BUILD_INFO);
        payload.build_info_sha256[0] ^= 1;
        match payload.copy_entry(at, &mut io::sink(), &quiet(&cancel)) {
            Err(PayloadError::Damaged { name, .. }) => assert_eq!(&*name, BUILD_INFO),
            other => panic!("{other:?}"),
        }
        payload.build_info_sha256[0] ^= 1;
        let copied = payload.copy_entry(at, &mut io::sink(), &quiet(&cancel));
        assert_eq!(copied.unwrap(), record.len() as u64);

        // A copy that cannot be written is an I/O error, not damage, and stops where it failed.
        let at = place(&payload, "gauntlet.exe");
        let mut full = Full {
            room: 5,
            written: Vec::new(),
        };
        match payload.copy_entry(at, &mut full, &quiet(&cancel)) {
            Err(error @ PayloadError::Io(_)) => {
                assert_eq!(error.rule(), "io");
                assert_eq!(error.source().unwrap().to_string(), "no room left");
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(full.written, b"not a");

        // What the zip crate cannot read of an entry is damage, whatever the cause.
        let failing = Arc::new(AtomicBool::new(false));
        let reader = Failing {
            archive: Cursor::new(runtime.zip()),
            failing: Arc::clone(&failing),
        };
        let mut payload = Payload::open(reader).unwrap();
        failing.store(true, Ordering::Relaxed);
        let at = place(&payload, DEFAULT_CONFIG);
        match payload.copy_entry(at, &mut Vec::new(), &quiet(&cancel)) {
            Err(PayloadError::Damaged { name, detail }) => {
                assert_eq!(&*name, DEFAULT_CONFIG);
                assert!(detail.contains("unreadable sector"), "{detail}");
            }
            Err(other) => panic!("{other}"),
            Ok(copied) => panic!("{copied} bytes copied"),
        }
    }

    /// A reader of an archive whose next read, once it is told to, is interrupted, as a read by a
    /// thread that takes a signal is.
    struct Interrupting {
        archive: Cursor<Vec<u8>>,
        interrupt: Arc<AtomicBool>,
    }

    impl Read for Interrupting {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            if self.interrupt.swap(false, Ordering::Relaxed) {
                return Err(io::Error::from(io::ErrorKind::Interrupted));
            }
            self.archive.read(buffer)
        }
    }

    impl Seek for Interrupting {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.archive.seek(to)
        }
    }

    #[test]
    fn an_interrupted_read_is_made_again() {
        let runtime = standard(LINUX);
        let interrupt = Arc::new(AtomicBool::new(false));
        let reader = Interrupting {
            archive: Cursor::new(runtime.zip()),
            interrupt: Arc::clone(&interrupt),
        };
        let mut payload = Payload::open(reader).unwrap();
        let cancel = Cancel::default();
        for (name, data) in &runtime.files {
            interrupt.store(true, Ordering::Relaxed);
            let mut copy = Vec::new();
            let at = place(&payload, name);
            let copied = payload.copy_entry(at, &mut copy, &quiet(&cancel));
            assert_eq!(
                copied.unwrap_or_else(|error| panic!("{name}: {error}")),
                data.len() as u64
            );
            assert_eq!(copy, *data, "{name}");
            // Every copy met its interruption.
            assert!(!interrupt.load(Ordering::Relaxed), "{name}");
        }
    }

    /// Progress that writes down each step it is told, and asks its job to stop at the first when
    /// it holds the job's cancel.
    #[derive(Default)]
    struct Recorded {
        /// Each step's bytes done, total and item, in order.
        steps: Mutex<Vec<(u64, u64, String)>>,
        /// The cancel of the job to stop.
        stopping: Option<Cancel>,
    }

    impl Recorded {
        fn stopping(cancel: &Cancel) -> Self {
            Recorded {
                stopping: Some(cancel.clone()),
                ..Recorded::default()
            }
        }

        fn steps(&self) -> Vec<(u64, u64, String)> {
            self.steps.lock().unwrap().clone()
        }
    }

    impl Progress for Recorded {
        fn phase(&self, _: PhaseId) {}

        fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
            let item = item.map_or_else(String::new, |item| item.as_str().to_owned());
            self.steps.lock().unwrap().push((done, total, item));
            if let Some(cancel) = &self.stopping {
                cancel.cancel();
            }
        }
    }

    /// The shader of three blocks that [`with_large_shader`] adds.
    const LARGE: &str = "shaders/large.spv";

    /// The standard Linux runtime with a shader of three blocks first in its archive.
    fn with_large_shader() -> Vec<u8> {
        let mut runtime = standard(LINUX);
        let large = (0..3 * BLOCK_LEN)
            .map(|index| (index % 251) as u8)
            .collect();
        runtime.files.push((LARGE.to_owned(), large));
        let mut raw = runtime.raw();
        let at = raw
            .entries
            .iter()
            .position(|entry| entry.name == LARGE.as_bytes())
            .unwrap();
        let large = raw.entries.remove(at);
        raw.entries.insert(0, large);
        raw.to_bytes()
    }

    /// The bytes the files of `payload`'s inventory declare in all.
    fn listed_size(payload: &Payload<impl Read + Seek>) -> u64 {
        payload
            .entries()
            .iter()
            .filter(|entry| entry.name.as_str() != BUILD_INFO)
            .map(|entry| entry.size)
            .sum()
    }

    #[test]
    fn verify_stops_when_cancelled() {
        let mut payload = open(with_large_shader()).unwrap();
        let total = listed_size(&payload);
        let block = BLOCK_LEN as u64;
        let cancel = Cancel::default();
        let progress = Recorded::stopping(&cancel);
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        match payload.verify(&job) {
            Err(error @ PayloadError::Cancelled) => {
                assert_eq!(
                    (error.rule(), error.to_string()),
                    ("cancelled", "cancelled".into())
                );
            }
            other => panic!("{other:?}"),
        }
        // Asked to stop as it told its first block, it read no other.
        assert_eq!(progress.steps(), [(block, total, LARGE.to_owned())]);

        // A job asked to stop copies nothing.
        let mut copy = Vec::new();
        match payload.copy_entry(place(&payload, VERSION_FILE), &mut copy, &job) {
            Err(PayloadError::Cancelled) => {}
            other => panic!("{other:?}"),
        }
        assert!(copy.is_empty());
        assert_eq!(progress.steps().len(), 1);

        // The runtime is as it was: a job nobody stops reads it whole.
        payload.verify(&quiet(&Cancel::default())).unwrap();
    }

    #[test]
    fn verify_advances_in_blocks() {
        let mut payload = open(with_large_shader()).unwrap();
        let total = listed_size(&payload);
        let block = BLOCK_LEN as u64;
        let cancel = Cancel::default();
        let progress = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        payload.verify(&job).unwrap();
        let steps = progress.steps();
        // The large shader goes a whole block at a time, though the decompressor gives less.
        let large = |done| (done, total, LARGE.to_owned());
        assert_eq!(
            steps[..3],
            [large(block), large(2 * block), large(3 * block)]
        );
        // Every step moves on by at most a block, out of the bytes of every listed file.
        let mut done = 0;
        for (now, of, item) in &steps {
            assert_eq!(*of, total, "{item}");
            assert!(
                *now > done && *now - done <= block,
                "{item}: {done} to {now}"
            );
            done = *now;
        }
        assert_eq!(done, total);
        // Every listed file that holds bytes is told, in the archive's order, and only those.
        let mut told: Vec<&str> = steps.iter().map(|(_, _, item)| item.as_str()).collect();
        told.dedup();
        let holding: Vec<&str> = payload
            .entries()
            .iter()
            .filter(|entry| entry.name.as_str() != BUILD_INFO && entry.size > 0)
            .map(|entry| entry.name.as_str())
            .collect();
        assert_eq!(told, holding);

        // Copied alone, the large shader tells its own bytes, a whole block at a time.
        let alone = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &alone,
        };
        let at = place(&payload, LARGE);
        let copied = payload.copy_entry(at, &mut io::sink(), &job).unwrap();
        assert_eq!(copied, 3 * block);
        let own = |done| (done, 3 * block, LARGE.to_owned());
        assert_eq!(alone.steps(), [own(block), own(2 * block), own(3 * block)]);
    }

    #[test]
    fn a_copy_writes_at_most_a_byte_past_the_declared_size() {
        // The large shader of three blocks declared, after the zip crate read its row, as a block
        // and a few bytes: a row the crate reads for more than the limits were held to.
        let mut payload = open(with_large_shader()).unwrap();
        let at = place(&payload, LARGE);
        let declared = BLOCK_LEN as u64 + 5;
        payload.entries[at].size = declared;
        let mut copy = Vec::new();
        match payload.copy_entry(at, &mut copy, &quiet(&Cancel::default())) {
            Err(PayloadError::Damaged { name, detail }) => {
                assert_eq!(&*name, LARGE);
                let held = declared + 1;
                assert_eq!(
                    &*detail,
                    format!("it holds {held} bytes and declares {declared}")
                );
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(copy.len() as u64, declared + 1);
    }

    #[test]
    fn every_byte_flip_is_refused_or_harmless() {
        let zip = standard(LINUX).zip();
        let original = open_and_verify(zip.clone()).unwrap();
        let mut wrong = Vec::new();
        let mut refused = 0;
        for at in 0..zip.len() {
            for bit in 0..u8::BITS {
                let mut flipped = zip.clone();
                flipped[at] ^= 1 << bit;
                match panic::catch_unwind(move || open_and_verify(flipped)) {
                    Ok(Err(_)) => refused += 1,
                    Ok(Ok(payload)) => {
                        let same = payload.build_info() == original.build_info()
                            && payload.entries() == original.entries();
                        if !same {
                            wrong.push(format!("byte {at}, bit {bit}: read as another runtime"));
                        }
                    }
                    Err(_) => wrong.push(format!("byte {at}, bit {bit}: panicked")),
                }
            }
        }
        assert!(wrong.is_empty(), "{}", wrong.join("\n"));
        eprintln!("{refused} of {} flips refused", 8 * zip.len());
    }

    #[test]
    fn every_truncation_is_refused() {
        let zip = standard(LINUX).zip();
        let mut wrong = Vec::new();
        for len in 0..zip.len() {
            let prefix = zip[..len].to_vec();
            match panic::catch_unwind(move || open_and_verify(prefix)) {
                Ok(Err(_)) => {}
                Ok(Ok(_)) => wrong.push(format!("the first {len} bytes: accepted")),
                Err(_) => wrong.push(format!("the first {len} bytes: panicked")),
            }
        }
        assert!(wrong.is_empty(), "{}", wrong.join("\n"));
    }

    #[test]
    fn payload_rules_are_stable() {
        let name = || -> Box<str> { "data/x.json".into() };
        let detail = || -> Box<str> { "what was found".into() };
        for (error, rule, shown) in [
            (
                PayloadError::Archive,
                "archive",
                "Not a runtime zip: it does not end in a ZIP end record without a comment",
            ),
            (
                PayloadError::Zip64,
                "zip64",
                "The runtime zip uses ZIP64, which no runtime needs",
            ),
            (
                PayloadError::Rows { detail: detail() },
                "rows",
                "The runtime zip's central directory is damaged: what was found",
            ),
            (
                PayloadError::TooManyEntries { count: 20_001 },
                "tooLarge",
                "Release payload is too large: 20001 entries",
            ),
            (
                PayloadError::TooLarge { bytes: 7 },
                "tooLarge",
                "Release payload is too large: 7 bytes",
            ),
            (
                PayloadError::Encrypted { name: name() },
                "encrypted",
                r#"Encrypted release entry: "data/x.json""#,
            ),
            (
                PayloadError::Method {
                    name: name(),
                    method: 12,
                },
                "method",
                r#"Release entry "data/x.json" is compressed by method 12, not stored or deflated"#,
            ),
            (
                PayloadError::DataDescriptor { name: name() },
                "dataDescriptor",
                r#"Release entry "data/x.json" gives its sizes after its data"#,
            ),
            (
                PayloadError::LocalHeader { detail: detail() },
                "localHeader",
                "A local header disagrees with the central directory: what was found",
            ),
            (
                PayloadError::Name {
                    error: NameError {
                        name: name(),
                        rule: NameRule::Dot,
                    },
                },
                "name",
                r#"Invalid release path: unsafe name "data/x.json": dot"#,
            ),
            (
                PayloadError::Directory { name: name() },
                "directory",
                r#"Duplicate, directory or linked release entry: "data/x.json""#,
            ),
            (
                PayloadError::Link { name: name() },
                "link",
                r#"Duplicate, directory or linked release entry: "data/x.json""#,
            ),
            (
                PayloadError::Duplicate { name: name() },
                "duplicate",
                "Duplicate release entry: data/x.json",
            ),
            (
                PayloadError::Conflict { name: name() },
                "conflict",
                "File/directory conflict in release payload: data/x.json",
            ),
            (
                PayloadError::UnknownRoot { name: name() },
                "unknownRoot",
                "Unexpected release payload file: data/x.json",
            ),
            (
                PayloadError::FileAsFolder { name: name() },
                "fileRoot",
                "Unexpected release payload directory: data/x.json",
            ),
            (
                PayloadError::FolderAsFile { name: name() },
                "folderRoot",
                "Release payload folder held as a file: data/x.json",
            ),
            (
                PayloadError::PlatformRoot { name: name() },
                "platformRoot",
                "Release payload file of another platform's runtime: data/x.json",
            ),
            (
                PayloadError::DataNotJson { name: name() },
                "dataNotJson",
                "Non-configuration file in release data: data/x.json",
            ),
            (
                PayloadError::Missing { name: name() },
                "incomplete",
                "Incomplete release payload: no data/x.json",
            ),
            (
                PayloadError::BuildInfoTooLarge { bytes: 7 },
                "buildInfoTooLarge",
                "Release metadata is too large: 7 bytes",
            ),
            (
                PayloadError::BuildInfo { detail: detail() },
                "buildInfo",
                "Invalid release metadata: what was found",
            ),
            (
                PayloadError::Version {
                    text: "v1.0".into(),
                },
                "version",
                r#"Invalid semantic release version: "v1.0""#,
            ),
            (
                PayloadError::Platform {
                    text: "another-os".into(),
                },
                "platform",
                r#"Release metadata names no platform a release is built for: "another-os""#,
            ),
            (
                PayloadError::Executable {
                    name: "game".into(),
                },
                "executable",
                r#"Release executable "game" is not the release platform's"#,
            ),
            (
                PayloadError::Digest { name: name() },
                "digest",
                "Invalid release digest: data/x.json",
            ),
            (
                PayloadError::Inventory { name: name() },
                "inventory",
                "Release file inventory does not match its payload: data/x.json",
            ),
            (
                PayloadError::VersionFile,
                "versionFile",
                "Release VERSION disagrees with its metadata",
            ),
            (
                PayloadError::Damaged {
                    name: name(),
                    detail: detail(),
                },
                "damaged",
                "Damaged release payload: data/x.json: what was found",
            ),
            (
                PayloadError::OtherPlatform {
                    platform: Platform::LinuxX64,
                },
                "otherPlatform",
                "This installer was built for a different operating system: the runtime is for \
                 linux-x64",
            ),
            (
                PayloadError::OtherVersion {
                    found: "1.0.1".into(),
                    expected: VERSION.into(),
                },
                "otherVersion",
                r#"The runtime's version "1.0.1" is not the launcher's version "1.0.0-beta""#,
            ),
            (PayloadError::Cancelled, "cancelled", "cancelled"),
            // A refusal of the launcher file's sections is reported as their own: its rule, its
            // text and its source.
            (
                PayloadError::Sections(SectionsError::MissingSection {
                    name: "runtime.zip".into(),
                }),
                "missingSection",
                r#"no section "runtime.zip""#,
            ),
            (
                PayloadError::Sections(SectionsError::Io(io::Error::other("unreadable sector"))),
                "io",
                "the launcher file cannot be read: unreadable sector",
            ),
            (
                PayloadError::Io(io::Error::other("unreadable sector")),
                "io",
                "The runtime zip cannot be read or copied: unreadable sector",
            ),
        ] {
            assert_eq!(error.rule(), rule, "{error}");
            assert_eq!(error.to_string(), shown, "{rule}");
            match &error {
                PayloadError::Name { error: inner } => {
                    let source = error.source().unwrap().downcast_ref::<NameError>();
                    assert_eq!(source, Some(inner));
                }
                PayloadError::Io(_) | PayloadError::Sections(SectionsError::Io(_)) => {
                    assert_eq!(error.source().unwrap().to_string(), "unreadable sector");
                }
                _ => assert!(error.source().is_none(), "{rule}"),
            }
        }
        assert!(size_of::<PayloadError>() <= MOST_ERROR_BYTES);
    }
}

#[cfg(test)]
mod crafted {
    use std::io::Cursor;

    use gdl_files::Cancel;
    use gdl_fixtures::{RawEntry, RawZip, RuntimeFiles};
    use serde_json::{Map, Value, json};

    use super::rows;
    use super::tests::{
        LINUX, SHADER, WINDOWS, build_info_of_len, declaring_in_all, entry, hex_sha256, listing,
        open, open_and_verify, quiet, standard,
    };
    use super::{
        BUILD_INFO, DEFAULT_CONFIG, MAX_BUILD_INFO_LEN, MAX_PAYLOAD_BYTES, MAX_PAYLOAD_ENTRIES,
        VERSION_FILE,
    };

    /// The rules decided once the rows have passed: where each name lies, which names lie
    /// together, what the entries declare in all, what the zip crate reads, what the release
    /// record says and whether it agrees with the archive, and whether the files read when the
    /// runtime is opened hold what they declare.
    const AFTER_THE_ROWS: [&str; 19] = [
        "unknownRoot",
        "fileRoot",
        "folderRoot",
        "dataNotJson",
        "duplicate",
        "conflict",
        "tooLarge",
        "incomplete",
        "buildInfoTooLarge",
        "localHeader",
        "buildInfo",
        "version",
        "platform",
        "executable",
        "digest",
        "inventory",
        "versionFile",
        "platformRoot",
        "damaged",
    ];

    /// The version the test runtimes carry.
    const VERSION: &str = "1.0.0-beta";

    /// The end record's length without a comment.
    const END_RECORD_LEN: usize = 22;

    // Where the fields the cases change lie in the end record.
    const END_DISK: usize = 4;
    const END_DIRECTORY_DISK: usize = 6;
    const END_DISK_COUNT: usize = 8;
    const END_COUNT: usize = 10;
    const END_DIRECTORY_LEN: usize = 12;
    const END_DIRECTORY_OFFSET: usize = 16;
    const END_COMMENT_LEN: usize = 20;

    // Where the fields the cases change lie in a row of the central directory.
    const ROW_NAME_LEN: usize = 28;
    const ROW_DISK: usize = 34;

    /// The longest name a runtime's entry may have.
    const LONGEST_NAME: usize = 1024;

    /// The longest component a name may have.
    const LONGEST_COMPONENT: usize = 200;

    /// A runtime zip with one thing wrong, and the rule that must refuse it.
    struct Refusal {
        what: String,
        rule: &'static str,
        zip: Vec<u8>,
    }

    /// The standard runtime of `platform` with `names` added, each in its inventory.
    fn with_files(platform: &str, names: &[&str]) -> Vec<u8> {
        let mut runtime = standard(platform);
        runtime.files.extend(
            names
                .iter()
                .map(|name| ((*name).to_owned(), b"crafted".to_vec())),
        );
        runtime.zip()
    }

    /// The standard runtime's archive for `platform`, changed field by field.
    fn changed(platform: &str, change: impl FnOnce(&mut RawZip)) -> Vec<u8> {
        let mut raw = standard(platform).raw();
        change(&mut raw);
        raw.to_bytes()
    }

    /// The standard runtime's archive for `platform`, its bytes changed.
    fn patched(platform: &str, change: impl FnOnce(&mut Vec<u8>)) -> Vec<u8> {
        let mut zip = standard(platform).zip();
        change(&mut zip);
        zip
    }

    /// The standard runtime of `platform`, changed file by file or field by field before its
    /// archive is written.
    fn with_runtime(platform: &str, change: impl FnOnce(&mut RuntimeFiles)) -> Vec<u8> {
        let mut runtime = standard(platform);
        change(&mut runtime);
        runtime.zip()
    }

    /// The standard runtime of `platform`, whose release record lists the files `change` leaves
    /// in its inventory.
    fn with_inventory(platform: &str, change: impl FnOnce(&mut Map<String, Value>)) -> Vec<u8> {
        with_runtime(platform, |runtime| {
            let mut files = listing(runtime);
            change(&mut files);
            runtime
                .extra
                .insert("files".to_owned(), Value::Object(files));
        })
    }

    /// The standard runtime of `platform` with `names` added to its archive, before
    /// `build-info.json`, and not to its inventory.
    fn with_entries(platform: &str, names: &[&str]) -> Vec<u8> {
        changed(platform, |raw| {
            let record = raw.entries.len() - 1;
            for name in names {
                raw.entries
                    .insert(record, RawEntry::deflated(name, b"crafted"));
            }
        })
    }

    /// The standard runtime of `platform`, its `build-info.json` holding `record`.
    fn with_record(platform: &str, record: &[u8]) -> Vec<u8> {
        changed(platform, |raw| {
            *entry(raw, BUILD_INFO) = RawEntry::deflated(BUILD_INFO, record);
        })
    }

    /// The standard runtime of `platform`, its `VERSION` holding `text`, which its inventory
    /// lists as it is.
    fn with_version_file(platform: &str, text: &[u8]) -> Vec<u8> {
        with_runtime(platform, |runtime| {
            let version = runtime
                .files
                .iter_mut()
                .find(|(name, _)| name == VERSION_FILE)
                .expect("every runtime has a VERSION");
            version.1 = text.to_vec();
        })
    }

    /// `runtime`'s archive, its release record held as it is, which builds fast however large the
    /// record is.
    fn with_stored_record(runtime: &RuntimeFiles) -> Vec<u8> {
        let mut small = runtime.clone();
        small.extra.clear();
        let mut raw = small.raw();
        *entry(&mut raw, BUILD_INFO) = RawEntry::stored(BUILD_INFO, &runtime.build_info_bytes());
        raw.to_bytes()
    }

    /// A `VERSION` of `len` bytes that holds the test runtimes' version, padded by spaces before
    /// its line end.
    fn version_file_of_len(len: u64) -> Vec<u8> {
        let padding = usize::try_from(len).unwrap() - VERSION.len() - 1;
        format!("{VERSION}{}\n", " ".repeat(padding)).into_bytes()
    }

    /// Where the end record of an archive without a comment starts.
    fn end_record(zip: &[u8]) -> usize {
        zip.len() - END_RECORD_LEN
    }

    /// Where the central directory starts, as the end record says.
    fn first_row(zip: &[u8]) -> usize {
        usize::try_from(u32_at(zip, end_record(zip) + END_DIRECTORY_OFFSET)).unwrap()
    }

    fn u16_at(zip: &[u8], at: usize) -> u16 {
        u16::from_le_bytes(zip[at..at + 2].try_into().unwrap())
    }

    fn u32_at(zip: &[u8], at: usize) -> u32 {
        u32::from_le_bytes(zip[at..at + 4].try_into().unwrap())
    }

    fn put_u16(zip: &mut [u8], at: usize, value: u16) {
        zip[at..at + 2].copy_from_slice(&value.to_le_bytes());
    }

    fn put_u32(zip: &mut [u8], at: usize, value: u32) {
        zip[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    /// A name of `len` characters under `shaders`, none of its components too long.
    fn name_of_len(len: usize) -> String {
        let mut name = String::from("shaders");
        while name.len() < len {
            let component = (len - name.len() - 1).min(LONGEST_COMPONENT);
            name.push('/');
            name.push_str(&"a".repeat(component));
        }
        assert_eq!(name.len(), len);
        name
    }

    /// How many rows an end record of the archive would count.
    fn rows(raw: &RawZip) -> u16 {
        u16::try_from(raw.entries.len()).unwrap()
    }

    /// The standard runtime broken one way at a time, each with the rule that refuses it first.
    fn refusals() -> Vec<Refusal> {
        let mut cases = Vec::new();
        let mut refuse = |what: &str, rule: &'static str, zip: Vec<u8>| {
            cases.push(Refusal {
                what: what.to_owned(),
                rule,
                zip,
            });
        };

        // Two entries of one name, in one letter case or two.
        let zip = changed(LINUX, |raw| {
            let at = raw
                .entries
                .iter()
                .position(|entry| entry.name == b"data/config.json")
                .unwrap();
            let twin = raw.entries[at].clone();
            raw.entries.insert(at + 1, twin);
        });
        refuse("an exact duplicate row", "duplicate", zip);
        for (platform, names) in [
            (LINUX, &["data/Config.json"][..]),
            (LINUX, &["shaders/A.spv", "shaders/a.spv"]),
            (WINDOWS, &["sdl3.DLL", "SDL3.dll"]),
        ] {
            let what = format!("{} on {platform}", names.join(" and "));
            refuse(&what, "duplicate", with_files(platform, names));
        }
        // A file that is also the folder above another, in one letter case or two.
        for names in [
            &["shaders/x", "shaders/x/y.spv"][..],
            &["shaders/X", "shaders/x/y.spv"],
        ] {
            let what = names.join(" and ");
            refuse(&what, "conflict", with_files(LINUX, names));
        }

        // Entries that are not regular files.
        for (what, mode) in [
            ("a symbolic link's mode", 0o120777),
            ("a FIFO's mode", 0o010644),
            ("a socket's mode", 0o140755),
        ] {
            let zip = changed(LINUX, |raw| {
                entry(raw, SHADER).external_attributes = mode << 16;
            });
            refuse(what, "link", zip);
        }
        let zip = changed(LINUX, |raw| {
            raw.entries.insert(0, RawEntry::stored("shaders/sub/", b""));
        });
        refuse("shaders/sub/", "directory", zip);
        let zip = changed(LINUX, |raw| {
            entry(raw, SHADER).external_attributes |= 0x10;
        });
        refuse("the DOS directory bit", "directory", zip);

        // Roots a runtime never writes, and its own roots in another letter case.
        for name in [
            "n64/extracted/x",
            "dreamcast/x",
            ".gdl-launcher/receipt.json",
            "saves/hero.json",
            "config/settings.json",
            "carddemo/icon.tpl",
            "Gauntlet/PDATA/WAR.WAD",
            "SDL3.dll/x",
        ] {
            refuse(name, "unknownRoot", with_files(LINUX, &[name]));
        }
        for (platform, name) in [
            (LINUX, "Data/config.json"),
            (LINUX, "DATA/config.json"),
            (LINUX, "version"),
            (LINUX, "GAUNTLET"),
            (LINUX, "BUILD-INFO.JSON"),
            (WINDOWS, "gauntlet.EXE"),
        ] {
            let what = format!("{name} on {platform}");
            refuse(&what, "unknownRoot", with_files(platform, &[name]));
        }

        // What may lie under data, and which roots are files and which folders.
        let zip = with_files(LINUX, &["data/texture.ngc"]);
        refuse("data/texture.ngc", "dataNotJson", zip);
        for (platform, name) in [
            (LINUX, "VERSION/x"),
            (LINUX, "build-info.json/x"),
            (LINUX, "portable.flag/x"),
            (LINUX, "gauntlet/x"),
            (WINDOWS, "gauntlet.exe/x"),
        ] {
            let what = format!("{name} on {platform}");
            refuse(&what, "fileRoot", with_files(platform, &[name]));
        }
        for names in [
            &["shaders"][..],
            &["data"],
            &["licenses"],
            &["lib", "lib/a.so"],
        ] {
            let what = format!("{} as files", names.join(" and "));
            refuse(&what, "folderRoot", with_files(LINUX, names));
        }

        // Too much of something.
        let zip = changed(LINUX, |raw| {
            let more = MAX_PAYLOAD_ENTRIES + 1 - raw.entries.len();
            raw.entries.extend(
                (0..more).map(|index| RawEntry::stored(&format!("shaders/{index:05}.spv"), b"")),
            );
        });
        refuse("20,001 rows", "tooLarge", zip);
        let zip = changed(LINUX, |raw| {
            raw.eocd_count = Some(u16::try_from(MAX_PAYLOAD_ENTRIES + 1).unwrap());
        });
        refuse("an end record counting 20,001 rows over 7", "tooLarge", zip);
        let zip = declaring_in_all(LINUX, MAX_PAYLOAD_BYTES + 1);
        refuse(
            "declared sizes summing to 2 GiB and a byte",
            "tooLarge",
            zip,
        );
        let zip = build_info_of_len(LINUX, MAX_BUILD_INFO_LEN + 1);
        refuse(
            "a build-info.json of 4 MiB and a byte",
            "buildInfoTooLarge",
            zip,
        );
        let long = name_of_len(LONGEST_NAME + 1);
        refuse(
            "a name of 1,025 characters",
            "name",
            with_files(LINUX, &[&long]),
        );
        let wide = format!("shaders/{}", "a".repeat(LONGEST_COMPONENT + 1));
        refuse(
            "a component of 201 characters",
            "name",
            with_files(LINUX, &[&wide]),
        );

        // Names that could leave the installation, name a device or carry the alpha's names.
        for name in [
            "data\\back.json",
            "shaders/a.spv\0.txt",
            "\u{e9}.json",
            "a\u{7f}b",
            "CON .txt",
            "installation.json",
            "data/installation.json",
            "licenses/GauntletDarkLegacy-Update.exe",
            "shaders/.gdl-update/x",
        ] {
            refuse(&format!("{name:?}"), "name", with_files(LINUX, &[name]));
        }
        let zip = changed(LINUX, |raw| {
            entry(raw, SHADER).name = b"shaders/\xfftest.spv".to_vec();
        });
        refuse("a name that is not UTF-8", "name", zip);

        // Archives no release script writes.
        for (what, flag) in [
            ("the encrypted flag", 0x0001),
            ("the strong-encryption flag", 0x0040),
            ("the masked-headers flag", 0x2000),
        ] {
            let zip = changed(LINUX, |raw| entry(raw, SHADER).flags |= flag);
            refuse(what, "encrypted", zip);
        }
        for method in [12, 14, 99, 93] {
            let zip = changed(LINUX, |raw| entry(raw, SHADER).method = method);
            refuse(&format!("method {method}"), "method", zip);
        }
        let zip = changed(LINUX, |raw| entry(raw, SHADER).flags |= 0x0008);
        refuse("the data-descriptor flag", "dataDescriptor", zip);
        let zip = changed(LINUX, |raw| {
            entry(raw, "data/config.json").local_name = Some(b"data/cOnfig.json".to_vec());
        });
        refuse(
            "a local header naming data/cOnfig.json under data/config.json",
            "localHeader",
            zip,
        );
        let zip = changed(LINUX, |raw| raw.eocd_count = Some(rows(raw) + 1));
        refuse("an end-record count one over", "rows", zip);
        let zip = changed(LINUX, |raw| raw.eocd_count = Some(rows(raw) - 1));
        refuse("an end-record count one under", "rows", zip);
        let zip = changed(LINUX, |raw| {
            entry(raw, SHADER).extra = vec![0x55, 0x54, 0x05, 0x00, 0x01, 0, 0, 0, 0];
        });
        refuse("an extra field in a row", "rows", zip);
        let zip = changed(LINUX, |raw| {
            entry(raw, SHADER).comment = b"a row's comment".to_vec();
        });
        refuse("a row comment", "rows", zip);
        let zip = changed(LINUX, |raw| raw.comment = b"an archive's comment".to_vec());
        refuse("an archive comment", "archive", zip);
        let zip = changed(LINUX, |raw| raw.zip64_locator = true);
        refuse("a ZIP64 locator", "zip64", zip);
        let zip = changed(LINUX, |raw| raw.eocd_count = Some(0xFFFF));
        refuse("a 0xFFFF count", "zip64", zip);
        let zip = changed(LINUX, |raw| raw.prefix = b"MZ\x90".to_vec());
        refuse("3 prepended bytes", "rows", zip);
        let zip = patched(LINUX, |zip| {
            zip.splice(0..0, *b"#!");
        });
        refuse(
            "bytes before the archive that its offsets leave out",
            "rows",
            zip,
        );
        let zip = patched(LINUX, |zip| {
            let at = end_record(zip);
            zip.splice(at..at, *b"\0\0");
        });
        refuse(
            "bytes between the central directory and the end record",
            "rows",
            zip,
        );
        let text = b"This is a text file, not a zip archive.\n".to_vec();
        refuse("not a zip", "archive", text);
        refuse(
            "a bare end-record signature",
            "archive",
            b"PK\x05\x06".to_vec(),
        );
        refuse("an empty file", "archive", Vec::new());

        // The other rules of the end record and the rows, a field at a time.
        for (what, at, value, rule) in [
            ("an end record on disk 1", END_DISK, 1, "rows"),
            (
                "a central directory on disk 1",
                END_DIRECTORY_DISK,
                1,
                "rows",
            ),
            ("an end record on disk 0xFFFF", END_DISK, 0xFFFF, "zip64"),
            (
                "a central directory on disk 0xFFFF",
                END_DIRECTORY_DISK,
                0xFFFF,
                "zip64",
            ),
            (
                "0xFFFF rows counted on the disk",
                END_DISK_COUNT,
                0xFFFF,
                "zip64",
            ),
            ("0xFFFF rows counted in all", END_COUNT, 0xFFFF, "zip64"),
            (
                "a comment length without a comment",
                END_COMMENT_LEN,
                1,
                "archive",
            ),
        ] {
            let zip = patched(LINUX, |zip| {
                let field = end_record(zip) + at;
                put_u16(zip, field, value);
            });
            refuse(what, rule, zip);
        }
        let zip = patched(LINUX, |zip| {
            let at = end_record(zip) + END_DISK_COUNT;
            let count = u16_at(zip, at);
            put_u16(zip, at, count - 1);
        });
        refuse("fewer rows counted on the disk than in all", "rows", zip);
        for (what, at) in [
            ("a central directory of 0xFFFFFFFF bytes", END_DIRECTORY_LEN),
            (
                "a central directory at byte 0xFFFFFFFF",
                END_DIRECTORY_OFFSET,
            ),
        ] {
            let zip = patched(LINUX, |zip| {
                let field = end_record(zip) + at;
                put_u32(zip, field, 0xFFFF_FFFF);
            });
            refuse(what, "zip64", zip);
        }
        let zip = patched(LINUX, |zip| {
            let at = end_record(zip) + END_DIRECTORY_LEN;
            let len = u32_at(zip, at);
            put_u32(zip, at, len + 1);
        });
        refuse(
            "a central directory that would end inside the end record",
            "rows",
            zip,
        );
        let zip = patched(LINUX, |zip| {
            let at = first_row(zip);
            zip[at] ^= 0xFF;
        });
        refuse("a row without its signature", "rows", zip);
        let zip = patched(LINUX, |zip| {
            let at = first_row(zip) + ROW_DISK;
            put_u16(zip, at, 1);
        });
        refuse("a row whose entry starts on disk 1", "rows", zip);
        let zip = patched(LINUX, |zip| {
            let at = first_row(zip) + ROW_NAME_LEN;
            put_u16(zip, at, 0xFFFF);
        });
        refuse(
            "a row whose name runs past the central directory",
            "rows",
            zip,
        );
        let zip = changed(LINUX, |raw| {
            *entry(raw, SHADER) = RawEntry {
                declared_size: Some(7),
                ..RawEntry::stored(SHADER, b"shader")
            };
        });
        refuse(
            "a stored entry that declares more than it holds",
            "rows",
            zip,
        );
        let zip = changed(LINUX, |raw| {
            raw.entries.pop();
        });
        refuse("no build-info.json", "incomplete", zip);

        // What a runtime holds for its platform alone, listed in its release record and held in
        // its archive, or only held there.
        for (platform, name, rule) in [
            (WINDOWS, "lib/libgcc_s.so.1", "platformRoot"),
            (LINUX, "SDL3.dll", "platformRoot"),
            (LINUX, "zlib.DLL", "platformRoot"),
            (WINDOWS, "gauntlet", "executable"),
            (LINUX, "gauntlet.exe", "executable"),
        ] {
            let what = format!("{name} on {platform}");
            refuse(&what, rule, with_files(platform, &[name]));
            let what = format!("{name} on {platform}, held in the archive alone");
            refuse(&what, rule, with_entries(platform, &[name]));
        }
        let zip = with_runtime(LINUX, |runtime| {
            runtime.executable = "gauntlet.exe".to_owned()
        });
        refuse(
            "a Linux release record naming Windows' executable",
            "executable",
            zip,
        );

        // Release records with too much in them.
        let mut runtime = standard(LINUX);
        let mut files = listing(&runtime);
        let more = MAX_PAYLOAD_ENTRIES + 1 - files.len();
        files.extend((0..more).map(|index| {
            let digest = Value::String(hex_sha256(b""));
            (format!("shaders/{index:05}.spv"), digest)
        }));
        runtime
            .extra
            .insert("files".to_owned(), Value::Object(files));
        let zip = with_stored_record(&runtime);
        refuse("an inventory of 20,001 files", "buildInfo", zip);

        // Inventories that do not list the archive's files as they are.
        let zip = with_inventory(LINUX, |files| {
            files.remove("data/text/en.json");
        });
        refuse("a file missing from the inventory", "inventory", zip);
        let zip = with_inventory(LINUX, |files| {
            let digest = Value::String(hex_sha256(b"crafted"));
            files.insert("shaders/extra.spv".to_owned(), digest);
        });
        refuse("a file only the inventory lists", "inventory", zip);
        let zip = with_inventory(LINUX, |files| {
            let digest = files.remove(SHADER).unwrap();
            files.insert("shaders/Test.spv".to_owned(), digest);
        });
        refuse(
            "the key shaders/Test.spv for the entry shaders/test.spv",
            "inventory",
            zip,
        );
        let zip = with_inventory(LINUX, |files| {
            let digest = Value::String(hex_sha256(b"crafted"));
            files.insert(BUILD_INFO.to_owned(), digest);
        });
        refuse("build-info.json listed", "duplicate", zip);
        let zip = with_inventory(LINUX, |files| {
            let digest = hex_sha256(b"{}").to_uppercase();
            files.insert(DEFAULT_CONFIG.to_owned(), Value::String(digest));
        });
        refuse("a digest in upper case", "digest", zip);
        let zip = with_runtime(LINUX, |runtime| {
            runtime.files.retain(|(name, _)| name != VERSION_FILE);
        });
        refuse("no VERSION", "incomplete", zip);
        let zip = with_runtime(LINUX, |runtime| {
            runtime.files.retain(|(name, _)| name != DEFAULT_CONFIG);
        });
        refuse("no data/config.json", "incomplete", zip);

        // Release records whose fields a release does not have.
        let zip = with_runtime(LINUX, |runtime| {
            runtime.version = "v0.1.0-alpha.1".to_owned();
        });
        refuse("the version v0.1.0-alpha.1", "version", zip);
        let zip = RuntimeFiles::standard("another-os", VERSION).zip();
        refuse("the platform another-os", "platform", zip);
        let zip = with_runtime(LINUX, |runtime| runtime.executable = "game".to_owned());
        refuse("the executable game", "executable", zip);
        let zip = with_runtime(LINUX, |runtime| {
            runtime.extra.insert("commit".to_owned(), json!(5));
        });
        refuse("a commit of 5", "buildInfo", zip);
        let zip = with_runtime(LINUX, |runtime| {
            runtime.extra.insert("files".to_owned(), json!([]));
        });
        refuse("an inventory list", "buildInfo", zip);
        refuse(
            "a build-info.json that is not JSON",
            "buildInfo",
            with_record(LINUX, b"not JSON\n"),
        );
        let record = [&b"\xef\xbb\xbf"[..], &standard(LINUX).build_info_bytes()].concat();
        refuse(
            "a build-info.json with a byte-order mark",
            "buildInfo",
            with_record(LINUX, &record),
        );
        refuse(
            "a build-info.json of a JSON list",
            "buildInfo",
            with_record(LINUX, b"[]\n"),
        );

        // VERSION files that are not the record's version.
        let zip = with_version_file(LINUX, &version_file_of_len(129));
        refuse("a VERSION of 129 bytes", "versionFile", zip);
        let zip = with_version_file(LINUX, b"1.0.1\n");
        refuse("a VERSION of another version", "versionFile", zip);
        let zip = with_version_file(LINUX, b"\xff1.0.0-beta\n");
        refuse("a VERSION that is not UTF-8", "versionFile", zip);
        let zip = with_version_file(LINUX, b"1.0.0-beta.1\n");
        refuse(
            "a VERSION that only begins with the version",
            "versionFile",
            zip,
        );

        // The files read as the runtime is opened, when they do not hold what they declare.
        for name in [BUILD_INFO, VERSION_FILE] {
            let zip = changed(LINUX, |raw| {
                let file = entry(raw, name);
                file.crc = file.crc.map(|crc| crc ^ 1);
            });
            refuse(&format!("a checksum that lies for {name}"), "damaged", zip);
            let zip = changed(LINUX, |raw| {
                let file = entry(raw, name);
                file.declared_size = file.declared_size.map(|size| size + 1);
            });
            refuse(
                &format!("a {name} that holds less than it declares"),
                "damaged",
                zip,
            );
        }

        cases
    }

    /// The standard runtime with one file's bytes damaged, which is opened and refused only when
    /// that file is read, each with the rule that refuses it then.
    fn damaged_files() -> Vec<Refusal> {
        let mut cases = Vec::new();
        let mut refuse = |what: &str, zip: Vec<u8>| {
            cases.push(Refusal {
                what: what.to_owned(),
                rule: "damaged",
                zip,
            });
        };
        let zip = changed(LINUX, |raw| entry(raw, SHADER).declared_size = Some(3));
        refuse("deflated output past the declared size", zip);
        let zip = changed(LINUX, |raw| entry(raw, SHADER).declared_size = Some(7));
        refuse(
            "deflated output short of the declared size, with its own checksum",
            zip,
        );
        let zip = changed(LINUX, |raw| {
            let shader = entry(raw, SHADER);
            shader.crc = shader.crc.map(|crc| crc ^ 1);
        });
        refuse("a checksum that is not the file's", zip);
        let zip = changed(LINUX, |raw| entry(raw, SHADER).data.truncate(2));
        refuse("a deflate stream cut short", zip);
        let zip = with_inventory(LINUX, |files| {
            let digest = Value::String(hex_sha256(b"damaged"));
            files.insert(DEFAULT_CONFIG.to_owned(), digest);
        });
        refuse("a wrong digest value", zip);
        cases
    }

    #[test]
    fn each_crafted_runtime_is_refused_for_its_rule() {
        let mut wrong = Vec::new();
        for Refusal { what, rule, zip } in refusals() {
            match open(zip) {
                Ok(_) => wrong.push(format!("{what}: accepted, not refused for {rule}")),
                Err(error) if error.rule() != rule => wrong.push(format!(
                    "{what}: refused for {} ({error}), not for {rule}",
                    error.rule()
                )),
                Err(_) => {}
            }
        }
        // A file damaged where opening the runtime does not read is refused as it is read.
        for Refusal { what, rule, zip } in damaged_files() {
            let mut payload = match open(zip) {
                Ok(payload) => payload,
                Err(error) => {
                    wrong.push(format!("{what}: refused as it is opened ({error})"));
                    continue;
                }
            };
            match payload.verify(&quiet(&Cancel::default())) {
                Ok(()) => wrong.push(format!("{what}: verified, not refused for {rule}")),
                Err(error) if error.rule() != rule => wrong.push(format!(
                    "{what}: refused for {} ({error}), not for {rule}",
                    error.rule()
                )),
                Err(_) => {}
            }
        }
        assert!(wrong.is_empty(), "{}", wrong.join("\n"));
    }

    #[test]
    fn the_rows_refuse_their_own_faults_before_the_zip_crate_reads_them() {
        // The zip crate refuses some of these archives too, so a case the rows pass must be one
        // whose rule is decided after them.
        let mut wrong = Vec::new();
        for Refusal { what, rule, zip } in refusals() {
            match rows::read_entries(&mut Cursor::new(zip)) {
                Err(error) if error.rule() != rule => wrong.push(format!(
                    "{what}: the rows refuse it for {} ({error}), not for {rule}",
                    error.rule()
                )),
                Err(_) => {}
                Ok(_) if !AFTER_THE_ROWS.contains(&rule) => {
                    wrong.push(format!("{what}: the rows pass, though {rule} is theirs"));
                }
                Ok(_) => {}
            }
        }
        assert!(wrong.is_empty(), "{}", wrong.join("\n"));
    }

    #[test]
    fn crafted_runtimes_that_keep_the_rules_are_accepted() {
        let longest = name_of_len(LONGEST_NAME);
        for (what, zip) in [
            ("data/x.JSON", with_files(LINUX, &["data/x.JSON"])),
            ("a library at the top", with_files(WINDOWS, &["SDL3.dll"])),
            (
                "a library under lib",
                with_files(LINUX, &["lib/libgcc_s.so.1"]),
            ),
            (
                "a name ending .dll below the top, on Linux",
                with_files(LINUX, &["lib/plugins.dll", "licenses/SDL3.dll"]),
            ),
            ("a licence", with_files(LINUX, &["licenses/zip.txt"])),
            (
                "an executable's mode",
                changed(LINUX, |raw| {
                    entry(raw, "gauntlet").external_attributes = 0o100755 << 16;
                }),
            ),
            (
                "no mode, as DOS writes",
                changed(WINDOWS, |raw| {
                    entry(raw, "gauntlet.exe").external_attributes = 0;
                }),
            ),
            (
                "names that only resemble refused ones",
                with_files(
                    LINUX,
                    &[
                        "shaders/CONSOLE.spv",
                        "data/version.json",
                        "licenses/installation.json.txt",
                        "licenses/a..b",
                    ],
                ),
            ),
            ("the longest name", with_files(LINUX, &[&longest])),
            (
                "release metadata of keys the record does not know",
                with_runtime(LINUX, |runtime| {
                    let future = json!({"x": [1, 2.5, null], "y": "z"});
                    runtime.extra.insert("future".to_owned(), future);
                }),
            ),
            (
                "a version with build metadata",
                RuntimeFiles::standard(LINUX, "0.1.0-alpha.1+abc").zip(),
            ),
            (
                "a VERSION of 128 bytes",
                with_version_file(LINUX, &version_file_of_len(128)),
            ),
            (
                "a VERSION without a line end",
                with_version_file(LINUX, VERSION.as_bytes()),
            ),
            (
                "a VERSION between spaces and a Windows line end",
                with_version_file(LINUX, b" \t1.0.0-beta \r\n"),
            ),
            (
                "a build-info.json held as it is",
                changed(WINDOWS, |raw| {
                    let record = standard(WINDOWS).build_info_bytes();
                    *entry(raw, BUILD_INFO) = RawEntry::stored(BUILD_INFO, &record);
                }),
            ),
        ] {
            if let Err(error) = open_and_verify(zip) {
                panic!("{what}: {error}");
            }
        }
        // What the record holds beyond its own fields is kept.
        let zip = with_runtime(LINUX, |runtime| {
            runtime.extra.insert("future".to_owned(), json!({"x": 1}));
        });
        let payload = open(zip).unwrap();
        let mut other = Map::new();
        other.insert("future".to_owned(), json!({"x": 1}));
        assert_eq!(payload.build_info().other, other);
    }
}
