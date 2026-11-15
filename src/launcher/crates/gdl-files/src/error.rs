//! What the crate's file operations report when they fail: the path they failed on and why, as
//! technical detail for logs. What a player reads is chosen from the failure elsewhere.

use std::io;
use std::path::{Path, PathBuf};

/// Why a file operation failed, and on which path.
#[derive(Debug, thiserror::Error)]
pub enum FilesError {
    /// The system refused an operation on `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The path the operation was on.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// `path` is a link: a symbolic link, or on Windows a junction or a mount point. The launcher
    /// reaches the files it changes only through real folders.
    #[error("Choose a directory without links or junctions: {}", .path.display())]
    Link {
        /// The link, which is the path checked or one of the folders above it.
        path: PathBuf,
    },
    /// The file at `path` holds more than `limit` bytes.
    #[error("Metadata is too large: {}", .path.display())]
    TooLarge {
        /// The file read.
        path: PathBuf,
        /// The most bytes it may hold.
        limit: u64,
    },
    /// The file at `path` is not the JSON that was expected, or a value to be written there has
    /// no JSON form.
    #[error("{}: {source}", .path.display())]
    Json {
        /// The file read or written.
        path: PathBuf,
        /// What the JSON reader or writer found.
        #[source]
        source: serde_json::Error,
    },
    /// Something is at `path`, where a staged root was to go, though the job found the name free
    /// when it planned: the launcher never replaces what it did not make.
    #[error("Destination changed during installation: {}", .path.display())]
    Exists {
        /// The root's place.
        path: PathBuf,
    },
    /// `name` was given as a root to publish, but is not one name the staging holds at its top,
    /// or was given twice.
    #[error("{name:?} is not a single staged root")]
    NotRoot {
        /// The name as it was given.
        name: Box<str>,
    },
}

impl FilesError {
    /// The system's refusal of an operation on `path`.
    pub(crate) fn io(path: &Path, source: io::Error) -> FilesError {
        FilesError::Io {
            path: path.to_path_buf(),
            source,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::io;
    use std::path::Path;

    use super::FilesError;

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    #[test]
    fn errors_name_their_path_and_say_what_failed() {
        let path = Path::new(".gdl-launcher").join("receipt.json");
        let shown = path.display();

        let denied = io::Error::new(io::ErrorKind::PermissionDenied, "denied");
        let error = FilesError::io(&path, denied);
        assert_eq!(error.to_string(), format!("{shown}: denied"));
        assert!(matches!(&error, FilesError::Io { path: at, .. } if *at == path));
        assert_eq!(error.source().unwrap().to_string(), "denied");

        let error = FilesError::Link { path: path.clone() };
        assert_eq!(
            error.to_string(),
            format!("Choose a directory without links or junctions: {shown}")
        );
        assert!(error.source().is_none());

        let error = FilesError::TooLarge {
            path: path.clone(),
            limit: 64 << 10,
        };
        assert_eq!(error.to_string(), format!("Metadata is too large: {shown}"));
        assert!(error.source().is_none());

        let unfinished = serde_json::from_slice::<serde_json::Value>(b"{").unwrap_err();
        let detail = unfinished.to_string();
        let error = FilesError::Json {
            path: path.clone(),
            source: unfinished,
        };
        assert_eq!(error.to_string(), format!("{shown}: {detail}"));
        assert_eq!(error.source().unwrap().to_string(), detail);

        let error = FilesError::Exists { path: path.clone() };
        assert_eq!(
            error.to_string(),
            format!("Destination changed during installation: {shown}")
        );
        assert!(error.source().is_none());

        let error = FilesError::NotRoot {
            name: "data/config.json".into(),
        };
        assert_eq!(
            error.to_string(),
            r#""data/config.json" is not a single staged root"#
        );
        assert!(error.source().is_none());

        assert!(size_of::<FilesError>() <= MOST_ERROR_BYTES);
    }
}
