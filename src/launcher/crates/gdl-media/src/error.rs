//! What the readers report when a dump cannot be read: the path a read failed on and what the
//! system answered, or that the job reading the dump was asked to stop, as technical detail for
//! logs. What a player reads is chosen from the failure elsewhere.

use std::io;
use std::path::PathBuf;

use gdl_files::Cancelled;

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
}

impl MediaError {
    /// The failure's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        // Every variant is named, with no wildcard, so that a failure added later is given its
        // rule on purpose.
        match self {
            MediaError::Io { .. } => "io",
            MediaError::Cancelled => "cancelled",
        }
    }
}

impl From<Cancelled> for MediaError {
    /// A reader stops where its job's cancel tells it to, between two blocks of its work.
    fn from(_: Cancelled) -> MediaError {
        MediaError::Cancelled
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::io;
    use std::path::Path;

    use gdl_files::Cancel;

    use super::MediaError;

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
        let error = MediaError::Io {
            path: path.clone(),
            source: missing,
        };
        assert_eq!(error.to_string(), format!("{}: missing", path.display()));
        assert_eq!(error.source().unwrap().to_string(), "missing");
        assert!(matches!(&error, MediaError::Io { path: at, .. } if *at == path));

        assert_eq!(MediaError::Cancelled.to_string(), "cancelled");
        assert!(MediaError::Cancelled.source().is_none());

        assert!(size_of::<MediaError>() <= MOST_ERROR_BYTES);
    }

    #[test]
    fn every_failure_names_its_rule() {
        let path = Path::new("dumps").join("dump.z64");
        let failures = [
            // The rule is the variant's, whatever the system answered.
            (
                MediaError::Io {
                    path: path.clone(),
                    source: io::ErrorKind::PermissionDenied.into(),
                },
                "io",
            ),
            (
                MediaError::Io {
                    path,
                    source: io::ErrorKind::UnexpectedEof.into(),
                },
                "io",
            ),
            (MediaError::Cancelled, "cancelled"),
        ];
        for (failure, rule) in failures {
            assert_eq!(failure.rule(), rule, "{failure:?}");
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
