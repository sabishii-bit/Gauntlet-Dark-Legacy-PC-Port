//! A runtime zip's end record and the rows of its central directory, read from the archive's own
//! bytes before the zip crate reads them. The zip crate reads every row an end record announces
//! before anything could bound how many there are, and keeps one entry of each name, so the end
//! record, every row and every name are held to a runtime's rules here first, and the zip crate
//! only has to agree with what was read.

use std::io::{Read, Seek, SeekFrom};

use gdl_files::{MAX_PAYLOAD_PATH_CHARS, NameError, NameRule, PAYLOAD_NAMES, RelPath};

use super::{MAX_PAYLOAD_ENTRIES, PayloadEntry, PayloadError};

/// The signature that begins the end record.
const EOCD_SIGNATURE: u32 = 0x0605_4b50;

/// The end record's length without a comment. A runtime's end record has none, so these are the
/// archive's last bytes.
const EOCD_LEN: usize = 22;

/// The signature that begins each row of the central directory.
const CENTRAL_SIGNATURE: u32 = 0x0201_4b50;

/// The length of a row's fixed fields, which its name, extra field and comment follow.
const CENTRAL_ROW_LEN: usize = 46;

/// The signature of a ZIP64 end-of-central-directory locator, which lies just before the end
/// record of an archive that has one.
const ZIP64_LOCATOR_SIGNATURE: u32 = 0x0706_4b50;

/// A ZIP64 locator's length.
const ZIP64_LOCATOR_LEN: usize = 20;

/// What a 16-bit field of the end record holds when its value lies in a ZIP64 record instead.
const ZIP64_SHORT: u16 = 0xFFFF;

/// What a 32-bit field of the end record holds when its value lies in a ZIP64 record instead.
const ZIP64_LONG: u32 = 0xFFFF_FFFF;

/// The flag of an encrypted entry.
const FLAG_ENCRYPTED: u16 = 0x0001;

/// The flag of an entry whose sizes and checksum follow its data instead of lying in its local
/// header.
const FLAG_DATA_DESCRIPTOR: u16 = 0x0008;

/// The flag of an entry under strong encryption.
const FLAG_STRONG_ENCRYPTION: u16 = 0x0040;

/// The flag of an archive whose local headers are masked, their true values kept encrypted in the
/// central directory.
const FLAG_MASKED_HEADERS: u16 = 0x2000;

/// The method of an entry held as it is.
const METHOD_STORED: u16 = 0;

/// The method of a deflated entry, the one release.py writes.
const METHOD_DEFLATED: u16 = 8;

/// The DOS attribute of a folder, in the low half of a row's external attributes.
const DOS_DIRECTORY: u32 = 0x10;

/// How far a Unix mode is shifted into the high half of a row's external attributes.
const UNIX_MODE_SHIFT: u32 = 16;

/// The bits of a Unix mode that give the type of file.
const S_IFMT: u32 = 0o170000;

/// The type bits of a regular file.
const S_IFREG: u32 = 0o100000;

/// The most bytes a runtime's central directory may take: as many rows as a runtime may have,
/// each with the longest name a runtime's entry may have.
const MAX_CENTRAL_DIRECTORY_LEN: usize =
    MAX_PAYLOAD_ENTRIES * (CENTRAL_ROW_LEN + MAX_PAYLOAD_PATH_CHARS);

/// Reads the end record and the central directory of the archive in `reader`, and gives back the
/// entries the rows declare, in their order, once the end record, every row and every name keep to
/// a runtime's rules. The first rule broken is reported.
pub(super) fn read_entries<R: Read + Seek>(
    reader: &mut R,
) -> Result<Vec<PayloadEntry>, PayloadError> {
    let archive_len = reader.seek(SeekFrom::End(0))?;
    let end = read_end_record(reader, archive_len)?;
    let rows = read_rows(reader, &end)?;
    let first_header = rows.iter().map(|row| row.header_offset).min().unwrap_or(0);
    rows.into_iter()
        .enumerate()
        .map(|(index, row)| row.into_entry(index, first_header))
        .collect()
}

/// What a runtime's end record says of its central directory.
struct EndRecord {
    /// How many rows the central directory holds.
    count: u16,
    /// Where the central directory starts.
    offset: u32,
    /// How many bytes the central directory takes.
    len: usize,
}

/// Reads the end record, which must be the archive's last bytes, and holds it to a runtime's
/// rules: no comment, no ZIP64, a single disk, at most [`MAX_PAYLOAD_ENTRIES`] rows, and a central
/// directory no longer than they could take that ends where the end record begins. The count is
/// bounded before a row is read.
fn read_end_record<R: Read + Seek>(
    reader: &mut R,
    archive_len: u64,
) -> Result<EndRecord, PayloadError> {
    let start = archive_len
        .checked_sub(EOCD_LEN as u64)
        .ok_or(PayloadError::Archive)?;
    let mut record = [0; EOCD_LEN];
    reader.seek(SeekFrom::Start(start))?;
    reader.read_exact(&mut record)?;
    let mut fields = Fields::new(&record);
    if fields.u32() != EOCD_SIGNATURE {
        return Err(PayloadError::Archive);
    }
    let disk = fields.u16();
    let directory_disk = fields.u16();
    let disk_count = fields.u16();
    let count = fields.u16();
    let len = fields.u32();
    let offset = fields.u32();
    // An end record with a comment is not the archive's last bytes.
    if fields.u16() != 0 {
        return Err(PayloadError::Archive);
    }
    if [disk, directory_disk, disk_count, count].contains(&ZIP64_SHORT)
        || len == ZIP64_LONG
        || offset == ZIP64_LONG
        || has_zip64_locator(reader, start)?
    {
        return Err(PayloadError::Zip64);
    }
    if disk != 0 || directory_disk != 0 {
        return Err(damaged(format!(
            "the end record lies on disk {disk} and its central directory on disk \
             {directory_disk}"
        )));
    }
    if disk_count != count {
        return Err(damaged(format!(
            "the end record counts {disk_count} rows on its disk and {count} in all"
        )));
    }
    if usize::from(count) > MAX_PAYLOAD_ENTRIES {
        return Err(PayloadError::TooManyEntries {
            count: usize::from(count),
        });
    }
    let Some(len) = usize::try_from(len)
        .ok()
        .filter(|&bytes| bytes <= MAX_CENTRAL_DIRECTORY_LEN)
    else {
        return Err(damaged(format!(
            "the central directory takes {len} bytes, more than {MAX_CENTRAL_DIRECTORY_LEN}"
        )));
    };
    if u64::from(offset) + len as u64 != start {
        return Err(damaged(format!(
            "the central directory at byte {offset} takes {len} bytes, so it does not end where \
             the end record begins, at byte {start}"
        )));
    }
    Ok(EndRecord { count, offset, len })
}

/// Whether a ZIP64 locator's signature lies just before the end record, which starts at
/// `end_record`.
fn has_zip64_locator<R: Read + Seek>(
    reader: &mut R,
    end_record: u64,
) -> Result<bool, PayloadError> {
    let Some(start) = end_record.checked_sub(ZIP64_LOCATOR_LEN as u64) else {
        return Ok(false);
    };
    let mut signature = [0; 4];
    reader.seek(SeekFrom::Start(start))?;
    reader.read_exact(&mut signature)?;
    Ok(u32::from_le_bytes(signature) == ZIP64_LOCATOR_SIGNATURE)
}

/// One row of the central directory: what it declares of an entry.
struct Row {
    /// The entry's name, as bytes.
    name: Vec<u8>,
    /// The entry's general-purpose flags.
    flags: u16,
    /// The entry's compression method.
    method: u16,
    /// How many bytes the archive holds of the entry.
    compressed_size: u32,
    /// How many bytes the entry has uncompressed.
    size: u32,
    /// The entry's external attributes: a Unix mode in the high half and DOS attributes in the
    /// low.
    external_attributes: u32,
    /// Where the entry's local header starts.
    header_offset: u32,
}

/// Reads the central directory, which must hold exactly the rows the end record counts, back to
/// back, each with its signature, with neither an extra field nor a comment, and on the one disk.
fn read_rows<R: Read + Seek>(reader: &mut R, end: &EndRecord) -> Result<Vec<Row>, PayloadError> {
    let mut directory = vec![0; end.len];
    reader.seek(SeekFrom::Start(u64::from(end.offset)))?;
    reader.read_exact(&mut directory)?;
    let count = usize::from(end.count);
    let mut rows = Vec::with_capacity(count);
    let mut rest = directory.as_slice();
    while !rest.is_empty() {
        let at = u64::from(end.offset) + (end.len - rest.len()) as u64;
        // Stopping at the rows counted keeps a directory padded past them from being parsed.
        if rows.len() == count {
            return Err(damaged(format!(
                "the central directory goes on at byte {at}, past the {count} rows its end \
                 record counts"
            )));
        }
        let Some((fixed, after)) = rest.split_first_chunk::<CENTRAL_ROW_LEN>() else {
            return Err(damaged(format!(
                "the central directory ends inside the row at byte {at}"
            )));
        };
        let mut fields = Fields::new(fixed);
        if fields.u32() != CENTRAL_SIGNATURE {
            return Err(damaged(format!("no row begins at byte {at}")));
        }
        // The system and version that made the entry, and the version that extracts it.
        fields.skip(4);
        let flags = fields.u16();
        let method = fields.u16();
        // The entry's time, date and CRC-32.
        fields.skip(8);
        let compressed_size = fields.u32();
        let size = fields.u32();
        let name_len = usize::from(fields.u16());
        let extra_len = usize::from(fields.u16());
        let comment_len = usize::from(fields.u16());
        let disk = fields.u16();
        // The entry's internal attributes.
        fields.skip(2);
        let external_attributes = fields.u32();
        let header_offset = fields.u32();
        let Some((variable, after)) = after.split_at_checked(name_len + extra_len + comment_len)
        else {
            return Err(damaged(format!(
                "the row at byte {at} runs past the central directory"
            )));
        };
        if extra_len != 0 || comment_len != 0 {
            return Err(damaged(format!(
                "the row at byte {at} has an extra field of {extra_len} bytes and a comment of \
                 {comment_len}"
            )));
        }
        if disk != 0 {
            return Err(damaged(format!(
                "the row at byte {at} starts its entry on disk {disk}"
            )));
        }
        rows.push(Row {
            name: variable[..name_len].to_vec(),
            flags,
            method,
            compressed_size,
            size,
            external_attributes,
            header_offset,
        });
        rest = after;
    }
    if rows.len() != count {
        return Err(damaged(format!(
            "the end record counts {count} rows, and the central directory holds {}",
            rows.len()
        )));
    }
    Ok(rows)
}

impl Row {
    /// The row's name as text, whatever bytes it holds, for reports.
    fn shown_name(&self) -> Box<str> {
        String::from_utf8_lossy(&self.name).into()
    }

    /// Holds the row to a runtime's rules and gives back the entry it declares, the archive's
    /// `index`th. `first_header` is the lowest offset of any row's local header.
    fn into_entry(self, index: usize, first_header: u32) -> Result<PayloadEntry, PayloadError> {
        let encryption = FLAG_ENCRYPTED | FLAG_STRONG_ENCRYPTION | FLAG_MASKED_HEADERS;
        if self.flags & encryption != 0 {
            return Err(PayloadError::Encrypted {
                name: self.shown_name(),
            });
        }
        if self.flags & FLAG_DATA_DESCRIPTOR != 0 {
            return Err(PayloadError::DataDescriptor {
                name: self.shown_name(),
            });
        }
        if self.method != METHOD_STORED && self.method != METHOD_DEFLATED {
            return Err(PayloadError::Method {
                name: self.shown_name(),
                method: self.method,
            });
        }
        if self.method == METHOD_STORED && self.compressed_size != self.size {
            return Err(damaged(format!(
                "the stored entry {:?} holds {} bytes and declares {}",
                self.shown_name(),
                self.compressed_size,
                self.size
            )));
        }
        // The local header that comes first must open the archive: bytes before it are data
        // prepended to the zip, which no runtime carries.
        if self.header_offset == first_header && first_header != 0 {
            return Err(damaged(format!(
                "{first_header} bytes come before the first local header"
            )));
        }
        if self.name.ends_with(b"/") || self.external_attributes & DOS_DIRECTORY != 0 {
            return Err(PayloadError::Directory {
                name: self.shown_name(),
            });
        }
        let file_type = (self.external_attributes >> UNIX_MODE_SHIFT) & S_IFMT;
        if file_type != 0 && file_type != S_IFREG {
            return Err(PayloadError::Link {
                name: self.shown_name(),
            });
        }
        let Ok(text) = std::str::from_utf8(&self.name) else {
            return Err(PayloadError::Name {
                error: NameError {
                    name: self.shown_name(),
                    rule: NameRule::NonAscii,
                },
            });
        };
        let name =
            RelPath::parse(text, &PAYLOAD_NAMES).map_err(|error| PayloadError::Name { error })?;
        Ok(PayloadEntry {
            name,
            size: u64::from(self.size),
            index,
        })
    }
}

/// The refusal of a central directory or end record that a runtime's could not be, for the reason
/// `detail` gives.
fn damaged(detail: String) -> PayloadError {
    PayloadError::Rows {
        detail: detail.into(),
    }
}

/// The little-endian fields of a record of a known length, read one after another.
struct Fields<'a> {
    /// What is left of the record.
    rest: &'a [u8],
}

impl<'a> Fields<'a> {
    fn new(record: &'a [u8]) -> Self {
        Fields { rest: record }
    }

    /// Takes the next `N` bytes.
    fn take<const N: usize>(&mut self) -> [u8; N] {
        let (field, rest) = self
            .rest
            .split_first_chunk::<N>()
            .expect("every field lies within its record");
        self.rest = rest;
        *field
    }

    /// Passes over fields not needed.
    fn skip(&mut self, len: usize) {
        self.rest = self
            .rest
            .get(len..)
            .expect("every field lies within its record");
    }

    fn u16(&mut self) -> u16 {
        u16::from_le_bytes(self.take())
    }

    fn u32(&mut self) -> u32 {
        u32::from_le_bytes(self.take())
    }
}

#[cfg(test)]
mod tests {
    use std::io::{self, Cursor, Read, Seek, SeekFrom};

    use gdl_files::NameRule;
    use gdl_fixtures::{RawEntry, RawZip};

    use super::{
        CENTRAL_ROW_LEN, EOCD_LEN, EOCD_SIGNATURE, MAX_CENTRAL_DIRECTORY_LEN, ZIP64_LOCATOR_LEN,
        ZIP64_LOCATOR_SIGNATURE, read_entries,
    };
    use crate::payload::{MAX_PAYLOAD_ENTRIES, PayloadError};

    /// A reader that counts the bytes read through it.
    struct Counting {
        archive: Cursor<Vec<u8>>,
        read: u64,
    }

    impl Counting {
        fn new(archive: Vec<u8>) -> Self {
            Counting {
                archive: Cursor::new(archive),
                read: 0,
            }
        }
    }

    impl Read for Counting {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            let read = self.archive.read(buffer)?;
            self.read += read as u64;
            Ok(read)
        }
    }

    impl Seek for Counting {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.archive.seek(to)
        }
    }

    /// An archive of a central directory of `len` zero bytes from its first byte, then an end
    /// record that counts `count` rows in it.
    fn zeroed_directory(count: u16, len: usize) -> Vec<u8> {
        let mut archive = vec![0; len];
        archive.extend_from_slice(&EOCD_SIGNATURE.to_le_bytes());
        // This disk, and the disk of the central directory.
        archive.extend_from_slice(&[0; 4]);
        archive.extend_from_slice(&count.to_le_bytes());
        archive.extend_from_slice(&count.to_le_bytes());
        archive.extend_from_slice(&u32::try_from(len).unwrap().to_le_bytes());
        // Where the central directory starts, and the comment's length.
        archive.extend_from_slice(&[0; 6]);
        assert_eq!(archive.len(), len + EOCD_LEN);
        archive
    }

    #[test]
    fn the_bounds_hold_before_a_row_is_read() {
        // What is read before the central directory: the end record, and the signature of a ZIP64
        // locator, were one to lie before it.
        let tail = (EOCD_LEN + size_of::<u32>()) as u64;
        assert_eq!(MAX_CENTRAL_DIRECTORY_LEN, 20_000 * (46 + 1024));

        // More rows than a runtime may have are refused from the end record alone, and as many
        // as it may have are read and checked.
        let over = u16::try_from(MAX_PAYLOAD_ENTRIES + 1).unwrap();
        let mut reader = Counting::new(zeroed_directory(over, CENTRAL_ROW_LEN));
        match read_entries(&mut reader) {
            Err(PayloadError::TooManyEntries { count }) => assert_eq!(count, usize::from(over)),
            other => panic!("{:?}", other.map(|entries| entries.len())),
        }
        assert!(reader.read <= tail, "{} bytes read", reader.read);
        let most = u16::try_from(MAX_PAYLOAD_ENTRIES).unwrap();
        let mut reader = Counting::new(zeroed_directory(most, CENTRAL_ROW_LEN));
        let error = read_entries(&mut reader).unwrap_err();
        assert_eq!(error.rule(), "rows", "{error}");
        assert!(reader.read > tail, "{} bytes read", reader.read);

        // So is a central directory longer than such rows could take, and one exactly that long
        // is read whole.
        let mut reader = Counting::new(zeroed_directory(1, MAX_CENTRAL_DIRECTORY_LEN + 1));
        let error = read_entries(&mut reader).unwrap_err();
        assert_eq!(error.rule(), "rows", "{error}");
        assert!(reader.read <= tail, "{} bytes read", reader.read);
        let mut reader = Counting::new(zeroed_directory(1, MAX_CENTRAL_DIRECTORY_LEN));
        let error = read_entries(&mut reader).unwrap_err();
        assert_eq!(error.rule(), "rows", "{error}");
        assert!(
            reader.read >= MAX_CENTRAL_DIRECTORY_LEN as u64,
            "{} bytes read",
            reader.read
        );
    }

    #[test]
    fn names_are_held_to_the_payload_name_rules() {
        for (name, refused) in [
            (
                &b"shaders/\xfftest.spv"[..],
                ("shaders/\u{fffd}test.spv", NameRule::NonAscii),
            ),
            (
                b"shaders/t\xc3\xa9st.spv",
                ("shaders/t\u{e9}st.spv", NameRule::NonAscii),
            ),
            (b"data/../escaped", ("data/../escaped", NameRule::Dot)),
            (b"data//config.json", ("data//config.json", NameRule::Empty)),
            (b"data/aux.json", ("data/aux.json", NameRule::DeviceName)),
            (
                b"lib/.gdl-update.lock",
                ("lib/.gdl-update.lock", NameRule::AlphaName),
            ),
        ] {
            let mut entry = RawEntry::stored("x", b"");
            entry.name = name.to_vec();
            let zip = RawZip {
                entries: vec![entry],
                ..RawZip::default()
            };
            match read_entries(&mut Cursor::new(zip.to_bytes())) {
                Err(PayloadError::Name { error }) => {
                    let (shown, rule) = refused;
                    assert_eq!((&*error.name, error.rule), (shown, rule));
                }
                other => panic!("{refused:?}: {:?}", other.map(|entries| entries.len())),
            }
        }
    }

    #[test]
    fn an_archive_of_no_entries_has_no_rows() {
        let mut reader = Counting::new(zeroed_directory(0, 0));
        assert!(read_entries(&mut reader).unwrap().is_empty());
        // Its end record is the whole archive, with no room for a ZIP64 locator before it.
        assert_eq!(reader.read, EOCD_LEN as u64);
        // An end record with room for a locator before it: zeros there are no locator, and a
        // locator's signature is.
        let mut archive = vec![0; ZIP64_LOCATOR_LEN];
        archive.extend(zeroed_directory(0, 0));
        // The end record's offset of the central directory, now where the record itself starts.
        let offset_field = ZIP64_LOCATOR_LEN + 16;
        let offset = u32::try_from(ZIP64_LOCATOR_LEN).unwrap();
        archive[offset_field..offset_field + 4].copy_from_slice(&offset.to_le_bytes());
        assert!(
            read_entries(&mut Cursor::new(archive.clone()))
                .unwrap()
                .is_empty()
        );
        archive[..4].copy_from_slice(&ZIP64_LOCATOR_SIGNATURE.to_le_bytes());
        let error = read_entries(&mut Cursor::new(archive)).unwrap_err();
        assert_eq!(error.rule(), "zip64", "{error}");
    }
}
