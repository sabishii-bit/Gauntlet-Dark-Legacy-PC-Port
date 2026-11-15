//! The rules a game runtime's zip is held to before the launcher takes a file from it: an archive
//! as the release scripts write one, whose names cannot leave the installation, name only the
//! runtime's own files and folders, and clash with no other name on a case-insensitive file
//! system.

mod rows;

use std::io::{self, Read, Seek};
use std::marker::PhantomData;

use gdl_files::{NameError, NameSet, RelPath};
use gdl_release::Platform;
use zip::ZipArchive;

/// The most entries a runtime may have.
pub const MAX_PAYLOAD_ENTRIES: usize = 20_000;

/// The most bytes a runtime's entries may declare in all, uncompressed.
pub const MAX_PAYLOAD_BYTES: u64 = 2 << 30;

/// The most bytes `build-info.json` may declare.
pub const MAX_BUILD_INFO_LEN: u64 = 4 << 20;

/// The release record at the top of a runtime, which lists every other file by its digest.
pub const BUILD_INFO: &str = "build-info.json";

/// The file at the top of a runtime that holds its version.
pub const VERSION_FILE: &str = "VERSION";

/// The file at the top of a runtime that makes the game keep its settings and saves beside it.
pub const PORTABLE_FLAG: &str = "portable.flag";

/// The files a runtime may have at its top, beside its platform's executable.
pub const ROOT_FILES: [&str; 3] = [VERSION_FILE, BUILD_INFO, PORTABLE_FLAG];

/// The folders a runtime may have at its top. Only a Linux runtime has `lib`.
pub const ROOT_FOLDERS: [&str; 4] = ["shaders", DATA_FOLDER, "licenses", "lib"];

/// The suffix, in any ASCII letter case, of the libraries a Windows runtime has at its top.
pub const LIBRARY_SUFFIX: &str = ".dll";

/// The suffix, in any ASCII letter case, of every file under `data`.
pub const DATA_SUFFIX: &str = ".json";

/// The folder of the settings defaults and text tables, which hold nothing but JSON.
const DATA_FOLDER: &str = "data";

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

/// A runtime zip whose end record, central directory, names and local headers keep to the
/// payload's rules, and the entries it holds.
#[derive(Debug)]
pub struct Payload<R> {
    /// The archive's entries, in the order of its central directory.
    entries: Vec<PayloadEntry>,
    /// The kind of reader the archive was read through.
    reader: PhantomData<R>,
}

impl<R: Read + Seek> Payload<R> {
    /// Reads the runtime zip in `reader` and holds it to the payload's rules, refusing it for the
    /// first rule it breaks. Its end record must end the archive, without a comment or ZIP64, and
    /// count at most [`MAX_PAYLOAD_ENTRIES`] rows; the central directory must hold exactly those
    /// rows, without extra fields or comments; no entry may be encrypted, compressed but by
    /// deflate, or a folder or link, and no bytes may come before the first; every name must keep
    /// to the payload's name rules, lie under one of the runtime's own roots in their exact case,
    /// and be one no other name shares in any letter case or stands above; the declared sizes
    /// must keep within [`MAX_PAYLOAD_BYTES`], with a `build-info.json` of at most
    /// [`MAX_BUILD_INFO_LEN`]; and the zip crate must read the same entries, every local header
    /// agreeing with its row.
    pub fn open(mut reader: R) -> Result<Payload<R>, PayloadError> {
        let entries = rows::read_entries(&mut reader)?;
        for entry in &entries {
            check_root(&entry.name)?;
        }
        check_names_apart(&entries)?;
        check_sizes(&entries)?;
        check_with_zip(reader, &entries)?;
        Ok(Payload {
            entries,
            reader: PhantomData,
        })
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
/// `build-info.json` of at most [`MAX_BUILD_INFO_LEN`].
fn check_sizes(entries: &[PayloadEntry]) -> Result<(), PayloadError> {
    let bytes = total_size(entries);
    if bytes > MAX_PAYLOAD_BYTES {
        return Err(PayloadError::TooLarge { bytes });
    }
    let build_info = entries
        .iter()
        .find(|entry| entry.name.as_str() == BUILD_INFO)
        .ok_or_else(|| PayloadError::Missing {
            name: BUILD_INFO.into(),
        })?;
    if build_info.size > MAX_BUILD_INFO_LEN {
        return Err(PayloadError::BuildInfoTooLarge {
            bytes: build_info.size,
        });
    }
    Ok(())
}

/// Has the zip crate read the archive, and refuses it unless the crate reads the same entries, as
/// many and in the same order, and finds every local header in agreement with its row and no two
/// entries overlapping.
fn check_with_zip<R: Read + Seek>(reader: R, entries: &[PayloadEntry]) -> Result<(), PayloadError> {
    let mut archive = ZipArchive::new(reader).map_err(|error| PayloadError::Rows {
        detail: format!("the zip crate refuses the archive: {error}").into(),
    })?;
    let names = entries.iter().map(|entry| entry.name.as_str().as_bytes());
    if !archive.file_names_raw().eq(names) {
        return Err(PayloadError::Rows {
            detail: "the zip crate reads other entries than the central directory's rows".into(),
        });
    }
    archive
        .validate_header_consistency()
        .map_err(|error| PayloadError::LocalHeader {
            detail: error.to_string().into(),
        })
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
    /// The archive could not be read.
    #[error("The runtime zip cannot be read: {0}")]
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
            PayloadError::DataNotJson { .. } => "dataNotJson",
            PayloadError::Missing { .. } => "incomplete",
            PayloadError::BuildInfoTooLarge { .. } => "buildInfoTooLarge",
            PayloadError::Io(_) => "io",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::io::{self, Cursor, Read, Seek, SeekFrom};

    use gdl_files::{NameError, NameRule, PAYLOAD_NAMES, RelPath};
    use gdl_fixtures::{RawEntry, RawZip, RuntimeFiles};
    use zip::ZipArchive;

    use super::{
        BUILD_INFO, MAX_BUILD_INFO_LEN, MAX_PAYLOAD_BYTES, MAX_PAYLOAD_ENTRIES, Payload,
        PayloadEntry, PayloadError, check_with_zip,
    };

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
        // The release scripts' limits: 2 GiB in all, and 4 MiB of release record.
        assert_eq!(MAX_PAYLOAD_BYTES, 2 * 1024 * 1024 * 1024);
        assert_eq!(MAX_BUILD_INFO_LEN, 4 * 1024 * 1024);
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

        // An entry the crate does not read, one too few, and another name in an entry's place.
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
        let mut reordered = entries.clone();
        reordered.swap(0, 1);
        for (what, claimed) in [
            ("more", more),
            ("fewer", fewer),
            ("renamed", renamed),
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
                PayloadError::Io(io::Error::other("unreadable sector")),
                "io",
                "The runtime zip cannot be read: unreadable sector",
            ),
        ] {
            assert_eq!(error.rule(), rule, "{error}");
            assert_eq!(error.to_string(), shown, "{rule}");
            match &error {
                PayloadError::Name { error: inner } => {
                    let source = error.source().unwrap().downcast_ref::<NameError>();
                    assert_eq!(source, Some(inner));
                }
                PayloadError::Io(_) => {
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

    use gdl_fixtures::{RawEntry, RawZip};

    use super::rows;
    use super::tests::{
        LINUX, SHADER, WINDOWS, build_info_of_len, declaring_in_all, entry, open, standard,
    };
    use super::{MAX_BUILD_INFO_LEN, MAX_PAYLOAD_BYTES, MAX_PAYLOAD_ENTRIES};

    /// The rules decided once the rows have passed: where each name lies, which names lie
    /// together, what the entries declare in all, and what the zip crate reads.
    const AFTER_THE_ROWS: [&str; 10] = [
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
    ];

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
        ] {
            if let Err(error) = open(zip) {
                panic!("{what}: {error}");
            }
        }
    }
}
