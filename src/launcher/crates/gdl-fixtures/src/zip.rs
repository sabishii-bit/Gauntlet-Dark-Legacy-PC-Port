//! ZIP archives written field by field, for the tests of the launcher's payload checks: a plain
//! archive that any reader takes, and archives with one field wrong that no ZIP writer would make,
//! such as two entries of one name, which the zip crate's own writer refuses.

use std::io::Write;

use flate2::Compression;
use flate2::write::DeflateEncoder;

/// The level release.py deflates a runtime's files at.
const DEFLATE_LEVEL: u32 = 6;

/// The compression method of an entry held as it is.
const METHOD_STORED: u16 = 0;

/// The compression method of a deflated entry.
const METHOD_DEFLATED: u16 = 8;

/// What made an entry, as Python's zipfile records itself on Linux: a Unix system, 3, in the high
/// byte, and ZIP 2.0, 20, in the low.
const MADE_BY_UNIX: u16 = 0x0314;

/// The ZIP version an entry needs to be extracted: 2.0, the first with deflate.
const VERSION_NEEDED: u16 = 20;

/// The external attributes of a regular file of mode 0o644, in their high half, where Unix systems
/// keep a file's mode.
const REGULAR_FILE: u32 = 0o100644 << 16;

/// The DOS time every entry carries: midnight.
const DOS_TIME: u16 = 0;

/// The DOS date every entry carries, so that the same entries always make the same bytes:
/// 1 January 1980, the first a DOS date holds, its day in the low five bits and its month in the
/// four above them.
const DOS_DATE: u16 = (1 << 5) | 1;

/// The number of the one disk a single-file archive lies on.
const DISK: u16 = 0;

/// How many disks a ZIP64 locator counts for a single-file archive.
const DISKS: u32 = 1;

/// An entry's internal attributes: no hint that it holds text.
const INTERNAL_ATTRIBUTES: u16 = 0;

/// The signature that begins each entry's local header.
const LOCAL_HEADER_SIGNATURE: u32 = 0x0403_4b50;

/// The signature that begins each row of the central directory.
const CENTRAL_ROW_SIGNATURE: u32 = 0x0201_4b50;

/// The signature of a ZIP64 end-of-central-directory locator.
const ZIP64_LOCATOR_SIGNATURE: u32 = 0x0706_4b50;

/// The signature of the end record, the last fixed part of an archive.
const END_RECORD_SIGNATURE: u32 = 0x0605_4b50;

/// The CRC-32 polynomial of IEEE 802.3, which ZIP uses, with its bits reversed, since the checksum
/// takes each byte lowest bit first.
const CRC32_POLYNOMIAL: u32 = 0xEDB8_8320;

/// The CRC-32 step of each byte value, so that the checksum takes a byte at a time.
const CRC32_TABLE: [u32; 256] = crc32_table();

/// The checksum a ZIP entry declares for what it holds: the CRC-32 of IEEE 802.3, as zlib computes
/// it.
pub fn crc32(bytes: &[u8]) -> u32 {
    !bytes.iter().fold(!0, |crc, &byte| {
        CRC32_TABLE[usize::from((crc as u8) ^ byte)] ^ (crc >> 8)
    })
}

/// Builds [`CRC32_TABLE`], a bit at a time.
const fn crc32_table() -> [u32; 256] {
    let mut table = [0; 256];
    let mut value = 0;
    while value < table.len() {
        let mut crc = value as u32;
        let mut bit = 0;
        while bit < u8::BITS {
            crc = if crc & 1 == 1 {
                (crc >> 1) ^ CRC32_POLYNOMIAL
            } else {
                crc >> 1
            };
            bit += 1;
        }
        table[value] = crc;
        value += 1;
    }
    table
}

/// One entry of a [`RawZip`], as its local header and its row of the central directory declare it.
/// Each field is written as it is, whether or not it agrees with the others.
#[derive(Clone, Debug)]
pub struct RawEntry {
    /// The name in its row of the central directory, as bytes, so that it need not be UTF-8.
    pub name: Vec<u8>,
    /// The name in its local header, when that differs from [`name`](Self::name).
    pub local_name: Option<Vec<u8>>,
    /// What it holds, as the archive holds it: already deflated when its method deflates.
    pub data: Vec<u8>,
    /// Its compression method, in both headers: 0 stores and 8 deflates.
    pub method: u16,
    /// Its general-purpose flags, in both headers.
    pub flags: u16,
    /// The uncompressed size both headers declare, or `None` for the length of
    /// [`data`](Self::data), a stored entry's true size.
    pub declared_size: Option<u32>,
    /// The CRC-32 both headers declare, or `None` for that of [`data`](Self::data), a stored
    /// entry's true checksum.
    pub crc: Option<u32>,
    /// Its external attributes, in its row: a Unix mode in the high half and DOS attributes in the
    /// low.
    pub external_attributes: u32,
    /// The system and ZIP version its row says made it.
    pub version_made_by: u16,
    /// Its extra field, in both headers.
    pub extra: Vec<u8>,
    /// Its comment, in its row.
    pub comment: Vec<u8>,
}

impl RawEntry {
    /// An entry holding `data` deflated at level 6, as release.py writes a runtime's files, with
    /// its true size and checksum declared.
    pub fn deflated(name: &str, data: &[u8]) -> Self {
        let mut encoder = DeflateEncoder::new(Vec::new(), Compression::new(DEFLATE_LEVEL));
        encoder
            .write_all(data)
            .expect("deflating into memory cannot fail");
        let deflated = encoder.finish().expect("deflating into memory cannot fail");
        RawEntry {
            method: METHOD_DEFLATED,
            declared_size: Some(u32_field(data.len(), "an entry")),
            crc: Some(crc32(data)),
            ..RawEntry::stored(name, &deflated)
        }
    }

    /// An entry holding `data` as it is: a regular file of mode 0o644, made on a Unix system, with
    /// no flags, extra field or comment.
    pub fn stored(name: &str, data: &[u8]) -> Self {
        RawEntry {
            name: name.as_bytes().to_vec(),
            local_name: None,
            data: data.to_vec(),
            method: METHOD_STORED,
            flags: 0,
            declared_size: None,
            crc: None,
            external_attributes: REGULAR_FILE,
            version_made_by: MADE_BY_UNIX,
            extra: Vec::new(),
            comment: Vec::new(),
        }
    }

    /// The uncompressed size both headers declare.
    fn declared_len(&self) -> u32 {
        self.declared_size
            .unwrap_or_else(|| u32_field(self.data.len(), "an entry"))
    }
}

/// A ZIP archive written exactly as its fields say, duplicates and contradictions included. It has
/// no ZIP64 fields, so no entry, name or offset may pass what a field of 16 or 32 bits holds.
#[derive(Clone, Debug, Default)]
pub struct RawZip {
    /// Bytes before the first local header, as a self-extracting archive has its program before
    /// the archive. Every offset in the archive counts them.
    pub prefix: Vec<u8>,
    /// The entries, in the order of their local headers and of their rows alike.
    pub entries: Vec<RawEntry>,
    /// The number of entries the end record declares, on this disk and in all, or `None` for the
    /// number of [`entries`](Self::entries).
    pub eocd_count: Option<u16>,
    /// The archive's comment, after the end record.
    pub comment: Vec<u8>,
    /// Whether a ZIP64 end-of-central-directory locator comes just before the end record. It points
    /// where a ZIP64 record would start, after the central directory, but none is written.
    pub zip64_locator: bool,
}

impl RawZip {
    /// The archive's bytes, in order: the prefix, each entry's local header and data, the central
    /// directory, the ZIP64 locator when there is one, then the end record and the comment.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut bytes = self.prefix.clone();
        // Where each entry's local header starts, and the checksum both its headers declare.
        let mut written = Vec::with_capacity(self.entries.len());
        for entry in &self.entries {
            let offset = u32_field(bytes.len(), "an archive");
            let crc = entry.crc.unwrap_or_else(|| crc32(&entry.data));
            let name = entry.local_name.as_deref().unwrap_or(&entry.name);
            put_u32(&mut bytes, LOCAL_HEADER_SIGNATURE);
            put_u16(&mut bytes, VERSION_NEEDED);
            put_u16(&mut bytes, entry.flags);
            put_u16(&mut bytes, entry.method);
            put_u16(&mut bytes, DOS_TIME);
            put_u16(&mut bytes, DOS_DATE);
            put_u32(&mut bytes, crc);
            put_u32(&mut bytes, u32_field(entry.data.len(), "an entry"));
            put_u32(&mut bytes, entry.declared_len());
            put_u16(&mut bytes, u16_field(name.len(), "a name"));
            put_u16(&mut bytes, u16_field(entry.extra.len(), "an extra field"));
            bytes.extend_from_slice(name);
            bytes.extend_from_slice(&entry.extra);
            bytes.extend_from_slice(&entry.data);
            written.push((offset, crc));
        }
        let directory_start = bytes.len();
        for (entry, (offset, crc)) in self.entries.iter().zip(written) {
            put_u32(&mut bytes, CENTRAL_ROW_SIGNATURE);
            put_u16(&mut bytes, entry.version_made_by);
            put_u16(&mut bytes, VERSION_NEEDED);
            put_u16(&mut bytes, entry.flags);
            put_u16(&mut bytes, entry.method);
            put_u16(&mut bytes, DOS_TIME);
            put_u16(&mut bytes, DOS_DATE);
            put_u32(&mut bytes, crc);
            put_u32(&mut bytes, u32_field(entry.data.len(), "an entry"));
            put_u32(&mut bytes, entry.declared_len());
            put_u16(&mut bytes, u16_field(entry.name.len(), "a name"));
            put_u16(&mut bytes, u16_field(entry.extra.len(), "an extra field"));
            put_u16(&mut bytes, u16_field(entry.comment.len(), "a comment"));
            put_u16(&mut bytes, DISK);
            put_u16(&mut bytes, INTERNAL_ATTRIBUTES);
            put_u32(&mut bytes, entry.external_attributes);
            put_u32(&mut bytes, offset);
            bytes.extend_from_slice(&entry.name);
            bytes.extend_from_slice(&entry.extra);
            bytes.extend_from_slice(&entry.comment);
        }
        let directory_end = bytes.len();
        if self.zip64_locator {
            put_u32(&mut bytes, ZIP64_LOCATOR_SIGNATURE);
            put_u32(&mut bytes, u32::from(DISK));
            put_u64(&mut bytes, directory_end as u64);
            put_u32(&mut bytes, DISKS);
        }
        let count = self
            .eocd_count
            .unwrap_or_else(|| u16_field(self.entries.len(), "an entry count"));
        put_u32(&mut bytes, END_RECORD_SIGNATURE);
        put_u16(&mut bytes, DISK);
        put_u16(&mut bytes, DISK);
        put_u16(&mut bytes, count);
        put_u16(&mut bytes, count);
        put_u32(
            &mut bytes,
            u32_field(directory_end - directory_start, "a central directory"),
        );
        put_u32(&mut bytes, u32_field(directory_start, "an archive"));
        put_u16(&mut bytes, u16_field(self.comment.len(), "a comment"));
        bytes.extend_from_slice(&self.comment);
        bytes
    }
}

/// `len`, the length or count of `what`, as a ZIP field of 16 bits.
fn u16_field(len: usize, what: &str) -> u16 {
    u16::try_from(len)
        .unwrap_or_else(|_| panic!("{what} of {len} does not fit a ZIP field of 16 bits"))
}

/// `len`, the length or offset of `what`, as a ZIP field of 32 bits.
fn u32_field(len: usize, what: &str) -> u32 {
    u32::try_from(len)
        .unwrap_or_else(|_| panic!("{what} of {len} bytes does not fit a ZIP field of 32 bits"))
}

/// Appends `value` as a little-endian field of 16 bits.
fn put_u16(bytes: &mut Vec<u8>, value: u16) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

/// Appends `value` as a little-endian field of 32 bits.
fn put_u32(bytes: &mut Vec<u8>, value: u32) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

/// Appends `value` as a little-endian field of 64 bits.
fn put_u64(bytes: &mut Vec<u8>, value: u64) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

#[cfg(test)]
mod tests {
    use std::io::{Cursor, Read, Write};

    use ::zip::{CompressionMethod, System, ZipArchive};
    use flate2::Compression;
    use flate2::read::DeflateDecoder;
    use flate2::write::DeflateEncoder;

    use super::{RawEntry, RawZip, crc32};

    /// Reads an archive's little-endian fields one after another, from `at`.
    struct Fields<'a> {
        bytes: &'a [u8],
        at: usize,
    }

    impl<'a> Fields<'a> {
        fn new(bytes: &'a [u8], at: usize) -> Self {
            Fields { bytes, at }
        }

        fn take(&mut self, len: usize) -> &'a [u8] {
            let bytes = self.bytes;
            let taken = &bytes[self.at..self.at + len];
            self.at += len;
            taken
        }

        fn u16(&mut self) -> u16 {
            u16::from_le_bytes(self.take(2).try_into().unwrap())
        }

        fn u32(&mut self) -> u32 {
            u32::from_le_bytes(self.take(4).try_into().unwrap())
        }

        fn u64(&mut self) -> u64 {
            u64::from_le_bytes(self.take(8).try_into().unwrap())
        }
    }

    /// Bytes that deflate well, as a runtime's text and shaders do.
    fn text(len: usize) -> Vec<u8> {
        b"Gauntlet Dark Legacy keeps its data in plain JSON. "
            .iter()
            .copied()
            .cycle()
            .take(len)
            .collect()
    }

    /// Bytes that deflate badly, as a compiled program does, from a multiplicative hash.
    fn noise(len: usize) -> Vec<u8> {
        (0..len)
            .map(|index| (index as u32).wrapping_mul(2_654_435_761).to_le_bytes()[3])
            .collect()
    }

    fn inflate(data: &[u8]) -> Vec<u8> {
        let mut inflated = Vec::new();
        DeflateDecoder::new(data)
            .read_to_end(&mut inflated)
            .unwrap();
        inflated
    }

    #[test]
    fn crc32_check_value() {
        // The check value every CRC catalogue gives: the checksum of the nine ASCII digits.
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        // Python's zlib.crc32 gives these.
        assert_eq!(crc32(b""), 0);
        assert_eq!(crc32(b"a"), 0xE8B7_BE43);
        assert_eq!(
            crc32(b"The quick brown fox jumps over the lazy dog"),
            0x414F_A339
        );
        // It agrees with flate2's own checksum over short inputs and long ones.
        let bytes = noise(70_000);
        for len in [
            1, 2, 3, 4, 5, 7, 8, 9, 255, 256, 257, 4095, 4096, 65_537, 70_000,
        ] {
            let mut reference = flate2::Crc::new();
            reference.update(&bytes[..len]);
            assert_eq!(crc32(&bytes[..len]), reference.sum(), "{len}");
        }
    }

    #[test]
    fn entries_hold_what_they_are_made_from() {
        let data = text(5000);
        let stored = RawEntry::stored("data/config.json", &data);
        assert_eq!(stored.name, b"data/config.json");
        assert_eq!(stored.local_name, None);
        assert_eq!(stored.data, data);
        assert_eq!(stored.method, 0);
        assert_eq!(stored.flags, 0);
        assert_eq!(stored.declared_size, None);
        assert_eq!(stored.crc, None);
        assert_eq!(stored.external_attributes, 0o100644 << 16);
        assert_eq!(stored.version_made_by, 0x0314);
        assert!(stored.extra.is_empty() && stored.comment.is_empty());

        // Deflated as release.py deflates, at level 6, with the true size and checksum declared.
        let deflated = RawEntry::deflated("data/config.json", &data);
        let mut encoder = DeflateEncoder::new(Vec::new(), Compression::new(6));
        encoder.write_all(&data).unwrap();
        assert_eq!(deflated.data, encoder.finish().unwrap());
        assert!(deflated.data.len() < data.len());
        assert_eq!(inflate(&deflated.data), data);
        assert_eq!(deflated.method, 8);
        assert_eq!(deflated.declared_size, Some(5000));
        assert_eq!(deflated.crc, Some(crc32(&data)));
        assert_eq!(deflated.name, stored.name);
        assert_eq!(deflated.local_name, None);
        assert_eq!(deflated.flags, 0);
        assert_eq!(deflated.external_attributes, stored.external_attributes);
        assert_eq!(deflated.version_made_by, stored.version_made_by);
        assert!(deflated.extra.is_empty() && deflated.comment.is_empty());

        // An empty file deflates to a stream that holds nothing.
        let empty = RawEntry::deflated("portable.flag", b"");
        assert!(!empty.data.is_empty());
        assert!(inflate(&empty.data).is_empty());
        assert_eq!((empty.declared_size, empty.crc), (Some(0), Some(0)));
    }

    #[test]
    fn a_plain_raw_zip_reads_back_with_zip() {
        let files: [(&str, Vec<u8>, bool); 7] = [
            ("VERSION", b"1.0.0-beta\n".to_vec(), false),
            ("data/config.json", b"{}".to_vec(), true),
            ("data/text/en.json", text(300_000), true),
            ("gauntlet", noise(200_000), true),
            ("licenses/zip.txt", text(40_000), false),
            ("portable.flag", Vec::new(), false),
            ("shaders/empty.spv", Vec::new(), true),
        ];
        let raw = RawZip {
            entries: files
                .iter()
                .map(|(name, data, deflate)| {
                    if *deflate {
                        RawEntry::deflated(name, data)
                    } else {
                        RawEntry::stored(name, data)
                    }
                })
                .collect(),
            ..RawZip::default()
        };
        let mut archive = ZipArchive::new(Cursor::new(raw.to_bytes())).unwrap();
        assert_eq!(archive.len(), files.len());
        assert_eq!(archive.offset(), 0);
        assert!(archive.comment().is_empty());
        assert!(!archive.has_duplicate_names());
        let names: Vec<&[u8]> = archive.file_names_raw().collect();
        let expected: Vec<&[u8]> = files.iter().map(|(name, ..)| name.as_bytes()).collect();
        assert_eq!(names, expected);
        archive.validate_header_consistency().unwrap();

        let mut start = 0;
        for (index, (name, data, deflate)) in files.iter().enumerate() {
            let mut file = archive.by_index(index).unwrap();
            assert_eq!(file.name().unwrap(), *name);
            let method = if *deflate {
                CompressionMethod::Deflated
            } else {
                CompressionMethod::Stored
            };
            assert_eq!(file.compression(), method, "{name}");
            assert_eq!(file.size(), data.len() as u64, "{name}");
            assert_eq!(file.crc32(), crc32(data), "{name}");
            if *deflate {
                assert_eq!(
                    file.compressed_size(),
                    raw.entries[index].data.len() as u64,
                    "{name}"
                );
            } else {
                assert_eq!(file.compressed_size(), data.len() as u64, "{name}");
            }
            assert_eq!(file.system(), System::Unix, "{name}");
            assert_eq!(file.version_made_by(), (2, 0), "{name}");
            assert_eq!(file.unix_mode(), Some(0o100644), "{name}");
            assert!(
                file.is_file() && !file.is_dir() && !file.is_symlink(),
                "{name}"
            );
            assert!(!file.encrypted(), "{name}");
            assert!(file.comment().is_empty(), "{name}");
            assert_eq!(file.extra_data(), None, "{name}");
            let modified = file.last_modified().unwrap();
            assert_eq!((modified.datepart(), modified.timepart()), (0x21, 0));
            assert_eq!(
                (modified.year(), modified.month(), modified.day()),
                (1980, 1, 1)
            );
            // The entries lie one after another from the archive's first byte.
            assert_eq!(file.header_start(), start, "{name}");
            start += (30 + name.len() + raw.entries[index].data.len()) as u64;
            // Reading to the end checks the declared size and checksum.
            let mut read = Vec::new();
            file.read_to_end(&mut read).unwrap();
            assert_eq!(read, *data, "{name}");
        }
        assert_eq!(archive.central_directory_start(), start);

        // An archive of no entries is its end record alone, and reads back as empty.
        let empty = RawZip::default().to_bytes();
        assert_eq!(empty.len(), 22);
        assert_eq!(empty[..4], *b"PK\x05\x06");
        assert!(empty[4..].iter().all(|&byte| byte == 0));
        let archive = ZipArchive::new(Cursor::new(empty)).unwrap();
        assert!(archive.is_empty());
    }

    #[test]
    fn raw_zips_write_each_field_as_told() {
        // An entry whose every field differs from what a writer would derive.
        let mut odd = RawEntry::stored("data/a.json", b"hello");
        odd.local_name = Some(b"data/A.JSON".to_vec());
        odd.method = 12;
        odd.flags = 0x0009;
        odd.declared_size = Some(99);
        odd.crc = Some(0x1234_5678);
        odd.external_attributes = (0o120777 << 16) | 0x10;
        odd.version_made_by = 0x0A3F;
        odd.extra = vec![0x55, 0x54, 0x01, 0x00, 0x07];
        odd.comment = b"a row's comment".to_vec();
        // A name that is not UTF-8, written twice.
        let mut twin = RawEntry::deflated("shaders/twin.spv", b"shader");
        twin.name = b"shaders/\xfftwin.spv".to_vec();
        let raw = RawZip {
            prefix: b"MZ\x90".to_vec(),
            entries: vec![odd.clone(), twin.clone(), twin.clone()],
            eocd_count: Some(7),
            comment: b"an archive's comment".to_vec(),
            zip64_locator: true,
        };
        let bytes = raw.to_bytes();
        assert_eq!(bytes[..3], *b"MZ\x90");

        // The local headers and data, from the end of the prefix.
        let mut fields = Fields::new(&bytes, 3);
        let mut offsets = Vec::new();
        for (entry, name, crc, size) in [
            (&odd, &b"data/A.JSON"[..], 0x1234_5678, 99),
            (&twin, &twin.name[..], crc32(b"shader"), 6),
            (&twin, &twin.name[..], crc32(b"shader"), 6),
        ] {
            offsets.push(fields.at as u32);
            assert_eq!(fields.u32(), 0x0403_4b50);
            assert_eq!(fields.u16(), 20);
            assert_eq!(fields.u16(), entry.flags);
            assert_eq!(fields.u16(), entry.method);
            assert_eq!((fields.u16(), fields.u16()), (0, 0x21));
            assert_eq!(fields.u32(), crc);
            assert_eq!(fields.u32() as usize, entry.data.len());
            assert_eq!(fields.u32(), size);
            assert_eq!(fields.u16() as usize, name.len());
            assert_eq!(fields.u16() as usize, entry.extra.len());
            assert_eq!(fields.take(name.len()), name);
            assert_eq!(fields.take(entry.extra.len()), entry.extra);
            assert_eq!(fields.take(entry.data.len()), entry.data);
        }
        assert_eq!(offsets[0], 3);
        assert_eq!(inflate(&twin.data), b"shader");

        // The central directory, its rows in the same order, then the locator and the end record.
        let directory_start = fields.at;
        for (entry, offset) in [&odd, &twin, &twin].into_iter().zip(offsets) {
            assert_eq!(fields.u32(), 0x0201_4b50);
            assert_eq!(fields.u16(), entry.version_made_by);
            assert_eq!(fields.u16(), 20);
            assert_eq!(fields.u16(), entry.flags);
            assert_eq!(fields.u16(), entry.method);
            assert_eq!((fields.u16(), fields.u16()), (0, 0x21));
            assert_eq!(
                fields.u32(),
                entry.crc.unwrap_or_else(|| crc32(&entry.data))
            );
            assert_eq!(fields.u32() as usize, entry.data.len());
            assert_eq!(fields.u32(), entry.declared_size.unwrap());
            assert_eq!(fields.u16() as usize, entry.name.len());
            assert_eq!(fields.u16() as usize, entry.extra.len());
            assert_eq!(fields.u16() as usize, entry.comment.len());
            // Its disk and internal attributes.
            assert_eq!((fields.u16(), fields.u16()), (0, 0));
            assert_eq!(fields.u32(), entry.external_attributes);
            assert_eq!(fields.u32(), offset);
            assert_eq!(fields.take(entry.name.len()), entry.name);
            assert_eq!(fields.take(entry.extra.len()), entry.extra);
            assert_eq!(fields.take(entry.comment.len()), entry.comment);
        }
        let directory_end = fields.at;
        assert_eq!(fields.u32(), 0x0706_4b50);
        assert_eq!(fields.u32(), 0);
        assert_eq!(fields.u64(), directory_end as u64);
        assert_eq!(fields.u32(), 1);
        let end_record = fields.at;
        assert_eq!(end_record, bytes.len() - 22 - raw.comment.len());
        assert_eq!(fields.u32(), 0x0605_4b50);
        assert_eq!((fields.u16(), fields.u16()), (0, 0));
        assert_eq!((fields.u16(), fields.u16()), (7, 7));
        assert_eq!(fields.u32() as usize, directory_end - directory_start);
        assert_eq!(fields.u32() as usize, directory_start);
        assert_eq!(fields.u16() as usize, raw.comment.len());
        assert_eq!(fields.take(raw.comment.len()), raw.comment);
        assert_eq!(fields.at, bytes.len());

        // Without a comment the locator lies 42 bytes from the end, and a count of the ZIP64
        // sentinel is written as it is.
        let bare = RawZip {
            entries: vec![twin],
            eocd_count: Some(0xFFFF),
            zip64_locator: true,
            ..RawZip::default()
        }
        .to_bytes();
        let mut fields = Fields::new(&bare, bare.len() - 42);
        assert_eq!(fields.u32(), 0x0706_4b50);
        let mut fields = Fields::new(&bare, bare.len() - 22);
        assert_eq!(fields.u32(), 0x0605_4b50);
        assert_eq!((fields.u16(), fields.u16()), (0, 0));
        assert_eq!((fields.u16(), fields.u16()), (0xFFFF, 0xFFFF));
    }
}
