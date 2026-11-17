//! What an extraction reports when it fails: the dump could not be read or breaks a rule of its
//! format, the staging refused a file or a folder of the tree, writing or reading back a staged
//! file failed, the tree the dump would make breaks a rule that keeps it one tree on every system,
//! the disc's native table is missing or cannot be restored, the dump changed between two reads of
//! it, an installation's trust names another dump, a file of the disc does not match its
//! fingerprint, or the job was asked to stop, as technical detail for logs. What a player reads is
//! chosen from the failure elsewhere.

use std::io;
use std::path::PathBuf;

use gdl_files::{Cancelled, FilesError};
use gdl_media::{DumpRefusal, MediaError};

/// Why an extraction failed.
#[derive(Debug, thiserror::Error)]
pub enum ExtractError {
    /// The dump could not be read, or it breaks a rule of its format or of the native files it
    /// holds. A reader that stops because the job asked it to is [`ExtractError::Cancelled`].
    #[error(transparent)]
    Media(MediaError),
    /// The staging refused to make a file or a folder of the tree: its name was taken, a link
    /// stood on its way, or the system refused.
    #[error(transparent)]
    Files(#[from] FilesError),
    /// Writing, syncing or reading back a file made in the staging failed.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The staged file.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The tree the dump would make breaks a rule that keeps it one tree on every system.
    #[error("{detail}")]
    Tree {
        /// The rule it breaks.
        rule: TreeRule,
        /// What is wrong, in English.
        detail: Box<str>,
    },
    /// The disc's native table, the files DISK.ROM packs and the table SIZES.ROM that lists them,
    /// is not there to be read, is too large to read, or cannot be restored from what was staged.
    #[error("{detail}")]
    Native {
        /// The rule it breaks.
        rule: NativeRule,
        /// What is wrong, in English, in the Dreamcast extractor's own words where it has fixed
        /// words for it.
        detail: Box<str>,
    },
    /// A file of the dump gave other bytes when it was read to be written than when it was read
    /// to be checked.
    #[error("the dump changed while it was read: {path}")]
    Changed {
        /// The file's path in the tree.
        path: Box<str>,
    },
    /// The dump is not the one an installation's trust names: another game, another region or
    /// revision of it, or a cartridge whose header names the trusted one while its bytes differ.
    #[error("the dump is refused for an installation: {}", .refusal.as_str())]
    Refused {
        /// What its identity says of it against the trust.
        refusal: DumpRefusal,
    },
    /// A file of the disc does not match the fingerprints an installation's trust holds, or they
    /// hold one for a file the disc does not have.
    #[error("{path}: {}", .rule.as_str())]
    Fingerprint {
        /// The file's path in the tree, or, for a fingerprint of no file, the digest it is kept
        /// under, in lowercase hexadecimal.
        path: Box<str>,
        /// The rule it breaks.
        rule: FingerprintRule,
    },
    /// The job was asked to stop before the extraction was done.
    #[error("cancelled")]
    Cancelled,
}

impl ExtractError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the reader's rule
    /// for a dump it refused or could not read, the staging's for a file or folder it refused,
    /// the refusal's for a dump the trust does not name, and the extraction's own otherwise.
    pub fn rule(&self) -> &'static str {
        // Every variant is named, with no wildcard, so that a failure added later is given its
        // rule on purpose.
        match self {
            ExtractError::Media(error) => error.rule(),
            ExtractError::Files(error) => error.rule(),
            ExtractError::Io { .. } => "io",
            ExtractError::Tree { rule, .. } => rule.as_str(),
            ExtractError::Native { rule, .. } => rule.as_str(),
            ExtractError::Changed { .. } => "changed",
            ExtractError::Refused { refusal } => refusal.as_str(),
            ExtractError::Fingerprint { rule, .. } => rule.as_str(),
            ExtractError::Cancelled => "cancelled",
        }
    }

    /// A tree refused by `rule`, for the reason `detail` gives.
    pub(crate) fn tree(rule: TreeRule, detail: impl Into<Box<str>>) -> ExtractError {
        ExtractError::Tree {
            rule,
            detail: detail.into(),
        }
    }

    /// A native table refused by `rule`, for the reason `detail` gives.
    pub(crate) fn native(rule: NativeRule, detail: impl Into<Box<str>>) -> ExtractError {
        ExtractError::Native {
            rule,
            detail: detail.into(),
        }
    }

    /// A disc refused by the fingerprint `rule`, at the tree's path `path`.
    pub(crate) fn fingerprint(rule: FingerprintRule, path: impl Into<Box<str>>) -> ExtractError {
        ExtractError::Fingerprint {
            path: path.into(),
            rule,
        }
    }
}

impl From<Cancelled> for ExtractError {
    /// An extraction stops where its job's cancel tells it to, between two blocks of its work.
    fn from(_: Cancelled) -> ExtractError {
        ExtractError::Cancelled
    }
}

impl From<MediaError> for ExtractError {
    /// A reader's failure is the extraction's, but for a stop the job asked for, which is the
    /// extraction's own whichever part of it heeded the job's cancel: a reader of the dump or the
    /// writer of the tree.
    fn from(error: MediaError) -> ExtractError {
        match error {
            MediaError::Cancelled => ExtractError::Cancelled,
            error => ExtractError::Media(error),
        }
    }
}

/// The rules that keep an extracted tree one tree on every system, which an extraction holds
/// every path it will write to before it writes the first.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TreeRule {
    /// Two of the tree's paths are the same, or differ only in letter case, so that a file system
    /// that folds letter case would hold one file for both.
    Duplicate,
    /// A folder of the tree is spelled in two letter cases, which Windows would make one folder
    /// and Linux two.
    FolderCase,
    /// A path of the tree is a file and also the folder above another, in any letter case.
    Conflict,
}

impl TreeRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            TreeRule::Duplicate => "treeDuplicate",
            TreeRule::FolderCase => "treeFolderCase",
            TreeRule::Conflict => "treeConflict",
        }
    }
}

/// The rules a Dreamcast extraction holds the disc's native table to, beyond those of its format:
/// that it is there, that it may be read whole, and that what was staged of it holds every file it
/// lists.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum NativeRule {
    /// The disc's root holds no SIZES.ROM or no DISK.ROM, by exactly those names.
    TableMissing,
    /// SIZES.ROM holds more bytes than a packed file table may be read whole in.
    TooLarge,
    /// The staged DISK.ROM ended before a file SIZES.ROM places in it.
    DiskTruncated,
}

impl NativeRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            NativeRule::TableMissing => "nativeTableMissing",
            NativeRule::TooLarge => "sizesTooLarge",
            NativeRule::DiskTruncated => "diskTruncated",
        }
    }
}

/// The rules an installation's extraction holds a disc's files to when its trust holds their
/// fingerprints: IP.BIN and every disc file must have one, every one must be theirs, and each
/// must match what the disc holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FingerprintRule {
    /// A file of the disc has no fingerprint.
    Missing,
    /// A fingerprint is of no file of the disc.
    Unexpected,
    /// A file of the disc does not match its fingerprint.
    Digest,
}

impl FingerprintRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            FingerprintRule::Missing => "fingerprintMissing",
            FingerprintRule::Unexpected => "fingerprintUnexpected",
            FingerprintRule::Digest => "fingerprintDigest",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::io;
    use std::path::Path;

    use gdl_files::{Cancelled, DISC_NAMES, FilesError, RelPath};
    use gdl_media::{DumpRefusal, MediaError, Trailer, inflate};

    use super::{ExtractError, FingerprintRule, NativeRule, TreeRule};

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// A refusal of the reader's, as a stream that is no raw DEFLATE gives one.
    fn refused_stream() -> MediaError {
        inflate(b"garbage", 4, Trailer::None).unwrap_err()
    }

    #[test]
    fn every_failure_names_its_rule() {
        let staged = Path::new("stage-n64-AbC123xy")
            .join("n64")
            .join("texture.bin");
        let failures = [
            // The reader's rule, whatever the failure: a refusal by a rule of the format, a failed
            // read and a read asked to stop.
            (ExtractError::from(refused_stream()), "deflate"),
            (
                ExtractError::from(MediaError::Io {
                    path: staged.clone(),
                    source: io::ErrorKind::NotFound.into(),
                }),
                "io",
            ),
            (ExtractError::from(MediaError::Cancelled), "cancelled"),
            (
                ExtractError::from(MediaError::Write {
                    file: RelPath::parse("DISK.ROM", &DISC_NAMES).unwrap(),
                    source: io::ErrorKind::StorageFull.into(),
                }),
                "io",
            ),
            // The staging's, for a name taken and for a link on the way.
            (
                ExtractError::from(FilesError::Io {
                    path: staged.clone(),
                    source: io::ErrorKind::AlreadyExists.into(),
                }),
                "io",
            ),
            (
                ExtractError::from(FilesError::Link {
                    path: staged.clone(),
                }),
                "link",
            ),
            (
                ExtractError::from(FilesError::Exists {
                    path: staged.clone(),
                }),
                "exists",
            ),
            // The extraction's own, a full disk among them.
            (
                ExtractError::Io {
                    path: staged.clone(),
                    source: io::ErrorKind::StorageFull.into(),
                },
                "io",
            ),
            (
                ExtractError::tree(TreeRule::Duplicate, "duplicate"),
                "treeDuplicate",
            ),
            (
                ExtractError::tree(TreeRule::FolderCase, "folder"),
                "treeFolderCase",
            ),
            (
                ExtractError::tree(TreeRule::Conflict, "conflict"),
                "treeConflict",
            ),
            (
                ExtractError::native(NativeRule::TableMissing, "missing"),
                "nativeTableMissing",
            ),
            (
                ExtractError::native(NativeRule::TooLarge, "large"),
                "sizesTooLarge",
            ),
            (
                ExtractError::native(NativeRule::DiskTruncated, "truncated"),
                "diskTruncated",
            ),
            (
                ExtractError::Changed {
                    path: "disc/SIZES.ROM".into(),
                },
                "changed",
            ),
            // The refusal's, for a dump an installation's trust does not name.
            (
                ExtractError::Refused {
                    refusal: DumpRefusal::OtherGame,
                },
                "otherGame",
            ),
            (
                ExtractError::Refused {
                    refusal: DumpRefusal::OtherRevision,
                },
                "otherRevision",
            ),
            (
                ExtractError::Refused {
                    refusal: DumpRefusal::HashMismatch,
                },
                "hashMismatch",
            ),
            (
                ExtractError::fingerprint(FingerprintRule::Missing, "disc/DISK.ROM"),
                "fingerprintMissing",
            ),
            (
                ExtractError::fingerprint(FingerprintRule::Unexpected, "e60b7f33"),
                "fingerprintUnexpected",
            ),
            (
                ExtractError::fingerprint(FingerprintRule::Digest, "IP.BIN"),
                "fingerprintDigest",
            ),
            (ExtractError::from(Cancelled), "cancelled"),
        ];
        for (failure, rule) in failures {
            assert_eq!(failure.rule(), rule, "{failure:?}");
        }
        // A job's cancel is the extraction's own stop, whichever part heeds it: the extraction's
        // or a reader's.
        assert!(matches!(
            ExtractError::from(Cancelled),
            ExtractError::Cancelled
        ));
        assert!(matches!(
            ExtractError::from(MediaError::Cancelled),
            ExtractError::Cancelled
        ));
        // Every other failure of a reader's stays the reader's.
        assert!(matches!(
            ExtractError::from(refused_stream()),
            ExtractError::Media(MediaError::Rom { .. })
        ));
    }

    #[test]
    fn errors_say_what_failed() {
        // The reader's and the staging's failures read as their own words.
        let refused = refused_stream();
        let words = refused.to_string();
        assert_eq!(ExtractError::from(refused).to_string(), words);
        let link = FilesError::Link {
            path: Path::new("stage").join("n64"),
        };
        let words = link.to_string();
        let error = ExtractError::from(link);
        assert_eq!(error.to_string(), words);
        assert!(error.source().is_none());

        // A failed write names the staged file and says what the system answered.
        let staged = Path::new("stage").join("n64").join("manifest.json");
        let error = ExtractError::Io {
            path: staged.clone(),
            source: io::Error::new(io::ErrorKind::StorageFull, "no room"),
        };
        assert_eq!(error.to_string(), format!("{}: no room", staged.display()));
        assert_eq!(error.source().unwrap().to_string(), "no room");

        // A tree's refusal is its detail alone, and so is a native table's; a dump that changed
        // names the file of the tree it changed in, and a stop says so.
        let error = ExtractError::tree(TreeRule::FolderCase, "a folder is spelled Font and font");
        assert_eq!(error.to_string(), "a folder is spelled Font and font");
        assert!(error.source().is_none());
        let error = ExtractError::native(NativeRule::DiskTruncated, "Truncated DISK.ROM payload");
        assert_eq!(error.to_string(), "Truncated DISK.ROM payload");
        assert!(error.source().is_none());
        let error = ExtractError::Changed {
            path: "disc/SIZES.ROM".into(),
        };
        assert_eq!(
            error.to_string(),
            "the dump changed while it was read: disc/SIZES.ROM"
        );
        assert!(error.source().is_none());
        assert_eq!(ExtractError::Cancelled.to_string(), "cancelled");

        // A dump an installation's trust does not name says why; a file that does not match its
        // fingerprint is named by its path in the tree, with the rule it breaks.
        let error = ExtractError::Refused {
            refusal: DumpRefusal::HashMismatch,
        };
        assert_eq!(
            error.to_string(),
            "the dump is refused for an installation: hashMismatch"
        );
        assert!(error.source().is_none());
        let error = ExtractError::fingerprint(FingerprintRule::Digest, "disc/AUDIO/X.STR");
        assert_eq!(error.to_string(), "disc/AUDIO/X.STR: fingerprintDigest");
        assert!(error.source().is_none());

        // Every failure is cheap to return, whatever it carries.
        assert!(size_of::<ExtractError>() <= MOST_ERROR_BYTES);
    }

    #[test]
    fn tree_rules_have_stable_names() {
        let rules = [
            (TreeRule::Duplicate, "treeDuplicate"),
            (TreeRule::FolderCase, "treeFolderCase"),
            (TreeRule::Conflict, "treeConflict"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(ExtractError::tree(rule, "").rule(), name);
        }
    }

    #[test]
    fn native_rules_have_stable_names() {
        let rules = [
            (NativeRule::TableMissing, "nativeTableMissing"),
            (NativeRule::TooLarge, "sizesTooLarge"),
            (NativeRule::DiskTruncated, "diskTruncated"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(ExtractError::native(rule, "").rule(), name);
        }
    }

    #[test]
    fn fingerprint_rules_have_stable_names() {
        let rules = [
            (FingerprintRule::Missing, "fingerprintMissing"),
            (FingerprintRule::Unexpected, "fingerprintUnexpected"),
            (FingerprintRule::Digest, "fingerprintDigest"),
        ];
        for (rule, name) in rules {
            assert_eq!(rule.as_str(), name);
            assert_eq!(ExtractError::fingerprint(rule, "IP.BIN").rule(), name);
        }
    }
}
