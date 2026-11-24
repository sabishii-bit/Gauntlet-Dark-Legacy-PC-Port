//! A stand-in for a disk that fills up, which the tests copy into to see how a copy that cannot
//! be written fails.

use std::io::{self, Write};

/// A destination with room for `room` bytes, which then fails every write as a full disk does.
pub(crate) struct Full {
    /// How many more bytes it takes.
    pub(crate) room: usize,
    /// What it took.
    pub(crate) written: Vec<u8>,
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
