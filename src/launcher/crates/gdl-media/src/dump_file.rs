//! Opening a file of the player's dump to read: only a regular file, and only once what is at its
//! path has been seen to be one. A plain open of a FIFO waits for a writer, beyond the reach of a
//! job's cancel, and an open of a device may do more than give bytes, so neither is ever opened.

use std::fs::{self, File};
use std::io;
use std::path::Path;

/// The file at `path` open to read, and its length, or `None`, unopened, when what is there is no
/// regular file: a folder, a FIFO, a socket or a device. A link is followed, as the player may
/// reach a dump through one. The opened file is looked at again, so that what took the name since
/// is not read either; only a FIFO put there in between could still hold the open. Fails as the
/// system answers, with [`io::ErrorKind::NotFound`] when nothing is there.
pub(crate) fn open_regular(path: &Path) -> io::Result<Option<(File, u64)>> {
    open_regular_within(path, u64::MAX)
}

/// The file at `path` open to read, and its length, as [`open_regular`] opens it, when it is a
/// regular file of at most `most` bytes, or `None`, unopened, when it is no regular file or is
/// longer: a long file is passed over by its length alone, so that one another program holds
/// open, sharing no read of it, fails nothing that only looks for short files.
pub(crate) fn open_regular_within(path: &Path, most: u64) -> io::Result<Option<(File, u64)>> {
    let metadata = fs::metadata(path)?;
    if !metadata.is_file() || metadata.len() > most {
        return Ok(None);
    }
    let file = File::open(path)?;
    let metadata = file.metadata()?;
    let within = metadata.is_file() && metadata.len() <= most;
    Ok(within.then_some((file, metadata.len())))
}

/// What the readers' tests make and wait on to show that no read of a dump waits on a FIFO.
#[cfg(all(test, unix))]
pub(crate) mod fifo {
    use std::fs::OpenOptions;
    use std::path::Path;
    use std::process::Command;
    use std::sync::mpsc;
    use std::thread;
    use std::time::Duration;

    /// Far longer than any refusal takes, and short enough for a test to fail in.
    const MOST_WAIT: Duration = Duration::from_secs(10);

    /// Makes a FIFO at `path`, as the system's `mkfifo` makes one.
    pub(crate) fn make(path: &Path) {
        let made = Command::new("mkfifo").arg(path).status().unwrap();
        assert!(made.success(), "mkfifo: {made}");
    }

    /// What `read` gives, run on a thread of its own. Panics when it has given nothing within
    /// [`MOST_WAIT`], as a read that waits on the FIFO at `fifo` for a writer does, once a writer
    /// has opened the FIFO and let the waiting read go, so that the test ends.
    pub(crate) fn without_waiting<T: Send + 'static>(
        fifo: &Path,
        read: impl FnOnce() -> T + Send + 'static,
    ) -> T {
        let (told, answer) = mpsc::channel();
        let reader = thread::spawn(move || {
            let _ = told.send(read());
        });
        match answer.recv_timeout(MOST_WAIT) {
            Ok(given) => {
                reader.join().unwrap();
                given
            }
            Err(_) => {
                let _ = OpenOptions::new().write(true).open(fifo);
                panic!("a read of the FIFO {} waited for a writer", fifo.display());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io::{ErrorKind, Read};

    use super::open_regular;

    #[test]
    fn only_a_regular_file_is_opened() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("dump.bin");
        fs::write(&path, b"dump").unwrap();
        let (mut file, len) = open_regular(&path).unwrap().unwrap();
        assert_eq!(len, 4);
        let mut bytes = Vec::new();
        file.read_to_end(&mut bytes).unwrap();
        assert_eq!(bytes, b"dump");
        // A folder is never opened, on either system.
        assert!(open_regular(dir.path()).unwrap().is_none());
        // Nothing there is what the system answers.
        let error = open_regular(&dir.path().join("missing.bin")).unwrap_err();
        assert_eq!(error.kind(), ErrorKind::NotFound);
        assert_eq!(fs::read(&path).unwrap(), b"dump");
    }

    #[cfg(unix)]
    #[test]
    fn a_fifo_a_device_or_a_link_to_either_is_never_opened() {
        use std::os::unix::fs::symlink;

        use super::fifo;

        let dir = tempfile::tempdir().unwrap();
        // A FIFO, which a plain open waits on until a writer comes, is passed over at once, and so
        // is a link to one.
        let fifo_path = dir.path().join("track.bin");
        fifo::make(&fifo_path);
        let linked_fifo = dir.path().join("linked.bin");
        symlink(&fifo_path, &linked_fifo).unwrap();
        for reading in [fifo_path.clone(), linked_fifo] {
            let opened = fifo::without_waiting(&fifo_path, move || {
                open_regular(&reading).map(|opened| opened.is_some())
            });
            assert!(!opened.unwrap());
        }
        // So is a device, which gives bytes without end.
        let device = dir.path().join("device.bin");
        symlink("/dev/zero", &device).unwrap();
        assert!(open_regular(&device).unwrap().is_none());
        // A link to a file is followed.
        let file = dir.path().join("file.bin");
        fs::write(&file, b"dump").unwrap();
        let linked_file = dir.path().join("to-file.bin");
        symlink(&file, &linked_file).unwrap();
        let (_, len) = open_regular(&linked_file).unwrap().unwrap();
        assert_eq!(len, 4);
    }
}
