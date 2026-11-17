//! The ISO 9660 volume a GD-ROM's high-density area holds, read as the Dreamcast extractor reads
//! it: the primary volume descriptor after the boot area, which names the volume and keeps the
//! root directory's record, and the tree of directories from that root, walked record by record
//! in the order each directory keeps them, a folder walked as soon as its record is met. Each
//! record's identifier is judged by itself, its version dropped, before it is joined to the path
//! of the folder that holds it, so that no identifier names a place outside the tree or stands
//! for a folder and the file within it.

use std::collections::BTreeSet;
use std::ops::Range;
use std::str;

use gdl_files::{Cancel, DISC_NAMES, NameSet, RelPath, check_component};

use super::{BOOT_LBA, BOOT_SECTORS, DiscFile, SECTOR, Tracks, ascii_text, is_number, unsafe_name};
use crate::error::quoted;
use crate::{DiscRule, MediaError};

/// The LBA of the primary volume descriptor: the first sector after the boot area, which fills
/// the sectors ISO 9660 keeps for a system's own use.
pub const PVD_LBA: u32 = BOOT_LBA + BOOT_SECTORS;

/// What a primary volume descriptor begins with: its type, 1, the standard's identifier, and the
/// descriptor's version, 1.
pub const STANDARD_ID: &[u8; 7] = b"\x01CD001\x01";

/// Where the volume descriptor keeps the root directory's record.
pub const ROOT_RECORD_AT: usize = 156;

/// Where the volume descriptor names the volume, padded with spaces.
pub const VOLUME_ID: Range<usize> = 40..72;

/// Where the volume descriptor gives the size of its logical blocks, both-endian, which must be a
/// sector's user data.
pub const BLOCK_SIZE_AT: usize = 128;

/// The fewest bytes a directory record takes: its fixed fields and an identifier of one byte.
pub const MIN_RECORD_LEN: usize = 34;

/// How deep below the root, which lies at depth 0, a directory may lie.
pub const MAX_DIRECTORY_DEPTH: usize = 32;

/// The most bytes a directory may hold.
pub const MAX_DIRECTORY_LEN: u32 = 16 << 20;

/// The most entries, folders and files, the volume's tree may hold.
pub const MAX_ENTRIES: usize = 100_000;

/// The flag of a record whose extent is a directory.
pub const FLAG_DIRECTORY: u8 = 2;

/// The flags a record may carry: [`FLAG_DIRECTORY`], and 1, which hides an entry from a listing.
/// Any other marks what the reader does not read, such as a file continued in another record.
pub const ALLOWED_FLAGS: u8 = 3;

/// [`SECTOR`] as the size of the extent the volume descriptor is read as.
const DESCRIPTOR_EXTENT: u32 = SECTOR as u32;

/// [`SECTOR`] as the size of a logical block, which the volume descriptor must give.
const BLOCK_SIZE: u16 = SECTOR as u16;

/// Where a record gives the length of the extended attribute record before its extent.
const ATTRIBUTE_LEN_AT: usize = 1;

/// Where a record gives the LBA of its extent's first sector, both-endian.
const EXTENT_LBA_AT: usize = 2;

/// Where a record gives how many bytes its extent holds, both-endian.
const EXTENT_SIZE_AT: usize = 10;

/// Where a record keeps its flags.
const FLAGS_AT: usize = 25;

/// Where a record gives the size of an interleaved file's units.
const UNIT_SIZE_AT: usize = 26;

/// Where a record gives the gap between an interleaved file's units.
const INTERLEAVE_GAP_AT: usize = 27;

/// Where a record gives the sequence number of the volume its extent lies on, both-endian.
const SEQUENCE_AT: usize = 28;

/// Where a record gives its identifier's length.
const IDENTIFIER_LEN_AT: usize = 32;

/// Where a record's identifier begins, after its fixed fields.
const IDENTIFIER_AT: usize = 33;

/// The sequence number of a disc's one volume.
const ONE_VOLUME: u16 = 1;

/// The identifier a directory lists itself by.
const SELF_ID: &[u8] = b"\0";

/// The identifier a directory lists its parent by.
const PARENT_ID: &[u8] = b"\x01";

/// What comes between a file's name and the version ISO 9660 numbers it by.
const VERSION_SEPARATOR: char = ';';

/// What comes between a path's components.
const PATH_SEPARATOR: char = '/';

/// What the reader calls the volume descriptor's name of the volume when a byte of it lies outside
/// ASCII.
const VOLUME_FIELD: &str = "ISO9660 volume identifier";

/// What the Dreamcast extractor answers for a volume descriptor that is no primary one of ISO
/// 9660 with blocks of a sector's user data.
const UNSUPPORTED_DESCRIPTOR: &str = "Unsupported ISO9660 volume descriptor";

/// What it answers for a both-endian number whose halves differ.
const MISMATCHED_HALVES: &str = "Mismatched ISO9660 little/big-endian fields";

/// What it answers for a root record that is not a directory's.
const ROOT_NOT_DIRECTORY: &str = "ISO root is not a directory";

/// What it answers for a record shorter than its fixed fields, or that runs past its directory's
/// end or into the next sector.
const TRUNCATED_RECORD: &str = "Truncated ISO9660 directory record";

/// What it answers for a record of a kind it does not read.
const UNSUPPORTED_RECORD: &str = "Unsupported ISO9660 extended/interleaved/multiextent record";

/// What it answers for a record of another volume, or whose identifier runs past its end.
const INVALID_IDENTIFIER: &str = "Invalid ISO9660 file identifier";

/// What it answers for a directory too deep, too long, or read already.
const DIRECTORY_SHAPE: &str = "Cyclic or oversized ISO9660 directory";

/// What it answers for an entry whose path is taken already, or one past the most.
const EXCESSIVE_ENTRIES: &str = "Duplicate or excessive ISO9660 entries";

/// What the reader answers, before the identifier, for a record whose identifier holds a byte
/// outside ASCII.
const NON_ASCII_IDENTIFIER: &str = "ISO9660 file identifier holds a byte outside ASCII";

/// A disc's ISO 9660 volume as its walk read it: the volume's name and every file of its tree.
#[derive(Debug)]
pub(super) struct Volume {
    /// The volume's name, as the volume descriptor gives it, without the white space that pads
    /// it.
    pub(super) name: String,
    /// Every file of the tree, in the order the walk met them.
    pub(super) files: Vec<DiscFile>,
}

/// Where an extent lies on the disc and how many bytes it holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Extent {
    /// The LBA of its first sector.
    lba: u32,
    /// How many bytes it holds, from the start of its first sector.
    size: u32,
}

/// A directory record as the walk reads it.
#[derive(Clone, Copy, Debug)]
struct Record<'a> {
    /// The extent of the file or the directory it lists.
    extent: Extent,
    /// Its flags.
    flags: u8,
    /// Its identifier, as the directory keeps it.
    identifier: &'a [u8],
}

/// The ISO 9660 volume the disc's data tracks hold, read as the Dreamcast extractor reads it: the
/// volume descriptor at [`PVD_LBA`] and its root record, then the tree from that root. The
/// descriptor is one sector, read whole; `cancel` is looked at before each chunk a directory's
/// read takes, since a disc decides how many directories there are and how long. Refused by the
/// first rule the volume breaks.
pub(super) fn read_volume(tracks: &mut Tracks, cancel: &Cancel) -> Result<Volume, MediaError> {
    let (name, root) = read_descriptor(tracks)?;
    let mut walk = Walk::new(tracks, cancel);
    walk.directory(root, None, 0)?;
    Ok(Volume {
        name,
        files: walk.files,
    })
}

/// The volume's name and the root directory's extent, as the volume descriptor at [`PVD_LBA`]
/// gives them: refused unless the descriptor is a primary one of ISO 9660 whose blocks are a
/// sector's user data, the volume's name ASCII, and its root record one the walk reads and a
/// directory's. The name is judged before the root record, as the extractor judges them.
fn read_descriptor(tracks: &mut Tracks) -> Result<(String, Extent), MediaError> {
    let descriptor = tracks.read(PVD_LBA, DESCRIPTOR_EXTENT, &Cancel::default())?;
    if !descriptor.starts_with(STANDARD_ID) || both16(&descriptor, BLOCK_SIZE_AT)? != BLOCK_SIZE {
        return Err(MediaError::disc(
            DiscRule::VolumeDescriptor,
            UNSUPPORTED_DESCRIPTOR,
        ));
    }
    let name = ascii_text(&descriptor[VOLUME_ID], VOLUME_FIELD)?;
    let (root, _) = record(&descriptor, ROOT_RECORD_AT)?;
    if root.flags & FLAG_DIRECTORY == 0 {
        return Err(MediaError::disc(
            DiscRule::RootNotDirectory,
            ROOT_NOT_DIRECTORY,
        ));
    }
    Ok((name, root.extent))
}

/// The walk of a volume's tree: the tracks it reads from, the directories it has read, every path
/// it has taken and the files it has listed.
struct Walk<'a> {
    /// The disc's data tracks, which every directory is read from.
    tracks: &'a mut Tracks,
    /// Looked at before each chunk a directory's read takes.
    cancel: &'a Cancel,
    /// The LBA of every directory read, so that none is read twice.
    visited: BTreeSet<u32>,
    /// Every path taken, a folder's or a file's, so that no two differ only in letter case.
    paths: NameSet,
    /// How many paths have been taken.
    taken: usize,
    /// The files listed, in the order the walk met them.
    files: Vec<DiscFile>,
}

impl<'a> Walk<'a> {
    /// A walk that has read nothing yet.
    fn new(tracks: &'a mut Tracks, cancel: &'a Cancel) -> Walk<'a> {
        Walk {
            tracks,
            cancel,
            visited: BTreeSet::new(),
            paths: NameSet::default(),
            taken: 0,
            files: Vec::new(),
        }
    }

    /// Walks the directory `extent` holds: the folder `folder`, `depth` below the root, or the
    /// root itself where `folder` is none. Before any of it is read, it is refused when it lies
    /// deeper than [`MAX_DIRECTORY_DEPTH`], holds more than [`MAX_DIRECTORY_LEN`] bytes, or was
    /// read already. Then it is read whole and its records are taken in turn: a NUL where a record
    /// would begin ends its sector's records, and the directory's own record and its parent's are
    /// passed over. Every other record's entry is taken by its path, its extent must lie in the
    /// data tracks, and a folder is walked at once, before the next record.
    fn directory(
        &mut self,
        extent: Extent,
        folder: Option<&RelPath>,
        depth: usize,
    ) -> Result<(), MediaError> {
        if depth > MAX_DIRECTORY_DEPTH
            || extent.size > MAX_DIRECTORY_LEN
            || !self.visited.insert(extent.lba)
        {
            return Err(MediaError::disc(DiscRule::DirectoryShape, DIRECTORY_SHAPE));
        }
        let data = self.tracks.read(extent.lba, extent.size, self.cancel)?;
        let mut at = 0;
        while let Some(&len) = data.get(at) {
            if len == 0 {
                // No record crosses into the next sector, so the rest of this one holds none.
                at = (at / SECTOR + 1) * SECTOR;
                continue;
            }
            let (entry, taken) = record(&data, at)?;
            at += taken;
            if entry.identifier == SELF_ID || entry.identifier == PARENT_ID {
                continue;
            }
            let path = self.take(folder, entry.identifier)?;
            self.tracks
                .check_extent(entry.extent.lba, entry.extent.size)?;
            if entry.flags & FLAG_DIRECTORY == 0 {
                self.files.push(DiscFile {
                    path,
                    lba: entry.extent.lba,
                    size: entry.extent.size,
                });
            } else {
                self.directory(entry.extent, Some(&path), depth + 1)?;
            }
        }
        Ok(())
    }

    /// The path of the entry `identifier` names in `folder`, or in the root where `folder` is
    /// none, taken into the walk's paths. The identifier must be ASCII and, without the version
    /// after its name, a name the extracted tree may hold by itself; joined to the folder's path,
    /// it must make a path no longer than one the tree may hold; and that path must be neither one
    /// taken already, in any letter case, nor past the [`MAX_ENTRIES`] the tree may hold.
    fn take(&mut self, folder: Option<&RelPath>, identifier: &[u8]) -> Result<RelPath, MediaError> {
        let name = str::from_utf8(identifier)
            .ok()
            .filter(|name| name.is_ascii())
            .ok_or_else(|| non_ascii(identifier))?;
        let name = without_version(name);
        check_component(name, &DISC_NAMES)
            .map_err(|refused| unsafe_name(DiscRule::EntryName, name, refused.rule))?;
        let joined = match folder {
            Some(folder) => format!("{}{PATH_SEPARATOR}{name}", folder.as_str()),
            None => name.to_owned(),
        };
        let path = RelPath::parse(&joined, &DISC_NAMES)
            .map_err(|refused| unsafe_name(DiscRule::EntryName, &joined, refused.rule))?;
        if self.taken >= MAX_ENTRIES || self.paths.insert(path.clone()).is_err() {
            return Err(MediaError::disc(DiscRule::Entries, EXCESSIVE_ENTRIES));
        }
        self.taken += 1;
        Ok(path)
    }
}

/// The directory record at `at` in `data`, a directory's bytes or the volume descriptor's, and how
/// many bytes it takes, read as the Dreamcast extractor reads one. Refused, in this order, when it
/// is shorter than [`MIN_RECORD_LEN`] or runs past `data` or into the next sector; when it has an
/// extended attribute record, interleaves its file or carries a flag outside [`ALLOWED_FLAGS`];
/// when its extent lies on another volume or its identifier runs past its end; and when a
/// both-endian number's halves differ, the volume's sequence number judged first, then the
/// extent's LBA and its size.
fn record(data: &[u8], at: usize) -> Result<(Record<'_>, usize), MediaError> {
    let len = data.get(at).map_or(0, |&len| usize::from(len));
    let within = at.checked_add(len).is_some_and(|end| end <= data.len());
    if len < MIN_RECORD_LEN || !within || at % SECTOR + len > SECTOR {
        return Err(MediaError::disc(
            DiscRule::RecordTruncated,
            TRUNCATED_RECORD,
        ));
    }
    let bytes = &data[at..at + len];
    let flags = bytes[FLAGS_AT];
    if bytes[ATTRIBUTE_LEN_AT] != 0
        || bytes[UNIT_SIZE_AT] != 0
        || bytes[INTERLEAVE_GAP_AT] != 0
        || flags & !ALLOWED_FLAGS != 0
    {
        return Err(MediaError::disc(
            DiscRule::RecordUnsupported,
            UNSUPPORTED_RECORD,
        ));
    }
    let identifier_end = IDENTIFIER_AT + usize::from(bytes[IDENTIFIER_LEN_AT]);
    if both16(bytes, SEQUENCE_AT)? != ONE_VOLUME || identifier_end > len {
        return Err(MediaError::disc(
            DiscRule::FileIdentifier,
            INVALID_IDENTIFIER,
        ));
    }
    let extent = Extent {
        lba: both32(bytes, EXTENT_LBA_AT)?,
        size: both32(bytes, EXTENT_SIZE_AT)?,
    };
    let record = Record {
        extent,
        flags,
        identifier: &bytes[IDENTIFIER_AT..identifier_end],
    };
    Ok((record, len))
}

/// The 16-bit number ISO 9660 keeps both-endian at `at` in `bytes`, little-endian and then the
/// same big-endian: refused when the two halves differ.
fn both16(bytes: &[u8], at: usize) -> Result<u16, MediaError> {
    let little = u16::from_le_bytes(bytes_at(bytes, at)?);
    let big = u16::from_be_bytes(bytes_at(bytes, at + size_of::<u16>())?);
    agreed(little, big)
}

/// The 32-bit number ISO 9660 keeps both-endian at `at` in `bytes`, as [`both16`] reads a 16-bit
/// one.
fn both32(bytes: &[u8], at: usize) -> Result<u32, MediaError> {
    let little = u32::from_le_bytes(bytes_at(bytes, at)?);
    let big = u32::from_be_bytes(bytes_at(bytes, at + size_of::<u32>())?);
    agreed(little, big)
}

/// The `N` bytes at `at` in `bytes`: refused as a truncated record when `bytes` ends first.
fn bytes_at<const N: usize>(bytes: &[u8], at: usize) -> Result<[u8; N], MediaError> {
    bytes
        .get(at..)
        .and_then(|rest| rest.first_chunk::<N>())
        .copied()
        .ok_or_else(|| MediaError::disc(DiscRule::RecordTruncated, TRUNCATED_RECORD))
}

/// `little`, a both-endian number's little-endian half, when `big`, its big-endian half, gives
/// the same number.
fn agreed<T: PartialEq>(little: T, big: T) -> Result<T, MediaError> {
    if little == big {
        Ok(little)
    } else {
        Err(MediaError::disc(DiscRule::BothEndian, MISMATCHED_HALVES))
    }
}

/// `name` without the version ISO 9660 numbers a file by, a `;` and one or more ASCII digits at its
/// end, as the Dreamcast extractor drops it. Nothing else of it changes: an earlier `;` and its
/// digits stay, and so does every letter's case.
fn without_version(name: &str) -> &str {
    match name.rsplit_once(VERSION_SEPARATOR) {
        Some((stem, version)) if is_number(version) => stem,
        _ => name,
    }
}

/// The refusal of `identifier`, which holds a byte outside ASCII and so reads as no name: quoted
/// as UTF-8 reads it, with U+FFFD for each run of bytes that is no UTF-8, up to the bound every
/// refusal quotes to, as SIZES.ROM's reader quotes such a name.
fn non_ascii(identifier: &[u8]) -> MediaError {
    let detail = quoted(NON_ASCII_IDENTIFIER, &String::from_utf8_lossy(identifier));
    MediaError::disc(DiscRule::EntryName, detail)
}

#[cfg(test)]
mod tests {
    use std::ops::Range;
    use std::path::PathBuf;

    use gdl_files::{Cancel, Job, NoProgress};
    use gdl_fixtures::{DiscSet, Edc, RAW_SECTOR, pvd_sector, raw_sector, record, unhex};
    use serde_json::json;
    use tempfile::TempDir;

    use super::{
        ALLOWED_FLAGS, BLOCK_SIZE_AT, Extent, FLAG_DIRECTORY, MAX_DIRECTORY_DEPTH,
        MAX_DIRECTORY_LEN, MAX_ENTRIES, MIN_RECORD_LEN, PVD_LBA, ROOT_RECORD_AT, STANDARD_ID,
        VOLUME_ID, Walk, read_descriptor, read_volume,
    };
    use crate::gdrom::open_head;
    use crate::vectors::{dreamcast_golden, dreamcast_vectors, located, manifest_files, number};
    use crate::vectors::{text, word};
    use crate::{DiscRules, GdRom, INSTALL_RULES, MediaError, SCRIPT_RULES, read_sheet};

    /// The LBA the format gives the volume descriptor: the sector after the boot area's sixteen.
    /// The format's facts are given here apart from the reader's own constants, so that a reader
    /// that looks elsewhere is caught.
    const FORMAT_PVD_LBA: u32 = 45_016;

    /// What the format begins a primary volume descriptor with.
    const FORMAT_STANDARD_ID: &[u8; 7] = b"\x01CD001\x01";

    /// Where the format keeps the root directory's record in the volume descriptor.
    const FORMAT_ROOT_RECORD_AT: usize = 156;

    /// Where it names the volume.
    const FORMAT_VOLUME_ID: Range<usize> = 40..72;

    /// Where it gives the size of its logical blocks, both-endian.
    const FORMAT_BLOCK_SIZE_AT: usize = 128;

    /// How many bytes of user data a sector holds: a logical block, and a one-sector directory.
    const FORMAT_SECTOR: usize = 2048;

    /// [`FORMAT_SECTOR`] as the size of an extent.
    const SECTOR_SIZE: u32 = FORMAT_SECTOR as u32;

    /// Where a record gives its extent's LBA, both-endian: little-endian, then big-endian.
    const FORMAT_LBA_AT: usize = 2;

    /// Where a record gives its extent's size, both-endian.
    const FORMAT_SIZE_AT: usize = 10;

    /// Where a record keeps its flags.
    const FORMAT_FLAGS_AT: usize = 25;

    /// Where a record gives the sequence number of its extent's volume, both-endian.
    const FORMAT_SEQUENCE_AT: usize = 28;

    /// Where a record gives its identifier's length.
    const FORMAT_IDENTIFIER_LEN_AT: usize = 32;

    /// How many bytes of a record come before its identifier.
    const FORMAT_FIXED_LEN: usize = 33;

    /// Where a record gives the length of its extended attribute record, then where it gives an
    /// interleaved file's unit size and the gap between its units.
    const FORMAT_UNREAD_FIELDS: [usize; 3] = [1, 26, 27];

    /// The flag of a directory's record.
    const FORMAT_DIRECTORY: u8 = 2;

    /// The flag of a hidden entry's record.
    const FORMAT_HIDDEN: u8 = 1;

    /// The flag of a record whose file another record continues.
    const FORMAT_MULTI_EXTENT: u8 = 128;

    /// The deepest below the root the Dreamcast extractor walks a directory.
    const SCRIPT_DEPTH: usize = 32;

    /// The most bytes it reads a directory of.
    const SCRIPT_DIRECTORY_LEN: u32 = 16 * 1024 * 1024;

    /// The most entries it takes.
    const SCRIPT_ENTRIES: usize = 100_000;

    /// The first data track's file in a disc of one file.
    const TRACK: &str = "track3.bin";

    /// The LBA of that track's first sector, at its file's start.
    const TRACK_LBA: u32 = 45_000;

    /// Where every set keeps its root directory.
    const ROOT_LBA: u32 = 45_020;

    /// Where a disc of one file keeps its file.
    const FILE_LBA: u32 = 45_017;

    /// How many bytes that file holds.
    const FILE_LEN: u32 = 5;

    /// The volume a disc of one file names.
    const VOLUME: &[u8] = b"TEST";

    /// What the extractor answers for a volume descriptor that is no primary one of 2,048-byte
    /// blocks.
    const DESCRIPTOR_WORDS: &str = "Unsupported ISO9660 volume descriptor";

    /// What it answers for a both-endian number whose halves differ.
    const MISMATCHED_WORDS: &str = "Mismatched ISO9660 little/big-endian fields";

    /// What it answers for a root record that is not a directory's.
    const ROOT_WORDS: &str = "ISO root is not a directory";

    /// What it answers for a record that is too short, or runs past its directory or its sector.
    const TRUNCATED_WORDS: &str = "Truncated ISO9660 directory record";

    /// What it answers for a record of a kind it does not read.
    const UNSUPPORTED_WORDS: &str = "Unsupported ISO9660 extended/interleaved/multiextent record";

    /// What it answers for a record of another volume, or whose identifier runs past its end.
    const IDENTIFIER_WORDS: &str = "Invalid ISO9660 file identifier";

    /// What it answers for a directory too deep, too long, or read already.
    const SHAPE_WORDS: &str = "Cyclic or oversized ISO9660 directory";

    /// What it answers for a path taken already, or one past the most.
    const ENTRIES_WORDS: &str = "Duplicate or excessive ISO9660 entries";

    /// What the launcher answers, before the identifier, for one that holds a byte outside ASCII.
    const NON_ASCII_WORDS: &str = "ISO9660 file identifier holds a byte outside ASCII";

    /// What a reader gives: what it read, or the rule and the words it refused with.
    type Outcome<T> = Result<T, (&'static str, String)>;

    /// The refusal by `rule` with `words`.
    fn said<T>(rule: &'static str, words: &str) -> Outcome<T> {
        Err((rule, words.to_owned()))
    }

    /// The refusal of `identifier`, which holds a byte outside ASCII, quoted as UTF-8 reads it,
    /// in the launcher's own words.
    fn non_ascii<T>(identifier: &[u8]) -> Outcome<T> {
        let identifier = String::from_utf8_lossy(identifier);
        Err(("entryName", format!("{NON_ASCII_WORDS}: {identifier:?}")))
    }

    /// The refusal of `name`, which breaks the rule of names `name_rule`, in the launcher's own
    /// words.
    fn unsafe_name<T>(name: &str, name_rule: &str) -> Outcome<T> {
        Err((
            "entryName",
            format!("Unsafe disc name: {name:?} ({name_rule})"),
        ))
    }

    /// The refusal of an extent that reaches the sector at `lba`, which no data track holds.
    fn outside<T>(lba: u32) -> Outcome<T> {
        Err((
            "extentOutside",
            format!("ISO extent points outside data tracks at LBA {lba}"),
        ))
    }

    /// The rule and the words of `error`, which must be a refusal by a rule of the disc's format.
    fn refusal(error: MediaError) -> (&'static str, String) {
        assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
        (error.rule(), error.to_string())
    }

    /// `set` written into a folder of its own, and its sheet's path.
    fn written(set: &DiscSet) -> (TempDir, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let sheet = set.write(dir.path()).unwrap();
        (dir, sheet)
    }

    /// What `look` makes of the disc `set` holds, opened by `rules` in a folder of its own and
    /// closed again before the folder goes; or the refusal of the disc.
    fn with_disc<T>(
        set: &DiscSet,
        rules: &DiscRules,
        look: impl FnOnce(&GdRom) -> T,
    ) -> Outcome<T> {
        let (_dir, sheet) = written(set);
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        GdRom::open(&sheet, rules, &job)
            .map(|disc| look(&disc))
            .map_err(refusal)
    }

    /// The paths of the files the walk of the disc `set` holds lists, under the extractor's rules.
    fn paths(set: &DiscSet) -> Outcome<Vec<String>> {
        with_disc(set, &SCRIPT_RULES, |disc| {
            let files = disc.files().iter();
            files.map(|file| file.path.as_str().to_owned()).collect()
        })
    }

    /// The paths `paths` gives for a disc whose walk lists `expected`.
    fn listed(expected: &[&str]) -> Outcome<Vec<String>> {
        Ok(expected.iter().map(|path| (*path).to_owned()).collect())
    }

    /// A disc of one file, which the root lists by `identifier`.
    fn one_file(identifier: &[u8]) -> DiscSet {
        DiscSet::one_file(identifier, &[], Edc::Zero)
    }

    /// `set` with raw sectors, each holding as its user data one of `payloads`, written over its
    /// first data track's file from the sector at `lba`, as a chunk written after the set's own,
    /// which it wins over. The file is lengthened to hold them where it ends first.
    fn with_sectors(mut set: DiscSet, lba: u32, payloads: &[&[u8]]) -> DiscSet {
        let track = set.file_mut(TRACK);
        let place = u64::from(lba - TRACK_LBA) * RAW_SECTOR as u64;
        let sectors: Vec<u8> = (lba..)
            .zip(payloads)
            .flat_map(|(lba, payload)| raw_sector(lba, payload, Edc::Zero))
            .collect();
        track.len = track.len.max(place + sectors.len() as u64);
        track.chunks.push((place, sectors));
        set
    }

    /// A disc of one file whose volume descriptor is `descriptor` instead.
    fn with_descriptor(descriptor: &[u8]) -> DiscSet {
        with_sectors(one_file(b"A;1"), FORMAT_PVD_LBA, &[descriptor])
    }

    /// The volume descriptor of a disc of one file: its root directory a sector at 45020.
    fn descriptor() -> Vec<u8> {
        pvd_sector(VOLUME, ROOT_LBA, SECTOR_SIZE)
    }

    /// A disc of one file whose volume descriptor gives its root directory `size` bytes, and whose
    /// sectors from 45020 hold `directory` instead, a sector's user data each.
    fn with_root(directory: &[u8], size: u32) -> DiscSet {
        let set = with_descriptor(&pvd_sector(VOLUME, ROOT_LBA, size));
        let sectors: Vec<&[u8]> = directory.chunks(FORMAT_SECTOR).collect();
        with_sectors(set, ROOT_LBA, &sectors)
    }

    /// A disc of one file whose root directory, of one sector, holds `entries` instead of its one
    /// file.
    fn rooted(entries: &[Vec<u8>]) -> DiscSet {
        with_root(&directory(ROOT_LBA, ROOT_LBA, entries), SECTOR_SIZE)
    }

    /// The records of the directory at `lba`, whose parent is at `parent`: its own record and its
    /// parent's, then `entries`.
    fn directory(lba: u32, parent: u32, entries: &[Vec<u8>]) -> Vec<u8> {
        let own = record(b"\0", lba, SECTOR_SIZE, FORMAT_DIRECTORY);
        let up = record(b"\x01", parent, SECTOR_SIZE, FORMAT_DIRECTORY);
        [&[own, up], entries].concat().concat()
    }

    /// The record of a file the identifier `identifier` names: the five bytes a disc of one file
    /// keeps at 45017.
    fn file(identifier: &[u8]) -> Vec<u8> {
        record(identifier, FILE_LBA, FILE_LEN, 0)
    }

    /// The record of a folder the identifier `identifier` names, a directory of one sector at
    /// `lba`.
    fn folder(identifier: &[u8], lba: u32) -> Vec<u8> {
        record(identifier, lba, SECTOR_SIZE, FORMAT_DIRECTORY)
    }

    /// The records of a root directory that end exactly at `end`: its own and its parent's, then
    /// files' of 40 bytes each, the last a file's whose identifier makes up the rest.
    fn records_to(end: usize) -> Vec<u8> {
        let mut records = directory(ROOT_LBA, ROOT_LBA, &[]);
        let filler = file(b"F0000;1").len();
        let mut count = 0;
        while end - records.len() >= filler + MIN_RECORD_LEN {
            records.extend(file(format!("F{count:04};1").as_bytes()));
            count += 1;
        }
        let rest = end - records.len();
        records.extend(file("G".repeat(rest - FORMAT_FIXED_LEN).as_bytes()));
        assert_eq!(records.len(), end);
        records
    }

    #[test]
    fn the_volumes_places_and_bounds_are_the_formats() {
        assert_eq!(PVD_LBA, FORMAT_PVD_LBA);
        assert_eq!(STANDARD_ID, FORMAT_STANDARD_ID);
        assert_eq!(VOLUME_ID, FORMAT_VOLUME_ID);
        assert_eq!(
            (ROOT_RECORD_AT, BLOCK_SIZE_AT),
            (FORMAT_ROOT_RECORD_AT, FORMAT_BLOCK_SIZE_AT)
        );
        // The fixed fields and an identifier of one byte.
        assert_eq!(MIN_RECORD_LEN, FORMAT_FIXED_LEN + 1);
        assert_eq!(FLAG_DIRECTORY, FORMAT_DIRECTORY);
        assert_eq!(ALLOWED_FLAGS, FORMAT_DIRECTORY | FORMAT_HIDDEN);
        // The bounds the Dreamcast extractor walks a tree within.
        assert_eq!(
            (MAX_DIRECTORY_DEPTH, MAX_DIRECTORY_LEN, MAX_ENTRIES),
            (SCRIPT_DEPTH, SCRIPT_DIRECTORY_LEN, SCRIPT_ENTRIES)
        );
    }

    #[test]
    fn records_read_as_the_shared_vectors_pin() {
        // The records the extractor tests' builder writes: a file's, whose identifier of even
        // length a pad byte follows, a directory's own, and a folder's.
        let vectors = dreamcast_vectors();
        let cases = vectors["records"].as_array().unwrap();
        assert_eq!(cases.len(), 3);
        for case in cases {
            let bytes = unhex(&case["hex"]);
            let (read, len) = super::record(&bytes, 0).unwrap();
            assert_eq!(len, bytes.len(), "{case}");
            assert_eq!(read.identifier, text(&case["name"]).as_bytes(), "{case}");
            let extent = Extent {
                lba: word(&case["lba"]),
                size: word(&case["size"]),
            };
            assert_eq!(read.extent, extent, "{case}");
            assert_eq!(
                u64::from(read.flags),
                case["flags"].as_u64().unwrap(),
                "{case}"
            );
            // The bytes past the record are no part of it.
            let longer = [bytes.clone(), vec![0xFF; 40]].concat();
            let (again, len) = super::record(&longer, 0).unwrap();
            assert_eq!((again.extent, len), (extent, bytes.len()), "{case}");
        }
    }

    #[test]
    fn the_walk_lists_files_in_record_order() {
        let vectors = dreamcast_vectors();
        let golden = dreamcast_golden(&vectors["sets"]["full"]["goldens"]["disc-manifest.json"]);
        let expected = manifest_files(&golden);
        // The full set's root lists the folder AUDIO before its three files: the folder is walked
        // as soon as its record is met, so its one file comes first, as the extractor lists it.
        let order: Vec<&str> = expected
            .as_array()
            .unwrap()
            .iter()
            .map(|row| text(&row["path"]))
            .collect();
        assert_eq!(order, ["AUDIO/X.STR", "DISK.ROM", "EMPTY.BIN", "SIZES.ROM"]);
        // Behind its cue sheet and its GDI, under the extractor's rules and an installation's.
        let sets = [
            (DiscSet::full(Edc::Zero), SCRIPT_RULES),
            (
                DiscSet::full(Edc::Computed).with_audio_files(),
                INSTALL_RULES,
            ),
            (DiscSet::full_gdi(Edc::Zero), SCRIPT_RULES),
            (
                DiscSet::full_gdi(Edc::Computed).with_audio_files(),
                INSTALL_RULES,
            ),
        ];
        for (set, rules) in sets {
            let walked = with_disc(&set, &rules, |disc| {
                (disc.volume().to_owned(), located(disc.files()))
            });
            let (volume, files) = walked.unwrap();
            assert_eq!(volume, text(&golden["volume"]), "{}", set.sheet);
            assert_eq!(files, expected, "{}", set.sheet);
        }
        // Records are taken in the order their directory keeps them, never sorted.
        let unsorted = rooted(&[file(b"B;1"), file(b"A;1"), file(b"C;1")]);
        assert_eq!(paths(&unsorted), listed(&["B", "A", "C"]));
        // A folder's files come where its record stands, and a folder that holds nothing lists
        // nothing.
        let folders = rooted(&[
            file(b"A;1"),
            folder(b"X", 45_021),
            file(b"B;1"),
            folder(b"Y", 45_022),
        ]);
        let folders = with_sectors(
            folders,
            45_021,
            &[
                &directory(45_021, ROOT_LBA, &[file(b"C;1"), file(b"D;1")]),
                &directory(45_022, ROOT_LBA, &[]),
            ],
        );
        assert_eq!(paths(&folders), listed(&["A", "X/C", "X/D", "B"]));
    }

    #[test]
    fn version_suffixes_go_and_nothing_else_changes() {
        // A version is a `;` and ASCII digits at the identifier's end: it goes, and nothing else
        // changes, an earlier version and every letter's case included.
        let cases: [(&[u8], &str); 14] = [
            (b"A;1", "A"),
            (b"A.B;32767", "A.B"),
            (b"A.B;1;2", "A.B;1"),
            (b"a.b;1", "a.b"),
            (b"Mixed.Case;7", "Mixed.Case"),
            (b"A;0001", "A"),
            (b"A;;1", "A;"),
            (b"A", "A"),
            (b"A;", "A;"),
            (b"A;x", "A;x"),
            (b"A;1x", "A;1x"),
            (b"A; 1", "A; 1"),
            (b"A;-1", "A;-1"),
            (b"A;+1", "A;+1"),
        ];
        for (identifier, path) in cases {
            let given = paths(&one_file(identifier));
            assert_eq!(given, listed(&[path]), "{}", identifier.escape_ascii());
        }
        // What the version leaves must be a name the tree may hold by itself.
        let refusals: [(&[u8], &str, &str); 5] = [
            (b"README.;1", "README.", "trailingDotOrSpace"),
            (b"README ;1", "README ", "trailingDotOrSpace"),
            (b";1", "", "empty"),
            (b".;1", ".", "dot"),
            (b"..;2", "..", "dot"),
        ];
        for (identifier, name, rule) in refusals {
            let given = paths(&one_file(identifier));
            assert_eq!(
                given,
                unsafe_name(name, rule),
                "{}",
                identifier.escape_ascii()
            );
        }
        // A folder's identifier loses a version the same way.
        let versioned = DiscSet::one_file(b"F;1", &[b"D;1"], Edc::Zero);
        assert_eq!(paths(&versioned), listed(&["D/F"]));
    }

    #[test]
    fn names_follow_the_shared_vectors() {
        let vectors = dreamcast_vectors();
        let cases = vectors["names"].as_array().unwrap();
        assert!(!cases.is_empty());
        let mut deviations = Vec::new();
        for case in cases {
            // The name as the file's identifier, at version 1, as the cases' disc lists it.
            let name = text(&case["name"]);
            let identifier = format!("{name};1");
            let iso = &case["iso"];
            match (
                paths(&one_file(identifier.as_bytes())),
                iso["rule"].as_str(),
            ) {
                (Ok(paths), None) => {
                    assert!(iso["python"].is_null(), "{case}");
                    assert_eq!(paths, [name], "{case}");
                }
                (Err((rule, detail)), Some(expected)) => {
                    assert_eq!(rule, expected, "{case}");
                    let words = match iso["nameRule"].as_str() {
                        // The launcher's own words, which quote the name and name the rule of
                        // names it breaks.
                        Some(name_rule) => format!("Unsafe disc name: {name:?} ({name_rule})"),
                        // An identifier outside ASCII reads as no name at all, so it is refused
                        // before any rule of names is judged.
                        None => format!("{NON_ASCII_WORDS}: {identifier:?}"),
                    };
                    assert_eq!(detail, words, "{case}");
                }
                (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
            }
            deviations.extend(iso["deviation"].as_u64());
        }
        // The names the extractor takes and the launcher refuses: a delete character, a name of
        // more than 200 characters, the alpha installer's names, and a device name followed by
        // spaces.
        deviations.sort_unstable();
        deviations.dedup();
        assert_eq!(deviations, [1, 2, 3, 4]);
    }

    #[test]
    fn paths_end_at_220_characters() {
        let vectors = dreamcast_vectors();
        let cases = vectors["isoPaths"].as_array().unwrap();
        assert_eq!(cases.len(), 2);
        for case in cases {
            // The path's folders, each named without a version, and its file at version 1.
            let path = text(&case["path"]);
            assert_eq!(path.chars().count(), number(&case["chars"]), "{case}");
            let components: Vec<&str> = path.split('/').collect();
            let (name, folders) = components.split_last().unwrap();
            assert_eq!(folders.len(), number(&case["depth"]), "{case}");
            let folders: Vec<&[u8]> = folders.iter().map(|folder| folder.as_bytes()).collect();
            let identifier = format!("{name};1");
            let set = DiscSet::one_file(identifier.as_bytes(), &folders, Edc::Zero);
            match (paths(&set), case["rule"].as_str()) {
                (Ok(paths), None) => {
                    assert!(case["python"].is_null(), "{case}");
                    assert_eq!(paths, [path], "{case}");
                }
                (Err((rule, detail)), Some(expected)) => {
                    assert_eq!(rule, expected, "{case}");
                    let name_rule = text(&case["nameRule"]);
                    assert_eq!(detail, format!("Unsafe disc name: {path:?} ({name_rule})"));
                    // The extractor takes the longer path: the launcher's bound is its own.
                    assert!(case["python"].is_null(), "{case}");
                    assert_eq!(case["deviation"], 2, "{case}");
                }
                (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
            }
        }
        // A folder's path is held to the bound too, as its record is met: one of 221 characters
        // is refused before it is walked, and one of 218 holds a file whose path makes up 220.
        let long = "a".repeat(200);
        let longer = DiscSet::one_file(b"F;1", &[long.as_bytes(), &[b'b'; 20]], Edc::Zero);
        let joined = format!("{long}/{}", "b".repeat(20));
        assert_eq!(paths(&longer), unsafe_name(&joined, "pathTooLong"));
        let fits = DiscSet::one_file(b"F;1", &[long.as_bytes(), &[b'b'; 17]], Edc::Zero);
        let path = format!("{long}/{}/F", "b".repeat(17));
        assert_eq!(path.len(), 220);
        assert_eq!(paths(&fits), listed(&[&path]));
    }

    #[test]
    fn unsafe_names_are_refused() {
        // The extractor's test: the small set's file named to escape the tree.
        let escape = DiscSet::small_with(b"../escape;1", 45_100, 0, Edc::Zero);
        assert_eq!(paths(&escape), unsafe_name("../escape", "forbidden"));
        // An identifier that holds a separator is one name, refused, never a folder holding a
        // file: a file's, and a folder's before its directory is walked.
        assert_eq!(paths(&one_file(b"A/B;1")), unsafe_name("A/B", "forbidden"));
        let foldered = DiscSet::one_file(b"F;1", &[b"A/B"], Edc::Zero);
        assert_eq!(paths(&foldered), unsafe_name("A/B", "forbidden"));
        // A folder named as a directory names itself or its parent is refused, before its extent
        // is judged, though it lies where no data track does.
        let dots = rooted(&[record(b"..", 999_999, SECTOR_SIZE, FORMAT_DIRECTORY)]);
        assert_eq!(paths(&dots), unsafe_name("..", "dot"));
        // A byte outside ASCII is judged before the rules of names, whatever else the identifier
        // breaks, and the identifier is quoted as SIZES.ROM's reader quotes a name: a character
        // written in UTF-8 as it is written, and a byte that is no UTF-8 as U+FFFD.
        let words = format!("{NON_ASCII_WORDS}: \"trak\u{e9}.bin;1\"");
        let identifier = "trak\u{e9}.bin;1".as_bytes();
        assert_eq!(paths(&one_file(identifier)), said("entryName", &words));
        let words = format!("{NON_ASCII_WORDS}: \"\u{fffd}/..;1\"");
        assert_eq!(paths(&one_file(b"\xe9/..;1")), said("entryName", &words));
        // Bytes past ASCII's that begin no character, or a character cut short, alone and between
        // two letters, at version 1.
        for byte in [0x80, 0xC3, 0xFF] {
            for identifier in [vec![byte, b';', b'1'], vec![b'a', byte, b'b', b';', b'1']] {
                let given = paths(&one_file(&identifier));
                assert_eq!(given, non_ascii(&identifier), "{byte:#x}");
            }
        }
        // A control character, with the name's other bytes, is quoted as the name.
        assert_eq!(paths(&one_file(b"A\tB;1")), unsafe_name("A\tB", "control"));
    }

    #[test]
    fn extended_interleaved_and_multiextent_records_are_refused() {
        let unsupported = said("recordUnsupported", UNSUPPORTED_WORDS);
        // The extractor's test: the small set's file flagged as one continued in another record.
        let flagged = |flags: u8| DiscSet::small_with(b"TEST.WAD;1", 45_100, flags, Edc::Zero);
        assert_eq!(paths(&flagged(FORMAT_MULTI_EXTENT)), unsupported);
        // Every flag but hidden and directory, alone or beside hidden.
        for bit in 2..u8::BITS {
            let flag = 1 << bit;
            for flags in [flag, flag | FORMAT_HIDDEN] {
                assert_eq!(paths(&flagged(flags)), unsupported, "{flags}");
            }
        }
        // A hidden file is listed as any other.
        assert_eq!(paths(&flagged(FORMAT_HIDDEN)), listed(&["TEST.WAD"]));
        // An extended attribute record's length, an interleaved file's unit size or its gap.
        for at in FORMAT_UNREAD_FIELDS {
            let mut entry = file(b"A;1");
            entry[at] = 1;
            assert_eq!(paths(&rooted(&[entry])), unsupported, "{at}");
        }
        // Every record is judged before its identifier is: the directory's own, which the walk
        // passes over, and the root's in the volume descriptor.
        let mut own = directory(ROOT_LBA, ROOT_LBA, &[file(b"A;1")]);
        own[FORMAT_FLAGS_AT] |= FORMAT_MULTI_EXTENT;
        assert_eq!(paths(&with_root(&own, SECTOR_SIZE)), unsupported);
        let mut root = descriptor();
        root[FORMAT_ROOT_RECORD_AT + FORMAT_FLAGS_AT] |= FORMAT_MULTI_EXTENT;
        assert_eq!(paths(&with_descriptor(&root)), unsupported);
        // A record of a kind the reader does not read is refused for that before its numbers.
        let mut both = file(b"A;1");
        both[FORMAT_UNREAD_FIELDS[1]] = 1;
        both[FORMAT_LBA_AT] ^= 1;
        assert_eq!(paths(&rooted(&[both])), unsupported);
    }

    #[test]
    fn truncated_and_crossing_records_are_refused() {
        let truncated = said("recordTruncated", TRUNCATED_WORDS);
        // A record shorter than its fixed fields and an identifier of a byte.
        for len in [1, 2, 33] {
            let mut entry = file(b"A;1");
            entry[0] = len;
            assert_eq!(paths(&rooted(&[entry])), truncated, "{len}");
        }
        // One that runs past the end of its directory, as the volume descriptor gives its size.
        let records = directory(ROOT_LBA, ROOT_LBA, &[file(b"A;1")]);
        let size = u32::try_from(records.len()).unwrap();
        assert_eq!(paths(&with_root(&records, size - 1)), truncated);
        assert_eq!(paths(&with_root(&records, size)), listed(&["A"]));
        // One that would cross into the next sector, the records before it filling the first
        // sector to its 2,040th byte.
        let mut crossing = records_to(2040);
        crossing.extend(file(b"Z;1"));
        assert_eq!(paths(&with_root(&crossing, 2 * SECTOR_SIZE)), truncated);
        // One that ends at the sector's last byte is read, and so is the next sector's first.
        let mut meeting = records_to(2014);
        meeting.extend(file(b"Y"));
        assert_eq!(meeting.len(), FORMAT_SECTOR);
        meeting.extend(file(b"Z;1"));
        let given = paths(&with_root(&meeting, 2 * SECTOR_SIZE)).unwrap();
        assert_eq!(given[given.len() - 2..], ["Y", "Z"]);
        // A NUL where a record would begin ends its sector's records: what follows it in the
        // sector is never read, and the next sector's records are.
        let mut padded = directory(ROOT_LBA, ROOT_LBA, &[file(b"A;1")]);
        padded.push(0);
        padded.extend([0xFF; 100]);
        padded.resize(FORMAT_SECTOR, 0);
        padded.extend(file(b"B;1"));
        assert_eq!(
            paths(&with_root(&padded, 2 * SECTOR_SIZE)),
            listed(&["A", "B"])
        );
        // The root's record in the volume descriptor too, which no NUL passes over.
        for len in [0, 33] {
            let mut root = descriptor();
            root[FORMAT_ROOT_RECORD_AT] = len;
            assert_eq!(paths(&with_descriptor(&root)), truncated, "{len}");
        }
    }

    #[test]
    fn cycles_depth_and_size_are_bounded() {
        let shape = said("directoryShape", SHAPE_WORDS);
        // A folder whose extent is the root's: the walk would read the root again.
        let looped = DiscSet::small_with(b"LOOP", ROOT_LBA, FORMAT_DIRECTORY, Edc::Zero);
        assert_eq!(paths(&looped), shape);
        // A folder that holds itself, and two folders that share one extent.
        let itself = with_sectors(
            rooted(&[folder(b"A", 45_021)]),
            45_021,
            &[&directory(45_021, ROOT_LBA, &[folder(b"B", 45_021)])],
        );
        assert_eq!(paths(&itself), shape);
        let shared = with_sectors(
            rooted(&[folder(b"A", 45_021), folder(b"B", 45_021)]),
            45_021,
            &[&directory(45_021, ROOT_LBA, &[file(b"F;1")])],
        );
        assert_eq!(paths(&shared), shape);
        // Thirty-two folders deep is as deep as a directory may lie; one more is refused.
        let deep: Vec<&[u8]> = vec![b"D"; SCRIPT_DEPTH];
        let deepest = format!("{}F", "D/".repeat(SCRIPT_DEPTH));
        let given = paths(&DiscSet::one_file(b"F;1", &deep, Edc::Zero));
        assert_eq!(given, listed(&[&deepest]));
        let deeper: Vec<&[u8]> = vec![b"D"; SCRIPT_DEPTH + 1];
        assert_eq!(paths(&DiscSet::one_file(b"F;1", &deeper, Edc::Zero)), shape);
        // A directory of more bytes than the most is refused before a sector of it is read: the
        // root, whose one sector would otherwise be read and the next looked for.
        let records = directory(ROOT_LBA, ROOT_LBA, &[file(b"A;1")]);
        let longest = with_root(&records, SCRIPT_DIRECTORY_LEN);
        assert_eq!(paths(&longest), outside(45_021));
        let longer = with_root(&records, SCRIPT_DIRECTORY_LEN + 1);
        assert_eq!(paths(&longer), shape);
    }

    #[test]
    fn duplicates_and_the_entry_bound_are_refused() {
        let entries = said("entries", ENTRIES_WORDS);
        // Two versions of one file, and one name in two letter cases, are one path.
        assert_eq!(paths(&rooted(&[file(b"A;1"), file(b"A;2")])), entries);
        let cases = rooted(&[file(b"README.TXT;1"), file(b"readme.txt;1")]);
        assert_eq!(paths(&cases), entries);
        // So are a file and a folder of one name, whichever comes first.
        let empty = directory(45_021, ROOT_LBA, &[]);
        let file_first = rooted(&[file(b"A;1"), folder(b"a", 45_021)]);
        assert_eq!(paths(&with_sectors(file_first, 45_021, &[&empty])), entries);
        let folder_first = rooted(&[folder(b"a", 45_021), file(b"A;1")]);
        assert_eq!(
            paths(&with_sectors(folder_first, 45_021, &[&empty])),
            entries
        );
        // One name in two folders is two paths.
        let apart = rooted(&[folder(b"X", 45_021), file(b"A;1")]);
        let apart = with_sectors(
            apart,
            45_021,
            &[&directory(45_021, ROOT_LBA, &[file(b"A;1")])],
        );
        assert_eq!(paths(&apart), listed(&["X/A", "A"]));

        // The bound counts folders and files alike: a folder and its file, walked with room for
        // two more entries, are taken; with room for one, the folder is and its file is refused.
        let (_dir, sheet) = written(&DiscSet::one_file(b"F;1", &[b"D"], Edc::Zero));
        let cancel = Cancel::default();
        let (_, mut tracks, _) = open_head(read_sheet(&sheet).unwrap(), &SCRIPT_RULES).unwrap();
        let (_, root) = read_descriptor(&mut tracks).unwrap();
        {
            let mut walk = Walk {
                taken: SCRIPT_ENTRIES - 2,
                ..Walk::new(&mut tracks, &cancel)
            };
            walk.directory(root, None, 0).unwrap();
            assert_eq!(walk.taken, SCRIPT_ENTRIES);
            assert_eq!(walk.files.len(), 1);
        }
        let mut walk = Walk {
            taken: SCRIPT_ENTRIES - 1,
            ..Walk::new(&mut tracks, &cancel)
        };
        let walked = walk.directory(root, None, 0).map_err(refusal);
        assert_eq!(walked, said("entries", ENTRIES_WORDS));
        assert!(walk.files.is_empty());
    }

    #[test]
    fn a_zero_size_file_may_lie_anywhere() {
        // The full set's EMPTY.BIN lies at an LBA no track holds, and is listed there: an extent
        // of no bytes takes no sector.
        let empty = with_disc(&DiscSet::full(Edc::Zero), &SCRIPT_RULES, |disc| {
            let mut files = disc.files().iter();
            files
                .find(|file| file.path.as_str() == "EMPTY.BIN")
                .map(|file| (file.lba, file.size))
        });
        assert_eq!(empty, Ok(Some((999_999, 0))));
        // So may one at the first LBA or the last.
        let anywhere = rooted(&[
            record(b"Z;1", 0, 0, 0),
            record(b"Y;1", u32::MAX, 0, 0),
            record(b"X;1", 999_999, 0, 0),
        ]);
        let given = with_disc(&anywhere, &SCRIPT_RULES, |disc| located(disc.files()));
        let expected = json!([
            {"path": "Z", "lba": 0, "size": 0},
            {"path": "Y", "lba": u32::MAX, "size": 0},
            {"path": "X", "lba": 999_999, "size": 0}
        ]);
        assert_eq!(given, Ok(expected));
        // A byte more, and its extent must lie in a data track.
        let one_byte = rooted(&[record(b"X;1", 999_999, 1, 0)]);
        assert_eq!(paths(&one_byte), outside(999_999));
        // A folder of no bytes holds nothing, wherever it lies.
        let hollow = rooted(&[record(b"E", 999_999, 0, FORMAT_DIRECTORY), file(b"A;1")]);
        assert_eq!(paths(&hollow), listed(&["A"]));
    }

    #[test]
    fn mismatched_both_endian_fields_are_refused() {
        let mismatched = said("bothEndian", MISMATCHED_WORDS);
        // Each half of a file record's LBA, size and volume sequence number: the first byte of
        // its little-endian half and the last of its big-endian one.
        let places = [
            FORMAT_LBA_AT,
            FORMAT_LBA_AT + 7,
            FORMAT_SIZE_AT,
            FORMAT_SIZE_AT + 7,
            FORMAT_SEQUENCE_AT,
            FORMAT_SEQUENCE_AT + 3,
        ];
        for at in places {
            let mut entry = file(b"A;1");
            entry[at] ^= 1;
            assert_eq!(paths(&rooted(&[entry])), mismatched, "{at}");
        }
        // The directory's own record, which the walk passes over, and the root's record in the
        // volume descriptor.
        let mut own = directory(ROOT_LBA, ROOT_LBA, &[file(b"A;1")]);
        own[FORMAT_SIZE_AT + 7] ^= 1;
        assert_eq!(paths(&with_root(&own, SECTOR_SIZE)), mismatched);
        let mut root = descriptor();
        root[FORMAT_ROOT_RECORD_AT + FORMAT_LBA_AT + 7] ^= 1;
        assert_eq!(paths(&with_descriptor(&root)), mismatched);
        // The size of the volume descriptor's blocks.
        let mut block = descriptor();
        block[FORMAT_BLOCK_SIZE_AT + 2] ^= 1;
        assert_eq!(paths(&with_descriptor(&block)), mismatched);
    }

    #[test]
    fn a_record_of_another_volume_or_a_long_identifier_is_refused() {
        let invalid = said("fileIdentifier", IDENTIFIER_WORDS);
        // A volume sequence number other than the disc's one volume's, its halves agreeing.
        for sequence in [0, 2, u16::MAX] {
            let mut entry = file(b"A;1");
            let place = FORMAT_SEQUENCE_AT..FORMAT_SEQUENCE_AT + 4;
            entry[place].copy_from_slice(&gdl_fixtures::both16(sequence));
            assert_eq!(paths(&rooted(&[entry])), invalid, "{sequence}");
        }
        // An identifier that runs past its record: three bytes take a record of 36 to its end,
        // and four run past it.
        let mut entry = file(b"A;1");
        assert_eq!(entry.len(), 36);
        entry[FORMAT_IDENTIFIER_LEN_AT] = 4;
        assert_eq!(paths(&rooted(&[entry])), invalid);
        // An identifier is as long as its length gives, the record's pad byte no part of it.
        let mut shorter = file(b"AB;1");
        shorter[FORMAT_IDENTIFIER_LEN_AT] = 3;
        assert_eq!(paths(&rooted(&[shorter])), listed(&["AB;"]));
    }

    #[test]
    fn the_volume_descriptor_is_held_to_the_format() {
        let unsupported = said("volumeDescriptor", DESCRIPTOR_WORDS);
        // Each byte of what a primary descriptor begins with: its type, the standard's identifier
        // and its version.
        for at in 0..FORMAT_STANDARD_ID.len() {
            let mut changed = descriptor();
            changed[at] ^= 1;
            assert_eq!(paths(&with_descriptor(&changed)), unsupported, "{at}");
        }
        // Blocks of another size than a sector's user data, the halves agreeing.
        for size in [0, 512, 4096] {
            let mut changed = descriptor();
            let place = FORMAT_BLOCK_SIZE_AT..FORMAT_BLOCK_SIZE_AT + 4;
            changed[place].copy_from_slice(&gdl_fixtures::both16(size));
            assert_eq!(paths(&with_descriptor(&changed)), unsupported, "{size}");
        }
        // The identifier is judged before the block size's halves.
        let mut both = descriptor();
        both[FORMAT_BLOCK_SIZE_AT + 2] ^= 1;
        both[1] ^= 1;
        assert_eq!(paths(&with_descriptor(&both)), unsupported);

        // The volume's name is read as the extractor reads it: ASCII, the white space at either
        // end stripped, NULs and what lies within kept.
        let volume = |name: &[u8]| {
            let set = with_descriptor(&pvd_sector(name, ROOT_LBA, SECTOR_SIZE));
            with_disc(&set, &SCRIPT_RULES, |disc| disc.volume().to_owned())
        };
        assert_eq!(volume(b"GAUNT1"), Ok("GAUNT1".to_owned()));
        assert_eq!(volume(b"\x1c\x1dTE ST\t\x1e"), Ok("TE ST".to_owned()));
        assert_eq!(volume(b"TEST\0"), Ok("TEST\0".to_owned()));
        assert_eq!(volume(b""), Ok(String::new()));
        let outside_ascii = said(
            "textField",
            "ISO9660 volume identifier holds a byte outside ASCII",
        );
        for at in [FORMAT_VOLUME_ID.start, 45, FORMAT_VOLUME_ID.end - 1] {
            let mut changed = descriptor();
            changed[at] = 0xE9;
            assert_eq!(paths(&with_descriptor(&changed)), outside_ascii, "{at}");
        }
        // So does a character outside ASCII written in UTF-8: at the name's start, and in the
        // padding at its end, where an ideographic space would otherwise be stripped as white
        // space.
        for character in ["\u{e9}", "\u{3000}"] {
            let bytes = character.as_bytes();
            for at in [FORMAT_VOLUME_ID.start, FORMAT_VOLUME_ID.end - bytes.len()] {
                let mut changed = descriptor();
                changed[at..at + bytes.len()].copy_from_slice(bytes);
                let given = paths(&with_descriptor(&changed));
                assert_eq!(given, outside_ascii, "{at} {character:?}");
            }
        }
        // A byte outside ASCII just past the name is no part of it.
        let mut past = descriptor();
        past[FORMAT_VOLUME_ID.end] = 0xE9;
        assert!(paths(&with_descriptor(&past)).is_ok());

        // The root's record must be a directory's: without its flag, or hidden alone, it is not.
        let root_flags = FORMAT_ROOT_RECORD_AT + FORMAT_FLAGS_AT;
        for flags in [0, FORMAT_HIDDEN] {
            let mut changed = descriptor();
            changed[root_flags] = flags;
            assert_eq!(
                paths(&with_descriptor(&changed)),
                said("rootNotDirectory", ROOT_WORDS)
            );
        }
        let mut hidden = descriptor();
        hidden[root_flags] = FORMAT_DIRECTORY | FORMAT_HIDDEN;
        assert_eq!(paths(&with_descriptor(&hidden)), listed(&["A"]));
        // The name is judged before the root's record.
        let mut both = descriptor();
        both[FORMAT_VOLUME_ID.start] = 0xE9;
        both[root_flags] = 0;
        assert_eq!(paths(&with_descriptor(&both)), outside_ascii);

        // A descriptor no data track holds: the first one ends with the boot area.
        let mut short = one_file(b"A;1");
        let (_, track) = short
            .files
            .iter_mut()
            .find(|(name, _)| name == TRACK)
            .unwrap();
        track.len = 16 * RAW_SECTOR as u64;
        assert_eq!(paths(&short), outside(FORMAT_PVD_LBA));
    }

    #[test]
    fn the_walk_stops_when_its_job_is_cancelled() {
        let (_dir, sheet) = written(&DiscSet::one_file(b"F;1", &[b"D"], Edc::Zero));
        let going = Cancel::default();
        let stopped = Cancel::default();
        stopped.cancel();
        let (_, mut tracks, _) = open_head(read_sheet(&sheet).unwrap(), &SCRIPT_RULES).unwrap();
        // The volume's reads stop before the root directory's, once the descriptor's one sector
        // is read, and a walk before the directory it is given.
        let volume = read_volume(&mut tracks, &stopped);
        assert!(matches!(volume, Err(MediaError::Cancelled)), "{volume:?}");
        let (_, root) = read_descriptor(&mut tracks).unwrap();
        {
            let mut walk = Walk::new(&mut tracks, &stopped);
            let walked = walk.directory(root, None, 0);
            assert!(matches!(walked, Err(MediaError::Cancelled)), "{walked:?}");
        }
        let volume = read_volume(&mut tracks, &going).unwrap();
        let files: Vec<&str> = volume.files.iter().map(|file| file.path.as_str()).collect();
        assert_eq!((volume.name.as_str(), files), ("TEST", vec!["D/F"]));
    }
}
