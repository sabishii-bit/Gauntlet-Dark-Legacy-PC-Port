//! One section of a launcher file read as a file of its own, so that a reader of what it holds,
//! such as the zip crate, which looks for a zip's end record at the end of what it reads, finds
//! the section's end where the section ends, not at the end of the launcher file.

use std::io::{self, Read, Seek, SeekFrom};

/// One section of a launcher file, read and sought within the section's bounds as a file of its
/// own: its first byte is at position 0, and it ends where the section ends, whatever follows the
/// section in the launcher file. Each read seeks the launcher file first, so whatever else has
/// read it in between does not matter.
#[derive(Debug)]
pub struct SectionView<'a, R> {
    /// The launcher file.
    source: &'a mut R,
    /// Where the section begins in the launcher file.
    start: u64,
    /// How many bytes the section holds.
    len: u64,
    /// Where the next read begins, from the section's start; past its end, reads give nothing.
    position: u64,
}

impl<'a, R> SectionView<'a, R> {
    /// The `len` bytes of `source` from `start`, read from their first.
    pub(super) fn new(source: &'a mut R, start: u64, len: u64) -> Self {
        SectionView {
            source,
            start,
            len,
            position: 0,
        }
    }
}

impl<R: Read + Seek> Read for SectionView<'_, R> {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        let left = self.len.saturating_sub(self.position);
        let wanted = buffer
            .len()
            .min(usize::try_from(left).unwrap_or(usize::MAX));
        if wanted == 0 {
            return Ok(0);
        }
        self.source
            .seek(SeekFrom::Start(self.start + self.position))?;
        let read = self.source.read(&mut buffer[..wanted])?;
        self.position += read as u64;
        Ok(read)
    }
}

impl<R> Seek for SectionView<'_, R> {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let position = match to {
            SeekFrom::Start(position) => Some(position),
            SeekFrom::End(delta) => self.len.checked_add_signed(delta),
            SeekFrom::Current(delta) => self.position.checked_add_signed(delta),
        };
        self.position = position.ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "a seek before the start of the section or past 2^64 bytes",
            )
        })?;
        Ok(self.position)
    }
}

#[cfg(test)]
mod tests {
    use std::io::{self, Cursor, Read, Seek, SeekFrom};

    use super::SectionView;

    /// A launcher file of ten bytes, whose section is the four from 3: `3456`.
    fn file() -> Cursor<Vec<u8>> {
        Cursor::new(b"0123456789".to_vec())
    }

    #[test]
    fn a_view_reads_its_section_alone() {
        let mut file = file();
        file.seek(SeekFrom::Start(9)).unwrap();
        let mut view = SectionView::new(&mut file, 3, 4);
        let mut whole = Vec::new();
        assert_eq!(view.read_to_end(&mut whole).unwrap(), 4);
        assert_eq!(whole, b"3456");
        // At its end there is nothing more to read, however much is asked for.
        let mut buffer = [0xEE; 8];
        assert_eq!(view.read(&mut buffer).unwrap(), 0);
        // A read past its end stops there; one of nothing reads nothing.
        view.seek(SeekFrom::Start(2)).unwrap();
        assert_eq!(view.read(&mut buffer).unwrap(), 2);
        assert_eq!(buffer[..3], *b"56\xEE");
        view.seek(SeekFrom::Start(0)).unwrap();
        assert_eq!(view.read(&mut []).unwrap(), 0);
        assert_eq!(view.stream_position().unwrap(), 0);
    }

    #[test]
    fn a_view_seeks_within_its_section() {
        let mut file = file();
        let mut view = SectionView::new(&mut file, 3, 4);
        let mut one = [0; 1];
        for (to, position, read) in [
            (SeekFrom::End(0), 4, None),
            (SeekFrom::End(-1), 3, Some(b'6')),
            (SeekFrom::Current(-3), 1, Some(b'4')),
            (SeekFrom::Current(0), 2, Some(b'5')),
            (SeekFrom::Start(0), 0, Some(b'3')),
            (SeekFrom::Start(100), 100, None),
            (SeekFrom::End(-4), 0, Some(b'3')),
        ] {
            assert_eq!(view.seek(to).unwrap(), position, "{to:?}");
            match read {
                Some(byte) => {
                    view.read_exact(&mut one).unwrap();
                    assert_eq!(one[0], byte, "{to:?}");
                }
                None => assert_eq!(view.read(&mut one).unwrap(), 0, "{to:?}"),
            }
        }
        // No seek goes before its start or past 2^64 bytes, and one refused moves nothing.
        for to in [
            SeekFrom::End(-5),
            SeekFrom::Current(-2),
            SeekFrom::Current(i64::MIN),
        ] {
            let error = view.seek(to).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{to:?}");
            assert_eq!(view.stream_position().unwrap(), 1, "{to:?}");
        }
        assert_eq!(view.seek(SeekFrom::Start(u64::MAX)).unwrap(), u64::MAX);
        assert_eq!(view.read(&mut one).unwrap(), 0);
        assert!(view.seek(SeekFrom::Current(1)).is_err());
    }

    #[test]
    fn whatever_moves_the_file_between_reads_does_not_matter() {
        let mut file = file();
        let mut first = [0; 2];
        {
            let mut view = SectionView::new(&mut file, 3, 4);
            view.read_exact(&mut first).unwrap();
        }
        assert_eq!(first, *b"34");
        // The file moved elsewhere, a new view of the same section reads from its own start, and
        // the file's own reads are its own again afterwards.
        file.seek(SeekFrom::Start(0)).unwrap();
        let mut view = SectionView::new(&mut file, 3, 4);
        view.seek(SeekFrom::Start(2)).unwrap();
        let mut rest = Vec::new();
        view.read_to_end(&mut rest).unwrap();
        assert_eq!(rest, b"56");
        // A section of nothing reads nothing, wherever it lies.
        let mut empty = SectionView::new(&mut file, 10, 0);
        assert_eq!(empty.read(&mut [0; 4]).unwrap(), 0);
        assert_eq!(empty.seek(SeekFrom::End(0)).unwrap(), 0);
    }

    /// A reader of a file whose reads fail once it is told to, which a view must pass on.
    struct Failing {
        file: Cursor<Vec<u8>>,
        failing: bool,
    }

    impl Read for Failing {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            if self.failing {
                return Err(io::Error::other("unreadable sector"));
            }
            self.file.read(buffer)
        }
    }

    impl Seek for Failing {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.file.seek(to)
        }
    }

    #[test]
    fn a_failed_read_moves_nothing() {
        let mut source = Failing {
            file: file(),
            failing: true,
        };
        let mut view = SectionView::new(&mut source, 3, 4);
        let error = view.read(&mut [0; 2]).unwrap_err();
        assert_eq!(error.to_string(), "unreadable sector");
        assert_eq!(view.stream_position().unwrap(), 0);
        view.source.failing = false;
        let mut two = [0; 2];
        view.read_exact(&mut two).unwrap();
        assert_eq!(two, *b"34");
    }
}
