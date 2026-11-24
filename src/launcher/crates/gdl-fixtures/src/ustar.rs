//! Tar archives written header by header, for the tests of the update's Linux archive rules: the
//! one member a release's Linux launcher file holds, and archives no tar writer would make, such as
//! a member whose name climbs out of its folder or a header that promises more than follows it.
//! Each header is a ustar block laid out as Python's tarfile lays one out, fields written as told.

use std::io::Write;
use std::ops::Range;

use flate2::Compression;
use flate2::write::GzEncoder;

/// The length of a tar block: each header fills one, and each entry's data is padded to whole ones.
const BLOCK: usize = 512;

/// How many zero blocks end an archive.
const END_BLOCKS: usize = 2;

/// A header's name, NUL-padded, or filling the field without a NUL when it takes all of it.
const NAME: Range<usize> = 0..100;

/// A header's mode.
const MODE: Range<usize> = 100..108;

/// The id of the user who owns the entry.
const USER: Range<usize> = 108..116;

/// The id of the group that owns the entry.
const GROUP: Range<usize> = 116..124;

/// The size of the data that follows the header, as the header declares it.
const SIZE: Range<usize> = 124..136;

/// The entry's modification time, in seconds since the epoch.
const MODIFIED: Range<usize> = 136..148;

/// A header's checksum field, read as eight spaces while the checksum is summed.
const CHECKSUM: Range<usize> = 148..156;

/// Where the checksum's six octal digits and their NUL go, leaving the field's last byte a space,
/// as tar writers leave it.
const CHECKSUM_DIGITS: Range<usize> = 148..155;

/// Where a header holds its entry's type.
const TYPE_AT: usize = 156;

/// A link's target, NUL-padded like the name.
const LINK_TARGET: Range<usize> = 157..257;

/// Where the ustar magic and its version go.
const MAGIC: Range<usize> = 257..265;

/// The ustar magic, `ustar` and a NUL, then its version, `00`.
const USTAR_MAGIC: &[u8; 8] = b"ustar\x0000";

/// The type of a regular file.
const REGULAR_FILE: u8 = b'0';

/// The type of a hard link.
const HARD_LINK: u8 = b'1';

/// The type of a symbolic link.
const SYMBOLIC_LINK: u8 = b'2';

/// The type of a folder.
const FOLDER: u8 = b'5';

/// The mode every entry carries: a program's, as the launcher's one member is.
const ENTRY_MODE: u64 = 0o755;

/// The user and group every entry names: root's.
const OWNER_ID: u64 = 0;

/// The time every entry was modified: the epoch, so that the same entries always make the same
/// bytes.
const MODIFIED_AT: u64 = 0;

/// What fills the checksum field while the checksum is summed.
const CHECKSUM_FILL: u8 = b' ';

/// One entry of a tar archive a test writes.
#[derive(Clone, Copy, Debug)]
pub enum TarEntry<'a> {
    /// A regular file holding `data`.
    File { name: &'a str, data: &'a [u8] },
    /// A symbolic link to `target`.
    Symlink { name: &'a str, target: &'a str },
    /// A hard link to the entry named `target`.
    HardLink { name: &'a str, target: &'a str },
    /// A folder. Its name is written as given: a tar writer would end it with `/`.
    Folder { name: &'a str },
    /// A regular file whose header declares `size` while `data` follows it, padded to a whole
    /// block as any entry's data is. A reader that takes `size` bytes reads on through the padding
    /// and the archive's end, and finds the member short only when `size` passes them all; one
    /// that takes fewer bytes than `data` holds looks for the next header inside it.
    Declared {
        name: &'a str,
        size: u64,
        data: &'a [u8],
    },
}

impl TarEntry<'_> {
    /// The entry's header: its name, its type, the size it declares and, for a link, its target.
    fn header(&self) -> [u8; BLOCK] {
        match *self {
            TarEntry::File { name, data } => header(name, REGULAR_FILE, data.len() as u64, ""),
            TarEntry::Symlink { name, target } => header(name, SYMBOLIC_LINK, 0, target),
            TarEntry::HardLink { name, target } => header(name, HARD_LINK, 0, target),
            TarEntry::Folder { name } => header(name, FOLDER, 0, ""),
            TarEntry::Declared { name, size, .. } => header(name, REGULAR_FILE, size, ""),
        }
    }

    /// What follows the entry's header, before its padding.
    fn data(&self) -> &[u8] {
        match *self {
            TarEntry::File { data, .. } | TarEntry::Declared { data, .. } => data,
            TarEntry::Symlink { .. } | TarEntry::HardLink { .. } | TarEntry::Folder { .. } => &[],
        }
    }
}

/// `entries` as a ustar archive, ended by two zero blocks, then gzip-compressed. Panics when a
/// name or a link's target is longer than its field of 100 bytes, or a declared size takes more
/// than its field's eleven octal digits.
pub fn tar_gz(entries: &[TarEntry<'_>]) -> Vec<u8> {
    // The gzip header carries no name and no time, so the same entries always make the same bytes.
    let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
    encoder
        .write_all(&ustar(entries))
        .expect("compressing into memory cannot fail");
    encoder
        .finish()
        .expect("compressing into memory cannot fail")
}

/// `entries` as an uncompressed ustar archive: each entry's header, then its data padded with zeros
/// to a whole block, then the two zero blocks that end an archive.
fn ustar(entries: &[TarEntry<'_>]) -> Vec<u8> {
    let mut archive = Vec::new();
    for entry in entries {
        archive.extend_from_slice(&entry.header());
        archive.extend_from_slice(entry.data());
        archive.resize(archive.len().next_multiple_of(BLOCK), 0);
    }
    archive.resize(archive.len() + END_BLOCKS * BLOCK, 0);
    archive
}

/// A ustar header for an entry named `name` of type `kind`, declaring `size` bytes of data and
/// linking to `target` (empty for anything but a link). The owner's names, the device numbers and
/// the name's prefix are left empty.
fn header(name: &str, kind: u8, size: u64, target: &str) -> [u8; BLOCK] {
    let mut header = [0; BLOCK];
    text(&mut header[NAME], name, "a name");
    octal(&mut header[MODE], ENTRY_MODE);
    octal(&mut header[USER], OWNER_ID);
    octal(&mut header[GROUP], OWNER_ID);
    octal(&mut header[SIZE], size);
    octal(&mut header[MODIFIED], MODIFIED_AT);
    header[TYPE_AT] = kind;
    text(&mut header[LINK_TARGET], target, "a link's target");
    header[MAGIC].copy_from_slice(USTAR_MAGIC);
    // The checksum is the sum of the header's bytes, its own field counted as spaces.
    header[CHECKSUM].fill(CHECKSUM_FILL);
    let sum: u64 = header.iter().map(|&byte| u64::from(byte)).sum();
    octal(&mut header[CHECKSUM_DIGITS], sum);
    header
}

/// Writes `value`, the `what` of an entry, into `field`, NUL-padded.
fn text(field: &mut [u8], value: &str, what: &str) {
    let bytes = value.as_bytes();
    assert!(
        bytes.len() <= field.len(),
        "{what} of {} bytes does not fit a tar field of {}: {value:?}",
        bytes.len(),
        field.len()
    );
    field[..bytes.len()].copy_from_slice(bytes);
}

/// Writes `value` into `field` as tar writes a number: zero-filled octal digits in every byte but
/// the last, which is NUL.
fn octal(field: &mut [u8], value: u64) {
    let digits = field.len() - 1;
    let written = format!("{value:0digits$o}");
    assert!(
        written.len() == digits,
        "{value} does not fit a tar field of {digits} octal digits"
    );
    field[..digits].copy_from_slice(written.as_bytes());
    field[digits] = 0;
}

#[cfg(test)]
mod tests {
    use std::io::Read;

    use flate2::read::GzDecoder;

    use super::{TarEntry, tar_gz};
    use crate::runtime::sha256_hex;

    /// The one member a release's Linux launcher file holds.
    const MEMBER: &str = "GauntletDarkLegacy-1.0.0-beta-linux-x64-launcher";

    /// The archive `tar_gz` compressed.
    fn gunzip(compressed: &[u8]) -> Vec<u8> {
        let mut archive = Vec::new();
        GzDecoder::new(compressed)
            .read_to_end(&mut archive)
            .expect("a gzip stream");
        archive
    }

    /// The sum of `header`'s bytes with its checksum field read as spaces, as tar readers check it.
    fn checksum_of(header: &[u8]) -> u32 {
        header[..148]
            .iter()
            .chain(b"        ")
            .chain(&header[156..512])
            .map(|&byte| u32::from(byte))
            .sum()
    }

    /// The text a NUL-terminated field of `header` holds.
    fn field(header: &[u8], at: usize, len: usize) -> &str {
        let bytes = &header[at..at + len];
        let end = bytes.iter().position(|&byte| byte == 0).unwrap_or(len);
        std::str::from_utf8(&bytes[..end]).expect("ASCII")
    }

    #[test]
    fn a_header_is_laid_out_as_ustar_lays_one_out() {
        let data = b"#!/bin/sh\nexec launcher\n";
        let archive = gunzip(&tar_gz(&[TarEntry::File { name: MEMBER, data }]));

        // The header, field by field, as Python's tarfile writes a TarInfo of this name, mode
        // 0o755 and size with tobuf(tarfile.USTAR_FORMAT), which gives this same block.
        let mut expected = MEMBER.as_bytes().to_vec();
        expected.resize(100, 0);
        expected.extend_from_slice(b"0000755\0"); // the mode
        expected.extend_from_slice(b"0000000\0"); // the user
        expected.extend_from_slice(b"0000000\0"); // the group
        expected.extend_from_slice(b"00000000030\0"); // the size: 24 bytes
        expected.extend_from_slice(b"00000000000\0"); // the time
        expected.extend_from_slice(b"016314\0 "); // the checksum: six digits, NUL, space
        expected.push(b'0'); // a regular file
        expected.resize(257, 0); // no link's target
        expected.extend_from_slice(b"ustar\x0000");
        expected.resize(512, 0); // no owner's names, device numbers or name prefix
        assert_eq!(archive[..512], expected[..]);
        assert_eq!(
            sha256_hex(&archive[..512]),
            "c3782f102f39e43baed46c1d5b54f6515fe4d8b9869dc925f73a1bfe95feb3ef"
        );
        // The checksum is the header's own sum, its field read as spaces.
        assert_eq!(checksum_of(&archive[..512]), 0o16314);

        // The data fills the next block, padded with zeros, and two zero blocks end the archive.
        assert_eq!(archive.len(), 4 * 512);
        assert_eq!(archive[512..512 + data.len()], data[..]);
        assert!(archive[512 + data.len()..].iter().all(|&byte| byte == 0));
    }

    #[test]
    fn a_declared_size_is_written_as_given() {
        // A header promising a byte past 2 GiB, the most a launcher member may hold, over no
        // data: the archive holds the header and its end alone.
        let oversized = gunzip(&tar_gz(&[TarEntry::Declared {
            name: MEMBER,
            size: (2 << 30) + 1,
            data: b"",
        }]));
        assert_eq!(field(&oversized, 124, 12), "20000000001");
        assert_eq!(oversized[135], 0);
        assert_eq!(oversized[156], b'0');
        assert_eq!(checksum_of(&oversized[..512]), 0o16314);
        assert_eq!(field(&oversized, 148, 8), "016314");
        assert_eq!(oversized.len(), 3 * 512);
        assert!(oversized[512..].iter().all(|&byte| byte == 0));
        assert_eq!(
            sha256_hex(&oversized[..512]),
            "a7a87b7b318c854aa31bab2111bcb42384dd95c6ceb5bd9f3ed12b21795b607e"
        );

        // A header promising a MiB over a few bytes: they follow, padded to their own block, and
        // the archive ends long before the MiB a reader would look for.
        let short = gunzip(&tar_gz(&[TarEntry::Declared {
            name: MEMBER,
            size: 1 << 20,
            data: b"a short member",
        }]));
        assert_eq!(field(&short, 124, 12), "00004000000");
        assert_eq!(checksum_of(&short[..512]), 0o16315);
        assert_eq!(field(&short, 148, 8), "016315");
        assert_eq!(short.len(), 4 * 512);
        assert_eq!(&short[512..526], b"a short member");
        assert!(short[526..].iter().all(|&byte| byte == 0));
        assert_eq!(
            sha256_hex(&short[..512]),
            "38d9dea0ce1bf9528ab0c18796d5f400fd6212fecad28bf6e918117f65a21f3b"
        );

        // A header promising less than follows: every byte is written all the same.
        let data = [b'x'; 700];
        let long = gunzip(&tar_gz(&[TarEntry::Declared {
            name: MEMBER,
            size: 3,
            data: &data,
        }]));
        assert_eq!(field(&long, 124, 12), "00000000003");
        assert_eq!(
            checksum_of(&long[..512]),
            u32::from_str_radix(field(&long, 148, 8), 8).unwrap()
        );
        assert_eq!(long.len(), 5 * 512);
        assert_eq!(long[512..1212], data[..]);
        assert!(long[1212..].iter().all(|&byte| byte == 0));

        // A size that agrees with its data is a plain file's header, and the largest size the
        // field holds, eleven octal sevens, is written too.
        let data = b"the launcher";
        assert_eq!(
            tar_gz(&[TarEntry::Declared {
                name: MEMBER,
                size: data.len() as u64,
                data,
            }]),
            tar_gz(&[TarEntry::File { name: MEMBER, data }])
        );
        let largest = gunzip(&tar_gz(&[TarEntry::Declared {
            name: MEMBER,
            size: 0o77777777777,
            data: b"",
        }]));
        assert_eq!(field(&largest, 124, 12), "77777777777");
        assert_eq!(
            checksum_of(&largest[..512]),
            u32::from_str_radix(field(&largest, 148, 8), 8).unwrap()
        );
    }

    #[test]
    fn each_kind_of_entry_is_written_as_its_type() {
        // A name that steps through its own folder, and a file that fills two whole blocks.
        const DOTTED: &str = "./GauntletDarkLegacy-1.0.0-beta-linux-x64-launcher";
        const FILLED: [u8; 1024] = [0xA5; 1024];
        let archive = gunzip(&tar_gz(&[
            TarEntry::Symlink {
                name: MEMBER,
                target: "../outside",
            },
            TarEntry::HardLink {
                name: "hard-link",
                target: MEMBER,
            },
            TarEntry::Folder { name: "folder" },
            TarEntry::File {
                name: "../outside",
                data: b"climbs out",
            },
            TarEntry::File {
                name: DOTTED,
                data: &FILLED,
            },
            TarEntry::File {
                name: "empty",
                data: b"",
            },
        ]));

        // Each header, with its name, type, size and link's target, then the blocks its data
        // fills.
        let expected: [(&str, u8, &[u8], &str); 6] = [
            (MEMBER, b'2', b"", "../outside"),
            ("hard-link", b'1', b"", MEMBER),
            ("folder", b'5', b"", ""),
            ("../outside", b'0', b"climbs out", ""),
            (DOTTED, b'0', &FILLED, ""),
            ("empty", b'0', b"", ""),
        ];
        let mut at = 0;
        for (entry, (name, kind, data, target)) in expected.into_iter().enumerate() {
            let header = &archive[at..at + 512];
            assert_eq!(field(header, 0, 100), name, "{entry}");
            assert_eq!(field(header, 100, 8), "0000755", "{entry}");
            assert_eq!(field(header, 108, 8), "0000000", "{entry}");
            assert_eq!(field(header, 116, 8), "0000000", "{entry}");
            assert_eq!(
                field(header, 124, 12),
                format!("{:011o}", data.len()),
                "{entry}"
            );
            assert_eq!(field(header, 136, 12), "00000000000", "{entry}");
            assert_eq!(header[154..156], *b"\0 ", "{entry}");
            assert_eq!(
                checksum_of(header),
                u32::from_str_radix(field(header, 148, 8), 8).unwrap(),
                "{entry}"
            );
            assert_eq!(header[156], kind, "{entry}");
            assert_eq!(field(header, 157, 100), target, "{entry}");
            assert_eq!(header[257..265], *b"ustar\x0000", "{entry}");
            assert!(header[265..].iter().all(|&byte| byte == 0), "{entry}");
            at += 512;
            assert_eq!(archive[at..at + data.len()], *data, "{entry}");
            let padded = data.len().div_ceil(512) * 512;
            assert!(
                archive[at + data.len()..at + padded]
                    .iter()
                    .all(|&byte| byte == 0),
                "{entry}"
            );
            at += padded;
        }
        // Two zero blocks end the archive, and nothing follows them.
        assert_eq!(archive.len(), at + 2 * 512);
        assert!(archive[at..].iter().all(|&byte| byte == 0));

        // Python's tarfile writes these same headers for a symbolic link, a hard link and a
        // folder of mode 0o755. It ends a folder's name with a slash, and given one so does this.
        for (entry, digest) in [
            (
                TarEntry::Symlink {
                    name: "outside-link",
                    target: "../outside",
                },
                "bb2e94228b5ed54f65e3ce99dc975b9b99f85a91133f75c315339ea35d4e54ea",
            ),
            (
                TarEntry::HardLink {
                    name: "hard-link",
                    target: MEMBER,
                },
                "0f8d74aeb6db344becd41647f2dc5dc715f94b040b6c63ce457428b5e7b18e85",
            ),
            (
                TarEntry::Folder { name: "folder/" },
                "f52bcd68b925aa836285437b0cae26adc4bf820c7f5a302fe689e42fa34e50ef",
            ),
        ] {
            assert_eq!(
                sha256_hex(&gunzip(&tar_gz(&[entry]))[..512]),
                digest,
                "{entry:?}"
            );
        }
    }

    #[test]
    fn archives_are_gzip_streams_that_carry_no_time() {
        let entries = [TarEntry::File {
            name: MEMBER,
            data: b"the launcher",
        }];
        let compressed = tar_gz(&entries);
        // The gzip magic, deflate, no flags and no time; the system byte is the unknown one.
        assert_eq!(compressed[..8], [0x1F, 0x8B, 8, 0, 0, 0, 0, 0]);
        assert_eq!(compressed[9], 0xFF);
        assert_eq!(tar_gz(&entries), compressed);
        assert!(compressed.len() < gunzip(&compressed).len());

        // No entries make an archive of its two zero blocks alone.
        assert_eq!(gunzip(&tar_gz(&[])), vec![0; 2 * 512]);
    }

    #[test]
    fn a_name_takes_its_whole_field() {
        // A name of exactly 100 bytes fills its field with no NUL after it.
        let name = "n".repeat(100);
        let archive = gunzip(&tar_gz(&[TarEntry::File {
            name: &name,
            data: b"",
        }]));
        assert_eq!(archive[..100], *name.as_bytes());
        assert_eq!(archive[100..108], *b"0000755\0");
    }

    #[test]
    #[should_panic(expected = "a name of 101 bytes does not fit a tar field of 100")]
    fn a_name_longer_than_its_field_panics() {
        tar_gz(&[TarEntry::Folder {
            name: &"n".repeat(101),
        }]);
    }

    #[test]
    #[should_panic(expected = "a link's target of 101 bytes does not fit a tar field of 100")]
    fn a_target_longer_than_its_field_panics() {
        tar_gz(&[TarEntry::Symlink {
            name: MEMBER,
            target: &"t".repeat(101),
        }]);
    }

    #[test]
    #[should_panic(expected = "8589934592 does not fit a tar field of 11 octal digits")]
    fn a_size_past_eleven_octal_digits_panics() {
        tar_gz(&[TarEntry::Declared {
            name: MEMBER,
            size: 8 << 30,
            data: b"",
        }]);
    }
}
