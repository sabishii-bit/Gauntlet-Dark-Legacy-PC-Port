//! What the readers report when a dump cannot be read or is refused: the path a read failed on and
//! what the system answered, that the job reading the dump was asked to stop, or the rule of its
//! format the dump breaks, as technical detail for logs. What a player reads is chosen from the
//! failure elsewhere.

use std::io;
use std::path::{Path, PathBuf};

use gdl_files::Cancelled;

/// What a reader answers for a dump path that names no file, such as a root.
const NAMES_NO_FILE: &str = "the path names no file";

/// Why a dump could not be read or was refused.
#[derive(Debug, thiserror::Error)]
pub enum MediaError {
    /// Reading `path` failed: the system refused it or could not give its bytes.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The file or folder that was being read.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The job reading the dump was asked to stop before the read was done.
    #[error("cancelled")]
    Cancelled,
    /// The N64 ROM breaks a rule of its format.
    #[error("{detail}")]
    Rom {
        /// The rule it breaks.
        rule: RomRule,
        /// What is wrong, in English, in the N64 extractor's own words where it has fixed words
        /// for it.
        detail: Box<str>,
    },
}

impl MediaError {
    /// The failure's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        // Every variant is named, with no wildcard, so that a failure added later is given its
        // rule on purpose.
        match self {
            MediaError::Io { .. } => "io",
            MediaError::Cancelled => "cancelled",
            MediaError::Rom { rule, .. } => rule.as_str(),
        }
    }

    /// Reading `path` failed with `source`.
    pub(crate) fn io(path: &Path, source: io::Error) -> MediaError {
        MediaError::Io {
            path: path.to_path_buf(),
            source,
        }
    }

    /// The refusal of a dump path that names no file, such as a root or a path that ends in
    /// `..`, which a reader gives before it reads anything.
    pub(crate) fn names_no_file(path: &Path) -> MediaError {
        let unnamed = io::Error::new(io::ErrorKind::InvalidInput, NAMES_NO_FILE);
        MediaError::io(path, unnamed)
    }

    /// An N64 ROM refused by `rule`, for the reason `detail` gives.
    pub(crate) fn rom(rule: RomRule, detail: impl Into<Box<str>>) -> MediaError {
        MediaError::Rom {
            rule,
            detail: detail.into(),
        }
    }
}

impl From<Cancelled> for MediaError {
    /// A reader stops where its job's cancel tells it to, between two blocks of its work.
    fn from(_: Cancelled) -> MediaError {
        MediaError::Cancelled
    }
}

/// The rules of the N64 ROM's format that a ROM is refused by.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RomRule {
    /// The ROM is shorter than its header, or does not end on a whole word.
    Header,
    /// The ROM's first word is that of none of the byte orders dumps keep a cartridge in.
    ByteOrder,
    /// The ROM is longer than the largest cartridge.
    RomTooLarge,
    /// The ROM holds no ROMDISK header, or more than one.
    RomdiskHeader,
    /// A part the ROM describes reaches past the data that holds it, as the ROMDISK's directory
    /// past the ROM's end or a file's bytes into the directory.
    OutsideData,
    /// The ROMDISK header's reserved word is not zero, or it lists no file or more than the most a
    /// ROMDISK may list.
    DirectoryCount,
    /// The ROMDISK's pointer table starts inside its header.
    DirectoryOverlap,
    /// A ROMDISK record starts before the one listed before it ends, or off a whole word.
    RecordPlace,
    /// A ROMDISK record's path has no end within the directory.
    UnterminatedPath,
    /// A ROMDISK record's path holds a byte outside ASCII.
    NonAsciiPath,
    /// A ROMDISK record's path does not begin with exactly one `/`.
    LeadingSlash,
    /// A ROMDISK record's path is no name the extracted tree may hold.
    UnsafePath,
    /// Two ROMDISK paths are the same, or differ only in letter case.
    DuplicatePath,
    /// A ROMDISK path is also the folder above another.
    Collision,
    /// A ROMDISK record's flags are neither none nor compressed.
    Flags,
    /// A ROMDISK file expands past the most a file may, or one kept as it is stores other than
    /// its size.
    FileSize,
    /// A ROMDISK file's bytes start inside its header.
    FileOverlap,
    /// The ROMDISK's files together expand past the most they may.
    ExpandedSize,
}

impl RomRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            RomRule::Header => "header",
            RomRule::ByteOrder => "byteOrder",
            RomRule::RomTooLarge => "romTooLarge",
            RomRule::RomdiskHeader => "romdiskHeader",
            RomRule::OutsideData => "outsideData",
            RomRule::DirectoryCount => "directoryCount",
            RomRule::DirectoryOverlap => "directoryOverlap",
            RomRule::RecordPlace => "recordPlace",
            RomRule::UnterminatedPath => "unterminatedPath",
            RomRule::NonAsciiPath => "nonAsciiPath",
            RomRule::LeadingSlash => "leadingSlash",
            RomRule::UnsafePath => "unsafePath",
            RomRule::DuplicatePath => "duplicatePath",
            RomRule::Collision => "collision",
            RomRule::Flags => "flags",
            RomRule::FileSize => "fileSize",
            RomRule::FileOverlap => "fileOverlap",
            RomRule::ExpandedSize => "expandedSize",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::io;
    use std::path::Path;

    use gdl_files::Cancel;

    use super::{MediaError, RomRule};

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// Reads nothing, but stops as a reader does before each block when its job is asked to.
    fn read_until_cancelled(cancel: &Cancel) -> Result<(), MediaError> {
        cancel.check()?;
        Ok(())
    }

    #[test]
    fn failures_name_their_path_and_say_what_failed() {
        let path = Path::new("dumps").join("dump.z64");
        let missing = io::Error::new(io::ErrorKind::NotFound, "missing");
        let error = MediaError::io(&path, missing);
        assert_eq!(error.to_string(), format!("{}: missing", path.display()));
        assert_eq!(error.source().unwrap().to_string(), "missing");
        assert!(matches!(&error, MediaError::Io { path: at, .. } if *at == path));

        assert_eq!(MediaError::Cancelled.to_string(), "cancelled");
        assert!(MediaError::Cancelled.source().is_none());

        // A path that names no file is refused as an argument no file could answer, before the
        // system is asked anything.
        let unnamed = Path::new("dumps").join("..");
        let error = MediaError::names_no_file(&unnamed);
        assert_eq!(
            error.to_string(),
            format!("{}: the path names no file", unnamed.display())
        );
        let MediaError::Io { path: at, source } = &error else {
            panic!("{error:?}");
        };
        assert_eq!(*at, unnamed);
        assert_eq!(source.kind(), io::ErrorKind::InvalidInput);
        assert_eq!(error.rule(), "io");

        // A refusal says what breaks the rule, and nothing the system answered caused it.
        let detail = "unrecognized Nintendo 64 ROM byte order";
        let refused = MediaError::rom(RomRule::ByteOrder, detail);
        assert_eq!(refused.to_string(), detail);
        assert!(refused.source().is_none());
        assert!(matches!(
            &refused,
            MediaError::Rom { rule: RomRule::ByteOrder, detail: said } if &**said == detail
        ));
        let worded = MediaError::rom(RomRule::RomTooLarge, format!("{} bytes", 1 << 26));
        assert_eq!(worded.to_string(), "67108864 bytes");

        assert!(size_of::<MediaError>() <= MOST_ERROR_BYTES);
    }

    #[test]
    fn every_failure_names_its_rule() {
        let path = Path::new("dumps").join("dump.z64");
        let failures = [
            // The rule is the variant's, whatever the system answered.
            (
                MediaError::io(&path, io::ErrorKind::PermissionDenied.into()),
                "io",
            ),
            (
                MediaError::io(&path, io::ErrorKind::UnexpectedEof.into()),
                "io",
            ),
            (MediaError::Cancelled, "cancelled"),
            // A refusal is named by the rule it breaks, whatever its words.
            (MediaError::rom(RomRule::Header, "short"), "header"),
            (MediaError::rom(RomRule::ByteOrder, "order"), "byteOrder"),
            (MediaError::rom(RomRule::RomTooLarge, "long"), "romTooLarge"),
        ];
        for (failure, rule) in failures {
            assert_eq!(failure.rule(), rule, "{failure:?}");
        }
        // Every rule of the ROM's format keeps the name the shared vectors give it.
        let rules = [
            (RomRule::Header, "header"),
            (RomRule::ByteOrder, "byteOrder"),
            (RomRule::RomTooLarge, "romTooLarge"),
            (RomRule::RomdiskHeader, "romdiskHeader"),
            (RomRule::OutsideData, "outsideData"),
            (RomRule::DirectoryCount, "directoryCount"),
            (RomRule::DirectoryOverlap, "directoryOverlap"),
            (RomRule::RecordPlace, "recordPlace"),
            (RomRule::UnterminatedPath, "unterminatedPath"),
            (RomRule::NonAsciiPath, "nonAsciiPath"),
            (RomRule::LeadingSlash, "leadingSlash"),
            (RomRule::UnsafePath, "unsafePath"),
            (RomRule::DuplicatePath, "duplicatePath"),
            (RomRule::Collision, "collision"),
            (RomRule::Flags, "flags"),
            (RomRule::FileSize, "fileSize"),
            (RomRule::FileOverlap, "fileOverlap"),
            (RomRule::ExpandedSize, "expandedSize"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(MediaError::rom(rule, "").rule(), name);
        }
    }

    #[test]
    fn a_cancelled_job_stops_the_read() {
        let cancel = Cancel::default();
        assert!(read_until_cancelled(&cancel).is_ok());

        cancel.cancel();
        let error = read_until_cancelled(&cancel).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        assert_eq!(error.rule(), "cancelled");
    }
}
