//! The Dreamcast GD-ROM as the Dreamcast extractor reads it: the sheet that lists the dump's
//! track files, a cue sheet or a GDI, and the raw sectors those files keep, each placed by the
//! address its own header gives and guarded by the error detection code it keeps after its user
//! data. A data track is placed by its first sector's header, never by where the sheet lists it,
//! and every extent of the disc, its boot area first, is read by LBA from the tracks that hold it.
//! The disc's files are those of the ISO 9660 volume its high-density area holds, walked from
//! the volume descriptor after the boot area; the native files its DISK.ROM packs are those the
//! table SIZES.ROM lists beside it.

use std::fmt;
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use gdl_files::{Cancel, Job, NameRule, RelPath};
use sha2::{Digest, Sha256};

use crate::dump_file::open_regular;
use crate::error::quoted;
use crate::{DiscRule, MediaError, Sha256Digest};

mod cue;
mod edc;
mod gdi;
mod ipbin;
mod iso;
mod packed;
mod sector;
mod sheet;
mod sheet_text;

pub use edc::{EDC_FIELD, EDC_POLYNOMIAL, EDC_SPAN, edc};
pub(crate) use ipbin::names_gauntlet_legends;
pub use ipbin::{
    AREA, BOOT_LBA, BOOT_LEN, BOOT_SECTORS, EXECUTABLE, HARDWARE_ID, IpBin, PRODUCT, TITLE,
    TITLE_PREFIX, VERSION,
};
pub use iso::{
    ALLOWED_FLAGS, BLOCK_SIZE_AT, FLAG_DIRECTORY, MAX_DIRECTORY_DEPTH, MAX_DIRECTORY_LEN,
    MAX_ENTRIES, MIN_RECORD_LEN, PVD_LBA, ROOT_RECORD_AT, STANDARD_ID, VOLUME_ID,
};
pub use packed::{
    MAX_SIZES_ROM_LEN, PackedFile, PackedPlace, SIZES_COMPONENTS, SIZES_ROW_LEN, packed_files,
};
pub use sector::{
    FRAMES_PER_SECOND, HEADER_LEN, LBA_BIAS, MAX_CHUNK_SECTORS, RAW_SECTOR, SECONDS_PER_MINUTE,
    SECTOR, SYNC, USER_DATA, sector_lba,
};
pub use sheet::{
    MAX_SHEET_LEN, Sheet, SheetKind, SheetTrack, TrackMode, find_sheet, parse_sheet, read_sheet,
};
pub(crate) use sheet::{sheet_naming, sheet_of, without_mark};
pub use sheet_text::{ascii_field, lines, strip};

/// What the Dreamcast extractor begins its answer with for a name the tree may not hold.
const UNSAFE_NAME: &str = "Unsafe disc name";

/// What the Dreamcast extractor answers for a data track's file that ends partway through a raw
/// sector, or in which the sheet places the track at or past the file's end.
const TRUNCATED_TRACK: &str = "Truncated raw track or INDEX beyond its end";

/// What it answers for a track's file that ends before the sectors a read takes from it.
const SHORT_READ: &str = "Truncated raw track";

/// What it answers for a raw sector whose header gives another address than its place.
const ADDRESS_DISAGREES: &str = "Raw sector address disagrees with its track";

/// What it answers for two data tracks that hold sectors of one LBA.
const OVERLAPPING_TRACKS: &str = "Overlapping data tracks";

/// What it answers, before the LBA, for an extent that reaches a sector no data track holds.
const OUTSIDE_DATA_TRACKS: &str = "ISO extent points outside data tracks at LBA";

/// What it answers for a boot area that is not Gauntlet Legends' on the Dreamcast.
const NOT_GAUNTLET_LEGENDS: &str = "Expected a Dreamcast Gauntlet Legends GD-ROM";

/// What the reader answers, before the file's name, for a track's file that is not there.
const MISSING_TRACK: &str = "Track file is missing";

/// What it answers, before the file's name, for a track's file that is no regular file, such as
/// a folder.
const NOT_A_FILE: &str = "Track file is not a regular file";

/// What it answers, before the file's name, for an audio track's file that ends partway through a
/// raw sector.
const TRUNCATED_AUDIO: &str = "Truncated audio track";

/// [`BOOT_LEN`] as the size of the extent the boot area is read as.
const BOOT_EXTENT: u32 = BOOT_LEN as u32;

/// [`RAW_SECTOR`] as a length in a track's file, whose places are reckoned in 64 bits.
const RAW_SECTOR_LEN: u64 = RAW_SECTOR as u64;

/// [`SECTOR`] as a length of an extent, whose sectors are reckoned in 64 bits.
const SECTOR_LEN: u64 = SECTOR as u64;

/// How each raw sector a disc's reader reads is checked.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SectorCheck {
    /// By its header, as the Dreamcast extractor checks it: the sync, mode 1, and the address
    /// that places it.
    Header,
    /// By its header and by the error detection code it keeps after its user data, as an
    /// installation's extraction checks it, so that a sector damaged in the dump is refused.
    HeaderAndEdc,
}

/// What a disc is held to as it is read, besides the rules the Dreamcast extractor reads it by.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DiscRules {
    /// How each raw sector read is checked.
    pub sector_check: SectorCheck,
    /// Whether every track the sheet lists must be there, each audio track's file a regular file
    /// of whole raw sectors; otherwise only the data tracks' files are looked at.
    pub every_track: bool,
    /// Whether a data track must start at exactly LBA 45000, where a GD-ROM's high-density area
    /// begins; otherwise a data track need only hold that LBA.
    pub high_density_start: bool,
}

/// The rules the Dreamcast extractor reads a disc by, and nothing more.
pub const SCRIPT_RULES: DiscRules = DiscRules {
    sector_check: SectorCheck::Header,
    every_track: false,
    high_density_start: false,
};

/// The rules an installation's extraction reads a disc by: every sector it reads held to its
/// error detection code, every track's file there, and the high-density area's first data track
/// starting where a pressed disc's does.
pub const INSTALL_RULES: DiscRules = DiscRules {
    sector_check: SectorCheck::HeaderAndEdc,
    every_track: true,
    high_density_start: true,
};

/// A data track as the reader placed it and the Dreamcast extractor's manifest records it: at the
/// LBA its first sector's header gives, whatever the sheet lists it as.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataTrack {
    /// The name of its file beside the sheet, as the sheet spells it.
    pub file: String,
    /// How many sectors of its file come before its first.
    pub index: u64,
    /// The LBA its first sector's header gives, which lies before LBA 0 for the disc's first
    /// addresses.
    pub first_lba: i64,
    /// How many sectors it holds: those of its file from its first to the file's end.
    pub sectors: u64,
}

/// What a disc's sheet and its data tracks' first sectors say of the disc, and its boot area.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscHead {
    /// The sheet the disc was read from.
    pub sheet: Sheet,
    /// Its data tracks, in LBA order.
    pub tracks: Vec<DataTrack>,
    /// Its boot area.
    pub boot: IpBin,
}

/// What the sheet at `sheet` and the disc's data tracks say of the disc, read as the Dreamcast
/// extractor opens a disc and held to `rules`: the sheet, read whole; each data track it lists, in
/// its order, its file opened and the track placed by the address its first sector's header gives
/// and, for a GDI, held to the LBA the GDI gives it; the tracks in LBA order, none holding sectors
/// of another; and the boot area, the [`BOOT_LEN`] bytes from [`BOOT_LBA`], refused unless it is
/// Gauntlet Legends' and then decoded. Under `rules` every audio track's file must be there too,
/// every sector read must keep its error detection code, and a data track must start at
/// [`BOOT_LBA`]; under any rules, one must hold that LBA. Refused by the first rule the disc
/// breaks. The files are only ever read, and are closed before this returns.
pub fn read_head(sheet: &Path, rules: &DiscRules) -> Result<DiscHead, MediaError> {
    head_of(read_sheet(sheet)?, rules)
}

/// What [`read_head`] reads of the disc whose sheet, `sheet`, was read already, so that the disc
/// is read by the very sheet its caller read.
pub(crate) fn head_of(sheet: Sheet, rules: &DiscRules) -> Result<DiscHead, MediaError> {
    let (sheet, tracks, boot) = open_head(sheet, rules)?;
    Ok(DiscHead {
        sheet,
        tracks: tracks.placed,
        boot,
    })
}

/// A file the disc's file system lists: its path in the disc's tree and the extent that holds it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscFile {
    /// Its path, every component of it a name the extracted tree may hold.
    pub path: RelPath,
    /// The LBA of its extent's first sector.
    pub lba: u32,
    /// How many bytes it holds, from the start of its extent's first sector.
    pub size: u32,
}

/// A GD-ROM dump open for reading: its sheet, its data tracks placed by their sectors' headers,
/// each with its file open, its boot area, the rules it is read by, and its ISO 9660 volume's
/// name and files.
pub struct GdRom {
    /// The sheet it was opened from.
    sheet: Sheet,
    /// Its data tracks and their files.
    tracks: Tracks,
    /// Its boot area.
    boot: IpBin,
    /// The rules it is read by.
    rules: DiscRules,
    /// Its volume's name.
    volume: String,
    /// Every file of its volume's tree, in the order the walk met them.
    files: Vec<DiscFile>,
}

impl GdRom {
    /// Opens the disc whose sheet is at `sheet`, its head read as [`read_head`] reads it, then
    /// its ISO 9660 volume as the Dreamcast extractor reads it: the volume descriptor after the
    /// boot area, and the tree of directories from its root, each record's identifier judged as
    /// one name of the extracted tree, its version dropped, before it is joined to its folder's
    /// path, and each record's extent held to the data tracks. The data tracks' files stay open
    /// for the reads that follow. The head and the volume descriptor are reads of fixed, small
    /// sizes, taken whole whatever the job asks; the job's cancel is looked at before each chunk
    /// a directory's read takes, since a disc decides how many directories there are and how long.
    pub fn open(sheet: &Path, rules: &DiscRules, job: &Job<'_>) -> Result<GdRom, MediaError> {
        let (sheet, mut tracks, boot) = open_head(read_sheet(sheet)?, rules)?;
        let volume = iso::read_volume(&mut tracks, job.cancel)?;
        Ok(GdRom {
            sheet,
            tracks,
            boot,
            rules: *rules,
            volume: volume.name,
            files: volume.files,
        })
    }

    /// The rules the disc is read by.
    pub fn rules(&self) -> &DiscRules {
        &self.rules
    }

    /// The disc's data tracks, in LBA order.
    pub fn data_tracks(&self) -> &[DataTrack] {
        &self.tracks.placed
    }

    /// The disc's boot area.
    pub fn boot(&self) -> &IpBin {
        &self.boot
    }

    /// The name of the disc's ISO 9660 volume, as its volume descriptor gives it, without the
    /// white space that pads it.
    pub fn volume(&self) -> &str {
        &self.volume
    }

    /// Every file of the disc's ISO 9660 tree, in the order the walk met them: each directory's
    /// records in the order it keeps them, a folder's files where the folder's record stands.
    /// Folders are not listed; a file's path names them.
    pub fn files(&self) -> &[DiscFile] {
        &self.files
    }

    /// The sheet the disc was opened from.
    pub fn sheet(&self) -> &Sheet {
        &self.sheet
    }

    /// The `size` bytes of the disc's user data from `lba`, read as [`copy`](Self::copy) reads a
    /// file's, whole into memory: for the small extents the reader takes whole, such as its boot
    /// area, a directory or a table of files.
    pub fn read(&mut self, lba: u32, size: u32) -> Result<Vec<u8>, MediaError> {
        self.tracks.read(lba, size, &Cancel::default())
    }

    /// Writes `file`'s bytes to `to` and gives the SHA-256 digest of what it wrote, read as the
    /// Dreamcast extractor reads an extent. Every sector the extent takes must lie in a data
    /// track, one track running on into the next where the two meet, which is checked before a
    /// sector is read. Then it is read in chunks of at most [`MAX_CHUNK_SECTORS`] sectors, none
    /// past its track's end, each sector's header held to the place its track puts it at and,
    /// under the rules of an installation, its error detection code to the bytes it covers; each
    /// sector gives its user data, the last cut to the file's size. `cancel` is looked at before
    /// each chunk, and each chunk is written as one. A write `to` refuses fails the copy as
    /// [`MediaError::Write`], which names the disc file it was copying and keeps what `to`
    /// answered, so that a caller can tell a full disk from a dump it could not read.
    pub fn copy(
        &mut self,
        file: &DiscFile,
        to: &mut dyn Write,
        cancel: &Cancel,
    ) -> Result<Sha256Digest, MediaError> {
        let mut hasher = Sha256::new();
        self.tracks
            .read_chunks(file.lba, file.size, cancel, |chunk| {
                to.write_all(chunk).map_err(|source| MediaError::Write {
                    file: file.path.clone(),
                    source,
                })?;
                hasher.update(chunk);
                Ok(())
            })?;
        Ok(Sha256Digest(hasher.finalize().into()))
    }
}

impl fmt::Debug for GdRom {
    /// The rules the disc is read by, where its data tracks lie, its boot area's fields, its
    /// volume's name and how many files its tree holds: never a path of that tree or of the dump's
    /// files, which say what the player keeps where, nor a byte of either.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        let tracks: Vec<(i64, u64)> = self
            .tracks
            .placed
            .iter()
            .map(|track| (track.first_lba, track.sectors))
            .collect();
        to.debug_struct("GdRom")
            .field("rules", &self.rules)
            .field("data_tracks", &tracks)
            .field("boot", &self.boot)
            .field("volume", &self.volume)
            .field("files", &self.files.len())
            .finish_non_exhaustive()
    }
}

/// A disc's data tracks in LBA order, each with its file open, and how each sector read from them
/// is checked.
struct Tracks {
    /// The tracks, in LBA order.
    placed: Vec<DataTrack>,
    /// Each track's file, in the same order.
    files: Vec<TrackFile>,
    /// How each sector read is checked.
    check: SectorCheck,
}

impl Tracks {
    /// The place among the tracks of the one that holds the sector at `lba`, if one does.
    fn holding(&self, lba: i64) -> Option<usize> {
        self.placed
            .iter()
            .position(|track| (track.first_lba..end_of(track)).contains(&lba))
    }

    /// Refused unless every sector of the extent of `size` bytes from `lba` lies in a data track,
    /// an extent running on from one track into the next where the two meet. An extent of no
    /// bytes takes no sector, wherever it lies.
    fn check_extent(&self, lba: u32, size: u32) -> Result<(), MediaError> {
        let mut at = i64::from(lba);
        let mut left = u64::from(size).div_ceil(SECTOR_LEN);
        while left > 0 {
            let place = self.holding(at).ok_or_else(|| outside(at))?;
            let taken = left.min(room(&self.placed[place], at));
            left -= taken;
            at = at.saturating_add_unsigned(taken);
        }
        Ok(())
    }

    /// The `size` bytes of user data from `lba`, read as [`Tracks::read_chunks`] reads them.
    fn read(&mut self, lba: u32, size: u32, cancel: &Cancel) -> Result<Vec<u8>, MediaError> {
        let mut data = Vec::new();
        self.read_chunks(lba, size, cancel, |chunk| {
            data.extend_from_slice(chunk);
            Ok(())
        })?;
        Ok(data)
    }

    /// Reads the `size` bytes of user data from `lba` as [`GdRom::copy`] does, the extent checked
    /// first, and gives `take` each chunk's, with `cancel` looked at before each chunk.
    fn read_chunks(
        &mut self,
        lba: u32,
        size: u32,
        cancel: &Cancel,
        mut take: impl FnMut(&[u8]) -> Result<(), MediaError>,
    ) -> Result<(), MediaError> {
        self.check_extent(lba, size)?;
        let mut at = i64::from(lba);
        let mut left = u64::from(size);
        let (mut raw, mut data) = (Vec::new(), Vec::new());
        while left > 0 {
            cancel.check()?;
            let place = self.holding(at).ok_or_else(|| outside(at))?;
            let track = &self.placed[place];
            let count = left
                .div_ceil(SECTOR_LEN)
                .min(MAX_CHUNK_SECTORS)
                .min(room(track, at));
            let first = track.index + at.abs_diff(track.first_lba);
            // At most MAX_CHUNK_SECTORS sectors, a little over a MiB.
            raw.resize(count as usize * RAW_SECTOR, 0);
            self.files[place].read_at(first * RAW_SECTOR_LEN, &mut raw)?;
            data.clear();
            let (sectors, _) = raw.as_chunks::<RAW_SECTOR>();
            for (expected, sector) in (at..).zip(sectors) {
                if sector_lba(sector)? != expected {
                    return Err(MediaError::disc(DiscRule::SectorAddress, ADDRESS_DISAGREES));
                }
                if self.check == SectorCheck::HeaderAndEdc {
                    intact(sector, expected)?;
                }
                data.extend_from_slice(&sector[USER_DATA]);
            }
            // The last chunk is cut to what is left of the extent; every other is whole.
            let kept = left.min(data.len() as u64);
            take(&data[..kept as usize])?;
            left -= kept;
            at = at.saturating_add_unsigned(count);
        }
        Ok(())
    }
}

/// A track's file, open for reading, and the path it was opened at, which its failures name.
struct TrackFile {
    /// Where it was opened.
    path: PathBuf,
    /// The file, opened only for reading.
    file: File,
}

impl TrackFile {
    /// Fills `into` with the file's bytes from `offset`: refused as a short read when the file
    /// ends first, as one cut short after its length was read does.
    fn read_at(&mut self, offset: u64, into: &mut [u8]) -> Result<(), MediaError> {
        self.file
            .seek(SeekFrom::Start(offset))
            .map_err(|error| MediaError::io(&self.path, error))?;
        self.file.read_exact(into).map_err(|error| {
            if error.kind() == io::ErrorKind::UnexpectedEof {
                MediaError::disc(DiscRule::ShortRead, SHORT_READ)
            } else {
                MediaError::io(&self.path, error)
            }
        })
    }
}

/// The disc's sheet, `sheet`, its data tracks placed and in LBA order with their files open, and
/// its boot area, as [`read_head`] reads them.
fn open_head(sheet: Sheet, rules: &DiscRules) -> Result<(Sheet, Tracks, IpBin), MediaError> {
    let folder = sheet.path.parent().unwrap_or(Path::new(""));
    let mut placed = Vec::new();
    for track in &sheet.tracks {
        let at = folder.join(&track.file);
        match track.mode {
            TrackMode::Audio if rules.every_track => check_audio(&at, &track.file)?,
            TrackMode::Audio => {}
            TrackMode::Mode1Raw => placed.push(place(track, at, rules)?),
        }
    }
    // In LBA order, the sort keeping the sheet's order among tracks of one LBA.
    placed.sort_by_key(|(track, _)| track.first_lba);
    let overlapping = placed
        .iter()
        .zip(placed.iter().skip(1))
        .any(|((before, _), (after, _))| end_of(before) > after.first_lba);
    if overlapping {
        return Err(MediaError::disc(
            DiscRule::OverlappingTracks,
            OVERLAPPING_TRACKS,
        ));
    }
    let (placed, files) = placed.into_iter().unzip();
    let mut tracks = Tracks {
        placed,
        files,
        check: rules.sector_check,
    };
    let boot_lba = i64::from(BOOT_LBA);
    if rules.high_density_start
        && tracks
            .placed
            .iter()
            .all(|track| track.first_lba != boot_lba)
    {
        let detail = format!(
            "No data track starts at LBA {BOOT_LBA}, where a GD-ROM's high-density area begins"
        );
        return Err(MediaError::disc(DiscRule::HighDensityStart, detail));
    }
    if tracks.holding(boot_lba).is_none() {
        let detail =
            format!("No data track holds LBA {BOOT_LBA}, where a GD-ROM's boot area begins");
        return Err(MediaError::disc(DiscRule::HighDensityStart, detail));
    }
    let boot = tracks.read(BOOT_LBA, BOOT_EXTENT, &Cancel::default())?;
    if !ipbin::names_gauntlet_legends(&boot) {
        return Err(MediaError::disc(
            DiscRule::NotGauntletLegends,
            NOT_GAUNTLET_LEGENDS,
        ));
    }
    let boot = IpBin::decode(boot)?;
    Ok((sheet, tracks, boot))
}

/// The data track `track` the sheet lists, its file at `path`, placed by the address its first
/// sector's header gives, with its file open. The file must hold whole raw sectors and the sector
/// the sheet places the track at; that sector's header gives the track's first LBA, held under
/// `rules` to the error detection code the sector keeps and, for a GDI, to the LBA it gives.
fn place(
    track: &SheetTrack,
    path: PathBuf,
    rules: &DiscRules,
) -> Result<(DataTrack, TrackFile), MediaError> {
    let (file, len) = open_track(&path, &track.file)?;
    let whole = len / RAW_SECTOR_LEN;
    if !len.is_multiple_of(RAW_SECTOR_LEN) || track.index >= whole {
        return Err(MediaError::disc(DiscRule::TruncatedTrack, TRUNCATED_TRACK));
    }
    let mut file = TrackFile { path, file };
    // The header alone, as the Dreamcast extractor reads it, or the whole sector, whose code
    // covers its header and user data both.
    let read = match rules.sector_check {
        SectorCheck::Header => HEADER_LEN,
        SectorCheck::HeaderAndEdc => RAW_SECTOR,
    };
    let mut sector = [0; RAW_SECTOR];
    file.read_at(track.index * RAW_SECTOR_LEN, &mut sector[..read])?;
    let first_lba = sector_lba(&sector)?;
    if rules.sector_check == SectorCheck::HeaderAndEdc {
        intact(&sector, first_lba)?;
    }
    if let Some(lba) = track.lba
        && i64::from(lba) != first_lba
    {
        let detail = format!(
            "GDI track {} starts at LBA {lba}, but its first sector's header gives LBA \
             {first_lba}",
            track.number
        );
        return Err(MediaError::disc(DiscRule::GdiFirstSector, detail));
    }
    let placed = DataTrack {
        file: track.file.clone(),
        index: track.index,
        first_lba,
        sectors: whole - track.index,
    };
    Ok((placed, file))
}

/// The file of a track at `path`, which the sheet names `name`, opened for reading, and its
/// length: refused when it is not there or is no regular file, such as a folder or a FIFO, which
/// is never opened.
fn open_track(path: &Path, name: &str) -> Result<(File, u64), MediaError> {
    open_regular(path)
        .map_err(|error| unopened(path, name, error))?
        .ok_or_else(|| MediaError::disc(DiscRule::MissingTrack, quoted(NOT_A_FILE, name)))
}

/// Refused unless the file of an audio track at `path`, which the sheet names `name`, is a regular
/// file of whole raw sectors.
fn check_audio(path: &Path, name: &str) -> Result<(), MediaError> {
    let metadata = fs::metadata(path).map_err(|error| unopened(path, name, error))?;
    if !metadata.is_file() {
        return Err(MediaError::disc(
            DiscRule::MissingTrack,
            quoted(NOT_A_FILE, name),
        ));
    }
    if !metadata.len().is_multiple_of(RAW_SECTOR_LEN) {
        return Err(MediaError::disc(
            DiscRule::TruncatedTrack,
            quoted(TRUNCATED_AUDIO, name),
        ));
    }
    Ok(())
}

/// What `error`, the system's answer to looking at or opening the file of a track at `path`, which
/// the sheet names `name`, makes of the track: missing when nothing is there, and any other
/// answer a failure to read.
fn unopened(path: &Path, name: &str, error: io::Error) -> MediaError {
    if error.kind() == io::ErrorKind::NotFound {
        MediaError::disc(DiscRule::MissingTrack, quoted(MISSING_TRACK, name))
    } else {
        MediaError::io(path, error)
    }
}

/// Refused unless `sector`, the raw sector at `lba`, keeps the error detection code of the bytes
/// the code covers.
fn intact(sector: &[u8; RAW_SECTOR], lba: i64) -> Result<(), MediaError> {
    let mut kept = [0; size_of::<u32>()];
    kept.copy_from_slice(&sector[EDC_FIELD]);
    if edc(&sector[EDC_SPAN]) == u32::from_le_bytes(kept) {
        return Ok(());
    }
    let detail = format!("Raw sector at LBA {lba} fails its error detection code");
    Err(MediaError::disc(DiscRule::SectorEdc, detail))
}

/// The LBA just past `track`'s last sector.
fn end_of(track: &DataTrack) -> i64 {
    track.first_lba.saturating_add_unsigned(track.sectors)
}

/// How many of `track`'s sectors lie from `lba`, one it holds, to its end.
fn room(track: &DataTrack, lba: i64) -> u64 {
    track.sectors - lba.abs_diff(track.first_lba)
}

/// The refusal of an extent that reaches the sector at `lba`, which no data track holds.
fn outside(lba: i64) -> MediaError {
    let detail = format!("{OUTSIDE_DATA_TRACKS} {lba}");
    MediaError::disc(DiscRule::ExtentOutside, detail)
}

/// A text field of the disc, which `what` names, read as [`ascii_field`] reads one: refused when a
/// byte of it lies outside ASCII.
fn ascii_text(bytes: &[u8], what: &str) -> Result<String, MediaError> {
    ascii_field(bytes).ok_or_else(|| {
        let detail = format!("{what} holds a byte outside ASCII");
        MediaError::disc(DiscRule::TextField, detail)
    })
}

/// The refusal by `rule` of `name`, a name taken from the dump that breaks the rule of names
/// `broken`: the name quoted whole, or past [`MAX_QUOTED_CHARS`](crate::error::MAX_QUOTED_CHARS)
/// characters its length and its start, and then the rule it breaks.
fn unsafe_name(rule: DiscRule, name: &str, broken: NameRule) -> MediaError {
    MediaError::disc(rule, format!("{} ({broken})", quoted(UNSAFE_NAME, name)))
}

/// Whether `text` is a number in ASCII digits: one or more of them, with no sign.
fn is_number(text: &str) -> bool {
    !text.is_empty() && text.bytes().all(|byte| byte.is_ascii_digit())
}

/// The refusal of a number a sheet gives that its reader cannot keep, `what` it is and the most it
/// may be.
fn too_large(what: &str, most: u64) -> MediaError {
    MediaError::disc(
        DiscRule::NumberTooLarge,
        format!("{what} is larger than {most}"),
    )
}

#[cfg(test)]
mod tests {
    use std::fmt;
    use std::fs::{self, File};
    use std::io::{self, Write};
    use std::path::PathBuf;

    use gdl_files::{Cancel, DISC_NAMES, Job, NoProgress, RelPath};
    use gdl_fixtures::{DiscSet, Edc, SparseFile, raw_sector, snapshot};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::{DiscFile, DiscRules, GdRom, INSTALL_RULES, SCRIPT_RULES, TrackFile, read_head};
    use crate::vectors::{dreamcast_golden, dreamcast_vectors, located, manifest_files};
    use crate::vectors::{manifest_restored, players_disc, recorded, restored, text};
    use crate::{AREA, MAX_SIZES_ROM_LEN, MediaError, PackedPlace, RAW_SECTOR, Sha256Digest};
    use crate::{VERSION, packed_files};

    /// How many bytes of user data the format gives a sector. The format's facts are given here
    /// apart from the reader's own constants, so that a reader that looks elsewhere is caught.
    const FORMAT_SECTOR: usize = 2048;

    /// Where the format keeps a raw sector's user data, after its sync and header.
    const FORMAT_USER_DATA_AT: usize = 16;

    /// Where the format keeps a raw sector's error detection code, right after its user data.
    const FORMAT_EDC_AT: usize = 2064;

    /// Where the format keeps the parities that correct a raw sector, after its code.
    const FORMAT_PARITIES_AT: usize = 2068;

    /// The most sectors the Dreamcast extractor reads from a track at once.
    const FORMAT_CHUNK_SECTORS: usize = 512;

    /// How many sectors the boot area takes.
    const FORMAT_BOOT_SECTORS: usize = 16;

    /// What the extractor answers for an extent that reaches a sector no data track holds, before
    /// the sector's LBA.
    const OUTSIDE_WORDS: &str = "ISO extent points outside data tracks at LBA";

    /// What it answers for a data track's file that ends partway through a raw sector, or that
    /// holds no sector where its sheet places the track.
    const TRUNCATED_WORDS: &str = "Truncated raw track or INDEX beyond its end";

    /// What it answers for a raw sector whose header gives another address than its place.
    const DISAGREES_WORDS: &str = "Raw sector address disagrees with its track";

    /// What it answers for bytes that do not begin with the sync and a header of mode 1.
    const NOT_MODE1_WORDS: &str = "Expected a MODE1/2352 sector header";

    /// What the launcher answers for an installation's disc whose data tracks start elsewhere.
    const STARTS_WORDS: &str =
        "No data track starts at LBA 45000, where a GD-ROM's high-density area begins";

    /// What it answers for a disc no data track of which holds the boot area's first sector.
    const HOLDS_WORDS: &str = "No data track holds LBA 45000, where a GD-ROM's boot area begins";

    /// A writer that keeps each write it is given apart, and may ask a job to stop as it takes
    /// its first.
    #[derive(Default)]
    struct Recording<'a> {
        /// Each write, in turn.
        writes: Vec<Vec<u8>>,
        /// The request to stop it makes as it takes a write.
        stop: Option<&'a Cancel>,
    }

    impl Recording<'_> {
        /// How many bytes each write held.
        fn lens(&self) -> Vec<usize> {
            self.writes.iter().map(Vec::len).collect()
        }

        /// Every byte written, in turn.
        fn joined(&self) -> Vec<u8> {
            self.writes.concat()
        }
    }

    impl Write for Recording<'_> {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
            self.writes.push(bytes.to_vec());
            if let Some(cancel) = self.stop {
                cancel.cancel();
            }
            Ok(bytes.len())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    /// A writer whose every write the system refuses, as a full disk's.
    struct Full;

    impl Write for Full {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::ErrorKind::StorageFull.into())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    /// A job nobody follows, asked to stop only through `cancel`.
    fn quiet(cancel: &Cancel) -> Job<'_> {
        Job {
            cancel,
            progress: &NoProgress,
        }
    }

    /// `set` written into a folder of its own, and its sheet's path.
    fn written(set: &DiscSet) -> (TempDir, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let sheet = set.write(dir.path()).unwrap();
        (dir, sheet)
    }

    /// `set` written into a folder of its own, a folder standing where its file `name` would,
    /// and its sheet's path.
    fn written_with_folder(set: &DiscSet, name: &str) -> (TempDir, PathBuf) {
        let mut without = set.clone();
        without.files.retain(|(held, _)| held != name);
        let (dir, sheet) = written(&without);
        fs::create_dir(dir.path().join(name)).unwrap();
        (dir, sheet)
    }

    /// The disc `set` holds, opened by `rules`, and the folder it was written into, which a caller
    /// keeps until the disc is closed.
    fn opened(set: &DiscSet, rules: &DiscRules) -> (TempDir, Result<GdRom, MediaError>) {
        let (dir, sheet) = written(set);
        let disc = GdRom::open(&sheet, rules, &quiet(&Cancel::default()));
        (dir, disc)
    }

    /// Whether the disc `set` holds opens by `rules`.
    fn opens(set: &DiscSet, rules: &DiscRules) -> bool {
        let (_dir, disc) = opened(set, rules);
        disc.is_ok()
    }

    /// What a copy of `file` from the disc `set` holds, opened by `rules`, writes.
    fn copied(set: &DiscSet, rules: &DiscRules, file: &DiscFile) -> Vec<u8> {
        let (_dir, disc) = opened(set, rules);
        let mut written = Vec::new();
        disc.unwrap()
            .copy(file, &mut written, &Cancel::default())
            .unwrap();
        written
    }

    /// The rule a reader refused by, one of the disc's format, and the words it said.
    fn refusal<T: fmt::Debug>(outcome: Result<T, MediaError>) -> (&'static str, String) {
        let error = outcome.unwrap_err();
        assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
        (error.rule(), error.to_string())
    }

    /// The refusal of the disc `set` holds, opened by `rules`.
    fn refused(set: &DiscSet, rules: &DiscRules) -> (&'static str, String) {
        refusal(opened(set, rules).1)
    }

    /// The refusal by `rule` with `words`.
    fn said(rule: &'static str, words: &str) -> (&'static str, String) {
        (rule, words.to_owned())
    }

    /// The place in a track's file of the byte `at` bytes into the raw sector `sector` sectors
    /// into the file.
    fn place_of(sector: usize, at: usize) -> u64 {
        (sector * RAW_SECTOR + at) as u64
    }

    /// `set` with the byte of its file `name` at `place` changed in its lowest bit.
    fn flipped(mut set: DiscSet, name: &str, place: u64) -> DiscSet {
        let file = set.file_mut(name);
        let mut bytes = file.contents();
        bytes[place as usize] ^= 1;
        *file = SparseFile::holding(bytes);
        set
    }

    /// `set` with its file `name` starting `sectors` raw sectors later, those before cut away.
    fn without_first(mut set: DiscSet, name: &str, sectors: usize) -> DiscSet {
        let file = set.file_mut(name);
        let bytes = file.contents();
        *file = SparseFile::holding(bytes[sectors * RAW_SECTOR..].to_vec());
        set
    }

    /// `set` with its sheet's text `from` written as `to`.
    fn resheeted(mut set: DiscSet, from: &str, to: &str) -> DiscSet {
        let name = set.sheet.clone();
        let sheet = set.file_mut(&name);
        let text = String::from_utf8(sheet.contents()).unwrap();
        assert!(text.contains(from), "{text:?}");
        *sheet = SparseFile::holding(text.replace(from, to).into_bytes());
        set
    }

    /// The SHA-256 digest the shared vectors give the IP.BIN of the tree the set `set` names is
    /// extracted to.
    fn ip_bin_digest(set: &Value) -> &str {
        let rows = set["tree"].as_array().unwrap();
        let row = rows.iter().find(|row| row[0] == "IP.BIN").unwrap();
        assert_eq!(row[1], 32_768);
        text(&row[2])
    }

    /// The file `disc`'s root lists by exactly the name `name`.
    fn at_root(disc: &GdRom, name: &str) -> DiscFile {
        let mut files = disc.files().iter();
        files
            .find(|file| file.path.as_str() == name)
            .cloned()
            .unwrap_or_else(|| panic!("the disc's root holds no {name}"))
    }

    /// The disc file `path` the root lists, `size` bytes at `lba`.
    fn disc_file(path: &str, lba: u32, size: u32) -> DiscFile {
        DiscFile {
            path: RelPath::parse(path, &DISC_NAMES).unwrap(),
            lba,
            size,
        }
    }

    /// The refusal of an extent that reaches the sector at `lba`.
    fn outside(lba: i64) -> (&'static str, String) {
        ("extentOutside", format!("{OUTSIDE_WORDS} {lba}"))
    }

    #[test]
    fn the_small_set_places_its_tracks_from_its_sheet() {
        let vectors = dreamcast_vectors();
        let small = &vectors["sets"]["small"];
        let golden = dreamcast_golden(&small["goldens"]["disc-manifest.json"]);
        let (dir, sheet) = written(&DiscSet::small(Edc::Zero));
        let before = snapshot(dir.path()).unwrap();

        let mut disc = GdRom::open(&sheet, &SCRIPT_RULES, &quiet(&Cancel::default())).unwrap();
        // Each data track lies at the LBA its first sector's header gives, as the extractor's
        // manifest records it: the second's file keeps three seconds of pregap before that
        // sector, so it starts 225 sectors in, at 45100.
        assert_eq!(recorded(disc.data_tracks()), golden["data_tracks"]);
        let second = &disc.data_tracks()[1];
        assert_eq!((second.index, second.first_lba), (225, 45_100));
        // The root lists the disc's one file at 45100, in that track, where the extractor's
        // manifest records it, and what lies there is read from that track by its LBA.
        assert_eq!(located(disc.files()), manifest_files(&golden));
        assert_eq!(disc.files()[0].lba, 45_100);
        assert_eq!(disc.read(45_100, 5).unwrap(), b"hello");
        // The boot area is the IP.BIN the extractor writes, and its fields and the volume's name
        // what it records.
        let boot = disc.boot();
        assert_eq!(Sha256Digest::of(boot.bytes()).hex(), ip_bin_digest(small));
        assert_eq!(boot.product(), text(&golden["product"]));
        assert_eq!(boot.title(), text(&golden["title"]));
        assert_eq!(boot.executable(), text(&golden["executable"]));
        assert_eq!(disc.volume(), text(&golden["volume"]));
        assert_eq!(disc.sheet().path, sheet);
        assert_eq!(*disc.rules(), SCRIPT_RULES);

        // The head alone reads the same, without the disc kept open.
        let head = read_head(&sheet, &SCRIPT_RULES).unwrap();
        assert_eq!(head.tracks, disc.data_tracks());
        assert_eq!(head.boot, *disc.boot());
        assert_eq!(head.sheet, *disc.sheet());
        // The dump is only ever read.
        drop(disc);
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[test]
    fn the_full_sets_place_their_tracks_where_the_goldens_record_them() {
        let vectors = dreamcast_vectors();
        let full = &vectors["sets"]["full"];
        let golden = dreamcast_golden(&full["goldens"]["disc-manifest.json"]);
        // The GDI set's data tracks, each from its file's first sector, in the manifest's form.
        let gdi: Vec<Value> = vectors["sets"]["fullGdi"]["dataTracks"]
            .as_array()
            .unwrap()
            .iter()
            .map(|row| json!({"file": row[0], "index": row[1], "first_lba": row[2], "sectors": row[3]}))
            .collect();
        let cue = golden["data_tracks"].clone();
        let gdi = Value::Array(gdi);
        // Under the extractor's rules, and under an installation's, with every sector's code and
        // every audio track's file.
        let sets = [
            (DiscSet::full(Edc::Zero), SCRIPT_RULES, &cue),
            (
                DiscSet::full(Edc::Computed).with_audio_files(),
                INSTALL_RULES,
                &cue,
            ),
            (DiscSet::full_gdi(Edc::Zero), SCRIPT_RULES, &gdi),
            (
                DiscSet::full_gdi(Edc::Computed).with_audio_files(),
                INSTALL_RULES,
                &gdi,
            ),
        ];
        for (set, rules, tracks) in sets {
            let (_dir, disc) = opened(&set, &rules);
            let disc = disc.unwrap();
            assert_eq!(recorded(disc.data_tracks()), *tracks, "{}", set.sheet);
            assert_eq!(*disc.rules(), rules);
            let boot = disc.boot();
            assert_eq!(Sha256Digest::of(boot.bytes()).hex(), ip_bin_digest(full));
            let fields = (boot.product(), boot.title(), boot.executable());
            let recorded_fields = (
                text(&golden["product"]),
                text(&golden["title"]),
                text(&golden["executable"]),
            );
            assert_eq!(fields, recorded_fields, "{}", set.sheet);
        }
    }

    #[test]
    fn the_full_sets_files_read_as_the_extractor_read_them() {
        let vectors = dreamcast_vectors();
        let golden = dreamcast_golden(&vectors["sets"]["full"]["goldens"]["disc-manifest.json"]);
        // Each file the extractor's manifest lists, where it lists it: one over two sectors, one
        // in a sector of its own, one of no bytes at an LBA no track holds, and SIZES.ROM, each
        // with the digest of what it read.
        let digests: Vec<&str> = golden["files"]
            .as_array()
            .unwrap()
            .iter()
            .map(|row| text(&row["sha256"]))
            .collect();
        assert_eq!(digests.len(), 4);
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
            let (_dir, disc) = opened(&set, &rules);
            let mut disc = disc.unwrap();
            // The walk lists them where the manifest does, and each reads to its digest.
            assert_eq!(
                located(disc.files()),
                manifest_files(&golden),
                "{}",
                set.sheet
            );
            let files = disc.files().to_vec();
            for (file, digest) in files.iter().zip(&digests) {
                let mut copied = Vec::new();
                let given = disc.copy(file, &mut copied, &Cancel::default()).unwrap();
                let path = file.path.as_str();
                assert_eq!(copied.len(), file.size as usize, "{} {path}", set.sheet);
                assert_eq!(given.hex(), *digest, "{} {path}", set.sheet);
                assert_eq!(Sha256Digest::of(&copied), given);
                assert_eq!(disc.read(file.lba, file.size).unwrap(), copied, "{path}");
            }
        }
    }

    #[test]
    fn the_full_sets_table_lists_the_native_files_the_extractor_restored() {
        let vectors = dreamcast_vectors();
        let goldens = &vectors["sets"]["full"]["goldens"];
        let golden = dreamcast_golden(&goldens["native-manifest.json"]);
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
            let (_dir, disc) = opened(&set, &rules);
            let mut disc = disc.unwrap();
            // SIZES.ROM and DISK.ROM lie at the root by those names. The table read from the disc
            // is the one the extractor read, by its digest, and lists the files the extractor
            // restored from DISK.ROM where it restored them, and the one outside it as it did.
            let (sizes, disk) = (at_root(&disc, "SIZES.ROM"), at_root(&disc, "DISK.ROM"));
            let table = disc.read(sizes.lba, sizes.size).unwrap();
            let digest = Sha256Digest::of(&table).hex();
            assert_eq!(digest, text(&golden["sizes_sha256"]), "{}", set.sheet);
            let files = packed_files(&table, u64::from(disk.size)).unwrap();
            assert_eq!(
                restored(&files),
                manifest_restored(&golden),
                "{}",
                set.sheet
            );
        }
    }

    #[test]
    fn a_discs_debug_form_names_none_of_its_paths() {
        let set = DiscSet::full(Edc::Zero);
        let (dir, sheet) = written(&set);
        let disc = GdRom::open(&sheet, &SCRIPT_RULES, &quiet(&Cancel::default())).unwrap();
        let shown = format!("{disc:?}");
        // What the disc is read by, where its data tracks lie, its boot area's fields, its
        // volume's name and how many files its tree holds.
        let tracks: Vec<(i64, u64)> = disc
            .data_tracks()
            .iter()
            .map(|track| (track.first_lba, track.sectors))
            .collect();
        let told = [
            format!("rules: {SCRIPT_RULES:?}"),
            format!("data_tracks: {tracks:?}"),
            format!("boot: {:?}", disc.boot()),
            format!("volume: {:?}", disc.volume()),
            format!("files: {}", disc.files().len()),
        ];
        for part in told {
            assert!(shown.contains(&part), "{part} in {shown}");
        }
        // Never a name or a path of the disc's tree, of the dump's files or of where they lie.
        assert_eq!(disc.files().len(), 4);
        let names = disc
            .files()
            .iter()
            .flat_map(|file| file.path.as_str().split('/'))
            .chain(set.files.iter().map(|(name, _)| name.as_str()));
        for name in names {
            assert!(!shown.contains(name), "{name} in {shown}");
        }
        let folder = dir.path().to_string_lossy().into_owned();
        assert!(!shown.contains(&folder), "{shown}");
    }

    #[test]
    fn an_extent_in_the_audio_gap_is_refused() {
        // The extractor's test: the small set's file listed at 45050, where its audio track
        // lies, refuses the disc as its root is walked, under any rules.
        let gap = DiscSet::small_with(b"TEST.WAD;1", 45_050, 0, Edc::Zero);
        assert_eq!(refused(&gap, &SCRIPT_RULES), outside(45_050));
        let guarded = DiscSet::small_with(b"TEST.WAD;1", 45_050, 0, Edc::Computed);
        assert_eq!(refused(&guarded, &INSTALL_RULES), outside(45_050));
        // A folder listed there is refused the same way, before its directory is read.
        let folder = DiscSet::small_with(b"AUDIO", 45_050, 2, Edc::Zero);
        assert_eq!(refused(&folder, &SCRIPT_RULES), outside(45_050));

        // The small set's first data track holds 45000 to 45020, its second 45100 alone.
        let (_dir, disc) = opened(&DiscSet::small(Edc::Zero), &SCRIPT_RULES);
        let mut disc = disc.unwrap();
        // Where the sheet's audio track would lie, between the two, and before the first and
        // past the last.
        for lba in [45_050, 45_021, 45_099, 44_999, 45_101, 999_999, u32::MAX] {
            assert_eq!(refusal(disc.read(lba, 5)), outside(i64::from(lba)), "{lba}");
        }
        // An extent that starts in a track and runs past its end is refused at its first sector
        // that no track holds, and read when it ends at the track's last.
        assert_eq!(refusal(disc.read(45_020, 2049)), outside(45_021));
        assert_eq!(disc.read(45_020, 2048).unwrap().len(), FORMAT_SECTOR);
        // The whole extent is judged before a sector of it is read or a byte written, so the
        // largest extent the format gives costs no more.
        let mut copied = Vec::new();
        let long = disc_file("LONG.BIN", 45_000, 22 * 2048);
        assert_eq!(
            refusal(disc.copy(&long, &mut copied, &Cancel::default())),
            outside(45_021)
        );
        assert!(copied.is_empty());
        assert_eq!(refusal(disc.read(45_000, u32::MAX)), outside(45_021));
        // An extent of no bytes takes no sector, wherever it lies.
        for lba in [45_050, 0, u32::MAX] {
            assert!(disc.read(lba, 0).unwrap().is_empty(), "{lba}");
        }
    }

    #[test]
    fn an_extent_runs_on_from_one_track_into_the_next() {
        // The GDI set with its first data track kept as two files that meet at 45010.
        let full = DiscSet::full_gdi(Edc::Zero);
        let first = full.file("track03.bin").contents();
        let (head, rest) = first.split_at(10 * RAW_SECTOR);
        let gdi = "3\r\n1 45000 4 2352 head.bin 0\r\n2 45010 4 2352 rest.bin 0\r\n\
                   3 45100 4 2352 track05.bin 0\r\n";
        let split = DiscSet {
            files: vec![
                ("head.bin".to_owned(), SparseFile::holding(head.to_vec())),
                ("rest.bin".to_owned(), SparseFile::holding(rest.to_vec())),
                ("track05.bin".to_owned(), full.file("track05.bin").clone()),
                ("split.gdi".to_owned(), SparseFile::holding(gdi.into())),
            ],
            sheet: "split.gdi".to_owned(),
        };
        let (_whole_dir, one) = opened(&full, &SCRIPT_RULES);
        let mut one = one.unwrap();
        let (_split_dir, two) = opened(&split, &SCRIPT_RULES);
        let mut two = two.unwrap();
        assert_eq!(
            recorded(two.data_tracks()),
            json!([
                {"file": "head.bin", "index": 0, "first_lba": 45000, "sectors": 10},
                {"file": "rest.bin", "index": 0, "first_lba": 45010, "sectors": 12},
                {"file": "track05.bin", "index": 0, "first_lba": 45100, "sectors": 4}
            ])
        );
        // The boot area, read across the two as the disc opens, and the whole first area are what
        // the one track gives.
        assert_eq!(two.boot(), one.boot());
        let area = 22 * 2048;
        assert_eq!(
            two.read(45_000, area).unwrap(),
            one.read(45_000, area).unwrap()
        );
        // A chunk ends at its track's end: a copy across the two takes a chunk from each.
        let boot = disc_file(
            "IP.BIN",
            45_000,
            (FORMAT_BOOT_SECTORS * FORMAT_SECTOR) as u32,
        );
        let mut writer = Recording::default();
        let digest = two.copy(&boot, &mut writer, &Cancel::default()).unwrap();
        assert_eq!(writer.lens(), [10 * FORMAT_SECTOR, 6 * FORMAT_SECTOR]);
        assert!(writer.joined() == one.boot().bytes());
        assert_eq!(digest, Sha256Digest::of(one.boot().bytes()));
        // Within one track the same extent takes one.
        let mut writer = Recording::default();
        one.copy(&boot, &mut writer, &Cancel::default()).unwrap();
        assert_eq!(writer.lens(), [16 * FORMAT_SECTOR]);
    }

    #[test]
    fn tracks_are_placed_in_lba_order_and_never_overlap() {
        let full = DiscSet::full_gdi(Edc::Zero);
        // Listed from the last to the first, the tracks are still placed by their LBAs.
        let backwards = resheeted(
            full.clone(),
            "1 45000 4 2352 track03.bin 0\r\n2 45050 0 2352 track04.raw 0\r\n\
             3 45100 4 2352 track05.bin 0",
            "1 45100 4 2352 track05.bin 0\r\n2 45050 0 2352 track04.raw 0\r\n\
             3 45000 4 2352 track03.bin 0",
        );
        let (_dir, disc) = opened(&backwards, &SCRIPT_RULES);
        let files: Vec<&str> = disc
            .as_ref()
            .unwrap()
            .data_tracks()
            .iter()
            .map(|track| track.file.as_str())
            .collect();
        assert_eq!(files, ["track03.bin", "track05.bin"]);

        let overlapping = said("overlappingTracks", "Overlapping data tracks");
        // One file listed for two tracks: the two hold the same sectors.
        let twice = resheeted(full.clone(), "track05.bin", "track03.bin");
        let twice = resheeted(twice, "3 45100", "3 45000");
        assert_eq!(refused(&twice, &SCRIPT_RULES), overlapping);
        // A track whose file runs a sector into the next track's.
        let mut both = full.clone();
        let first = both.file("track03.bin").contents();
        let next: Vec<u8> = (45_022..45_101)
            .flat_map(|lba| raw_sector(lba, &[], Edc::Zero))
            .collect();
        *both.file_mut("track03.bin") = SparseFile::holding([first, next].concat());
        assert_eq!(refused(&both, &SCRIPT_RULES), overlapping);
        // Running up to the next track's first sector, it meets it, and the two hold no sector
        // of one LBA.
        let mut meeting = full;
        let first = meeting.file("track03.bin").contents();
        let next: Vec<u8> = (45_022..45_100)
            .flat_map(|lba| raw_sector(lba, &[], Edc::Zero))
            .collect();
        *meeting.file_mut("track03.bin") = SparseFile::holding([first, next].concat());
        let (_dir, disc) = opened(&meeting, &SCRIPT_RULES);
        let disc = disc.unwrap();
        assert_eq!(disc.data_tracks()[0].sectors, 100);
    }

    #[test]
    fn a_truncated_track_is_refused() {
        let truncated = said("truncatedTrack", TRUNCATED_WORDS);
        for (edc, rules) in [(Edc::Zero, SCRIPT_RULES), (Edc::Computed, INSTALL_RULES)] {
            // The second track starts at its file's last sector, which is read.
            let set = DiscSet::small(edc);
            assert!(opens(&set, &rules), "{rules:?}");
            // The extractor's test: a byte past the second track's last whole sector.
            let mut longer = set.clone();
            longer.file_mut("track5.bin").len += 1;
            assert_eq!(refused(&longer, &rules), truncated, "{rules:?}");
            // A byte short of the first track's last.
            let mut shorter = set.clone();
            shorter.file_mut("track3.bin").len -= 1;
            assert_eq!(refused(&shorter, &rules), truncated, "{rules:?}");
            // A file of no sectors, and one of the pregap's alone, where the sheet places the
            // track 225 sectors in.
            for len in [0, 225 * RAW_SECTOR as u64] {
                let mut cut = set.clone();
                cut.file_mut("track5.bin").len = len;
                assert_eq!(refused(&cut, &rules), truncated, "{len} {rules:?}");
            }
            // The sheet places the track a sector past its file's last.
            let past = resheeted(set.clone(), "INDEX 01 00:03:00", "INDEX 01 00:03:01");
            assert_eq!(refused(&past, &rules), truncated, "{rules:?}");
        }
    }

    #[test]
    fn a_file_that_ends_before_a_read_is_refused_as_a_short_read() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("track.bin");
        fs::write(&path, [7; 100]).unwrap();
        let mut track = TrackFile {
            path: path.clone(),
            file: File::open(&path).unwrap(),
        };
        let mut into = [0; 60];
        track.read_at(40, &mut into).unwrap();
        assert_eq!(into, [7; 60]);
        // Its bytes end before the read does, as a file cut short after its length was read.
        for (offset, len) in [(41, 60), (0, 101), (100, 1), (1_000, 1)] {
            let mut into = vec![0; len];
            let given = refusal(track.read_at(offset, &mut into));
            assert_eq!(
                given,
                said("shortRead", "Truncated raw track"),
                "{offset} {len}"
            );
        }
        // A read the system refuses is a failure to read the file, which names it: a file
        // opened only to be written is never read.
        let mut unread = TrackFile {
            path: path.clone(),
            file: File::options().write(true).open(&path).unwrap(),
        };
        let error = unread.read_at(0, &mut [0; 1]).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path: at, .. } if *at == path),
            "{error:?}"
        );
        assert_eq!(error.rule(), "io");
    }

    #[test]
    fn a_sector_with_another_address_is_refused() {
        let disagrees = said("sectorAddress", DISAGREES_WORDS);
        // The extractor's test: the frame of sector 45010, in the boot area, set to 11, which
        // gives the address of 45011.
        let set = DiscSet::small(Edc::Zero).patched("track3.bin", place_of(10, 14), &[0x11]);
        assert_eq!(refused(&set, &SCRIPT_RULES), disagrees);
        let (_dir, sheet) = written(&set);
        assert_eq!(refusal(read_head(&sheet, &SCRIPT_RULES)), disagrees);

        // The sectors the opening reads after the boot area are judged as they are read: the
        // volume descriptor's, at 45016, and the root directory's, at 45020, their frames set to
        // 17 and 21. The head alone reads neither.
        for (sector, frame) in [(16, 0x17), (20, 0x21)] {
            let set =
                DiscSet::small(Edc::Zero).patched("track3.bin", place_of(sector, 14), &[frame]);
            assert_eq!(refused(&set, &SCRIPT_RULES), disagrees, "{sector}");
            let (_dir, sheet) = written(&set);
            assert!(read_head(&sheet, &SCRIPT_RULES).is_ok(), "{sector}");
        }
        // Past those a sector is judged only when it is read: one between the volume descriptor
        // and the root directory, at 45018, whose frame is set to 19.
        let set = DiscSet::small(Edc::Zero).patched("track3.bin", place_of(18, 14), &[0x19]);
        let (_dir, disc) = opened(&set, &SCRIPT_RULES);
        let mut disc = disc.unwrap();
        assert_eq!(refusal(disc.read(45_018, 1)), disagrees);
        assert_eq!(refusal(disc.read(45_017, 2049)), disagrees);
        assert_eq!(disc.read(45_017, 2048).unwrap().len(), FORMAT_SECTOR);

        // A sector that begins with no mode 1 header is refused for that, as the extractor refuses
        // it, before its address is compared: in the boot area, and the first of a track, which
        // places the track.
        let not_mode1 = said("sectorHeader", NOT_MODE1_WORDS);
        let cases = [
            ("track3.bin", place_of(3, 1)),
            ("track5.bin", place_of(225, 15)),
        ];
        for (name, place) in cases {
            let set = DiscSet::small(Edc::Zero).patched(name, place, &[0x02]);
            assert_eq!(refused(&set, &SCRIPT_RULES), not_mode1, "{name} {place}");
        }
    }

    #[test]
    fn install_rules_check_every_sectors_edc() {
        let set = DiscSet::full(Edc::Computed).with_audio_files();
        let stream = disc_file("AUDIO/X.STR", 45_102, 3_000);
        // Every sector of the set keeps its code: it opens under both rules, and its stream is
        // read alike.
        let kept = copied(&set, &SCRIPT_RULES, &stream);
        assert_eq!(kept.len(), 3_000);
        assert!(copied(&set, &INSTALL_RULES, &stream) == kept);

        // A bit of the stream's first sector's user data changed, at 45102: the extractor's rules
        // read the sector as it now is, and an installation's refuse it by its code once it is
        // read, before a byte of the chunk is written.
        let edc = |lba: i64| {
            let words = format!("Raw sector at LBA {lba} fails its error detection code");
            ("sectorEdc", words)
        };
        let user_data = place_of(225 + 2, FORMAT_USER_DATA_AT + 100);
        let damaged = flipped(set.clone(), "track5.bin", user_data);
        let read = copied(&damaged, &SCRIPT_RULES, &stream);
        assert_eq!(read[100], kept[100] ^ 1);
        assert!(read[..100] == kept[..100] && read[101..] == kept[101..]);
        let (_dir, disc) = opened(&damaged, &INSTALL_RULES);
        let mut disc = disc.unwrap();
        let mut written = Vec::new();
        let given = refusal(disc.copy(&stream, &mut written, &Cancel::default()));
        assert_eq!(given, edc(45_102));
        assert!(written.is_empty());
        assert_eq!(refusal(disc.read(45_102, 1)), edc(45_102));

        // The code's own bytes changed refuse the sector the same way. What follows them, the
        // parities that correct a sector, are no part of what the code covers.
        let code = flipped(set.clone(), "track5.bin", place_of(227, FORMAT_EDC_AT + 3));
        let (_dir, disc) = opened(&code, &INSTALL_RULES);
        assert_eq!(refusal(disc.unwrap().read(45_102, 1)), edc(45_102));
        let parity = flipped(
            set.clone(),
            "track5.bin",
            place_of(227, FORMAT_PARITIES_AT + 5),
        );
        assert!(copied(&parity, &INSTALL_RULES, &stream) == kept);

        // The sectors the disc reads as it opens are held to their codes as it opens: the boot
        // area's, each data track's first, which places it, the volume descriptor's, and the
        // directories', the root's and AUDIO's. The last three are changed where nothing reads
        // them, so that only their codes tell.
        let cases = [
            ("track3.bin", place_of(5, FORMAT_USER_DATA_AT + 7), 45_005),
            ("track5.bin", place_of(225, FORMAT_USER_DATA_AT), 45_100),
            (
                "track3.bin",
                place_of(16, FORMAT_USER_DATA_AT + 1_000),
                45_016,
            ),
            (
                "track3.bin",
                place_of(20, FORMAT_USER_DATA_AT + 2_000),
                45_020,
            ),
            (
                "track3.bin",
                place_of(21, FORMAT_USER_DATA_AT + 2_000),
                45_021,
            ),
        ];
        for (name, place, lba) in cases {
            let damaged = flipped(set.clone(), name, place);
            assert!(opens(&damaged, &SCRIPT_RULES), "{name}");
            assert_eq!(refused(&damaged, &INSTALL_RULES), edc(lba), "{name}");
        }
        // A set built as the extractor's tests build theirs keeps no codes at all: the first
        // sector read, the first data track's, is refused.
        let unguarded = DiscSet::full(Edc::Zero).with_audio_files();
        assert!(opens(&unguarded, &SCRIPT_RULES));
        assert_eq!(refused(&unguarded, &INSTALL_RULES), edc(45_000));
    }

    #[test]
    fn install_rules_need_every_track_file() {
        // The full set leaves out its audio track's file, which the extractor's rules never look
        // for and an installation's do.
        let set = DiscSet::full(Edc::Computed);
        let missing = |name: &str| ("missingTrack", format!("Track file is missing: {name:?}"));
        let not_a_file = |name: &str| {
            (
                "missingTrack",
                format!("Track file is not a regular file: {name:?}"),
            )
        };
        assert!(opens(&set, &SCRIPT_RULES));
        assert_eq!(refused(&set, &INSTALL_RULES), missing("track4.bin"));
        let with_audio = set.clone().with_audio_files();
        assert!(opens(&with_audio, &INSTALL_RULES));
        // An audio track's file of no sectors is whole; one that ends partway through a sector is
        // not.
        for (len, whole_sectors) in [(0, true), (2 * RAW_SECTOR as u64, true), (2353, false)] {
            let mut sized = with_audio.clone();
            sized.file_mut("track4.bin").len = len;
            assert!(opens(&sized, &SCRIPT_RULES), "{len}");
            if whole_sectors {
                assert!(opens(&sized, &INSTALL_RULES), "{len}");
            } else {
                let truncated = said("truncatedTrack", "Truncated audio track: \"track4.bin\"");
                assert_eq!(refused(&sized, &INSTALL_RULES), truncated, "{len}");
            }
        }
        // A folder where the audio track's file should be is no file, on either system.
        let (_dir, sheet) = written_with_folder(&with_audio, "track4.bin");
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        assert!(GdRom::open(&sheet, &SCRIPT_RULES, &job).is_ok());
        let given = refusal(GdRom::open(&sheet, &INSTALL_RULES, &job));
        assert_eq!(given, not_a_file("track4.bin"));

        // A data track's file is looked for under every rules: missing, or a folder.
        let mut without = with_audio.clone();
        without.files.retain(|(name, _)| name != "track5.bin");
        let (_dir, foldered) = written_with_folder(&with_audio, "track5.bin");
        for rules in [SCRIPT_RULES, INSTALL_RULES] {
            assert_eq!(refused(&without, &rules), missing("track5.bin"));
            let given = refusal(GdRom::open(&foldered, &rules, &job));
            assert_eq!(given, not_a_file("track5.bin"));
        }

        // The GDI set leaves out its audio track's file too.
        let gdi = DiscSet::full_gdi(Edc::Computed);
        assert!(opens(&gdi, &SCRIPT_RULES));
        assert_eq!(refused(&gdi, &INSTALL_RULES), missing("track04.raw"));
        assert!(opens(&gdi.with_audio_files(), &INSTALL_RULES));
    }

    #[cfg(unix)]
    #[test]
    fn a_fifo_named_as_a_track_is_refused_without_waiting_for_a_writer() {
        use crate::dump_file::fifo;

        let not_a_file = |name: &str| {
            let words = format!("Track file is not a regular file: {name:?}");
            ("missingTrack", words)
        };
        // A FIFO where a data track's file should be, under either rules, which a plain open would
        // wait on until a writer came, and where an audio track's should be, which an
        // installation's rules look at without opening it.
        let set = DiscSet::full(Edc::Computed).with_audio_files();
        let cases = [
            ("track5.bin", SCRIPT_RULES),
            ("track5.bin", INSTALL_RULES),
            ("track4.bin", INSTALL_RULES),
        ];
        for (name, rules) in cases {
            let mut without = set.clone();
            without.files.retain(|(held, _)| held != name);
            let (dir, sheet) = written(&without);
            let fifo_path = dir.path().join(name);
            fifo::make(&fifo_path);
            let opened = fifo::without_waiting(&fifo_path, move || {
                let cancel = Cancel::default();
                GdRom::open(&sheet, &rules, &quiet(&cancel)).map(drop)
            });
            assert_eq!(refusal(opened), not_a_file(name), "{name} {rules:?}");
        }
    }

    #[test]
    fn a_gdi_whose_first_header_disagrees_is_refused() {
        let first_sector = |number: u32, lba: u32, header: u32| {
            let words = format!(
                "GDI track {number} starts at LBA {lba}, but its first sector's header gives LBA \
                 {header}"
            );
            ("gdiFirstSector", words)
        };
        // The GDI gives another LBA than a data track's first sector does.
        let cases = [
            ("3 45100 4", "3 45101 4", first_sector(3, 45_101, 45_100)),
            ("1 45000 4", "1 44999 4", first_sector(1, 44_999, 45_000)),
        ];
        for (from, to, expected) in cases {
            let changed = resheeted(DiscSet::full_gdi(Edc::Zero), from, to);
            assert_eq!(refused(&changed, &SCRIPT_RULES), expected);
            let guarded = DiscSet::full_gdi(Edc::Computed).with_audio_files();
            let changed = resheeted(guarded, from, to);
            assert_eq!(refused(&changed, &INSTALL_RULES), expected);
        }
        // Or a track's file starts at another sector than the GDI says.
        let later = without_first(DiscSet::full_gdi(Edc::Zero), "track05.bin", 1);
        assert_eq!(
            refused(&later, &SCRIPT_RULES),
            first_sector(3, 45_100, 45_101)
        );
        // An audio track's file holds no header to agree with, and is never read.
        let audio = resheeted(DiscSet::full_gdi(Edc::Zero), "2 45050 0", "2 99999 0");
        assert!(opens(&audio, &SCRIPT_RULES));
        // A cue sheet gives no LBA: its tracks are placed by their headers alone, as the small
        // set's second is, 225 sectors into its file.
        let (_dir, disc) = opened(&DiscSet::small(Edc::Zero), &SCRIPT_RULES);
        assert_eq!(disc.unwrap().data_tracks()[1].first_lba, 45_100);
    }

    #[test]
    fn the_high_density_area_starts_at_45000() {
        let set = DiscSet::full(Edc::Computed).with_audio_files();
        let starts = said("highDensityStart", STARTS_WORDS);
        // Ten sectors before the boot area at the start of the first data track's file, where the
        // sheet places the track: it starts at 44990 and holds the boot area ten sectors in.
        let mut early = set.clone();
        let track = early.file_mut("track3.bin");
        let before: Vec<u8> = (44_990..45_000)
            .flat_map(|lba| raw_sector(lba, &[], Edc::Computed))
            .collect();
        *track = SparseFile::holding([before, track.contents()].concat());
        let (_dir, disc) = opened(&early, &SCRIPT_RULES);
        let disc = disc.unwrap();
        let first = &disc.data_tracks()[0];
        assert_eq!((first.first_lba, first.sectors), (44_990, 32));
        let (_dir, plain) = opened(&set, &SCRIPT_RULES);
        assert_eq!(disc.boot(), plain.unwrap().boot());
        // An installation's rules want the track to start where a pressed disc's does.
        assert_eq!(refused(&early, &INSTALL_RULES), starts);

        // Without a sector at 45000 there is no boot area to read under any rules.
        let late = without_first(set, "track3.bin", 1);
        assert_eq!(
            refused(&late, &SCRIPT_RULES),
            said("highDensityStart", HOLDS_WORDS)
        );
        assert_eq!(refused(&late, &INSTALL_RULES), starts);
        // Nor without a data track at all.
        let audio_only = DiscSet {
            files: vec![
                (
                    "track4.bin".to_owned(),
                    SparseFile::holding(vec![0; RAW_SECTOR]),
                ),
                (
                    "audio.cue".to_owned(),
                    SparseFile::holding(
                        b"FILE \"track4.bin\" BINARY\nTRACK 04 AUDIO\nINDEX 01 00:00:00\n".into(),
                    ),
                ),
            ],
            sheet: "audio.cue".to_owned(),
        };
        let holds = said("highDensityStart", HOLDS_WORDS);
        assert_eq!(refused(&audio_only, &SCRIPT_RULES), holds);
        assert_eq!(refused(&audio_only, &INSTALL_RULES), starts);
    }

    #[test]
    fn copies_take_at_most_512_sectors_and_stop_when_cancelled() {
        // The GDI set with its second data track 514 sectors long, each sector's user data a byte
        // of its own over and over.
        let sectors = FORMAT_CHUNK_SECTORS + 2;
        let fill = |sector: usize| vec![sector.to_le_bytes()[0]; FORMAT_SECTOR];
        let mut set = DiscSet::full_gdi(Edc::Zero);
        let long: Vec<u8> = (0..sectors)
            .flat_map(|sector| raw_sector(45_100 + sector as u32, &fill(sector), Edc::Zero))
            .collect();
        *set.file_mut("track05.bin") = SparseFile::holding(long);
        // A file of all of it but the last sector's bytes past its seventh.
        let size = (sectors - 1) * FORMAT_SECTOR + 7;
        let file = disc_file("LONG.BIN", 45_100, size as u32);
        let expected: Vec<u8> = (0..sectors).flat_map(fill).take(size).collect();
        let (_dir, disc) = opened(&set, &SCRIPT_RULES);
        let mut disc = disc.unwrap();

        // 512 sectors at once, then the two left, the last cut to the file's end.
        let mut writer = Recording::default();
        let digest = disc.copy(&file, &mut writer, &Cancel::default()).unwrap();
        let first = FORMAT_CHUNK_SECTORS * FORMAT_SECTOR;
        assert_eq!(writer.lens(), [first, FORMAT_SECTOR + 7]);
        assert!(writer.joined() == expected);
        assert_eq!(digest, Sha256Digest::of(&expected));
        assert!(disc.read(45_100, size as u32).unwrap() == expected);

        // Asked to stop as its first chunk is written, the copy stops before the next.
        let cancel = Cancel::default();
        let mut stopping = Recording {
            stop: Some(&cancel),
            ..Recording::default()
        };
        let error = disc.copy(&file, &mut stopping, &cancel).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        assert_eq!(stopping.lens(), [first]);
        // Asked before it starts, it writes nothing; a file of no bytes takes no chunk, so it has
        // nothing to stop.
        let mut idle = Recording::default();
        let error = disc.copy(&file, &mut idle, &cancel).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        let empty = DiscFile {
            size: 0,
            ..file.clone()
        };
        let digest = disc.copy(&empty, &mut idle, &cancel).unwrap();
        assert_eq!(digest, Sha256Digest::of(&[]));
        assert!(idle.writes.is_empty());

        // A write refused fails the copy as a write, never as a read of the dump: it names the disc
        // file it was copying by its place in the disc's tree, and keeps what the writer answered,
        // which tells a full disk.
        let error = disc.copy(&file, &mut Full, &Cancel::default()).unwrap_err();
        assert!(
            matches!(&error, MediaError::Write { file: copied, source }
                if *copied == file.path && source.kind() == io::ErrorKind::StorageFull),
            "{error:?}"
        );
        assert_eq!(error.rule(), "io");
        assert!(error.to_string().contains("LONG.BIN"), "{error}");
    }

    #[test]
    fn an_opening_stops_where_its_volume_is_read_once_its_job_is_asked_to() {
        // Asked to stop before it starts, the opening reads the disc's head and its volume
        // descriptor, reads of fixed sizes, and stops before the first chunk of the root
        // directory, under either rules. Asked nothing, it opens.
        let stopped = Cancel::default();
        stopped.cancel();
        let sets = [
            (DiscSet::small(Edc::Zero), SCRIPT_RULES),
            (
                DiscSet::full(Edc::Computed).with_audio_files(),
                INSTALL_RULES,
            ),
        ];
        for (set, rules) in sets {
            let (_dir, sheet) = written(&set);
            let error = GdRom::open(&sheet, &rules, &quiet(&stopped)).unwrap_err();
            assert!(matches!(error, MediaError::Cancelled), "{error:?}");
            assert!(GdRom::open(&sheet, &rules, &quiet(&Cancel::default())).is_ok());
        }
    }

    #[test]
    #[ignore = "needs the player's dump"]
    fn the_players_disc_places_its_tracks_and_walks_its_files() {
        let sheet = players_disc();
        // Opened by an installation's rules: every track's file there, the high-density area's
        // first data track at 45000, and every sector read held to its code, the boot area's,
        // the volume descriptor's, every directory's and SIZES.ROM's among them. A failure says
        // which rule the disc breaks, never what it holds.
        let mut disc = GdRom::open(&sheet, &INSTALL_RULES, &quiet(&Cancel::default()))
            .unwrap_or_else(|error| panic!("the disc is refused: {}", error.rule()));
        // Its three data tracks, each placed by its first sector's header: the single-density
        // area's, and the high-density area's two on either side of its audio track, the second
        // 225 sectors into its file.
        let tracks: Vec<(u64, i64, u64)> = disc
            .data_tracks()
            .iter()
            .map(|track| (track.index, track.first_lba, track.sectors))
            .collect();
        assert_eq!(
            tracks,
            [(0, 0, 850), (0, 45_000, 207_436), (225, 253_187, 295_963)]
        );
        // Its boot area names the US release, and its volume the game.
        let boot = disc.boot();
        assert!(boot.product() == "T9710N", "another product");
        assert!(boot.field(VERSION) == b"V1.003", "another version");
        assert!(boot.field(AREA) == b" U      ", "another area");
        assert!(boot.title() == "GAUNTLET LEGENDS", "another title");
        assert!(disc.volume() == "GAUNT1", "another volume");
        // Its volume's tree lists 156 files, 606,599,222 bytes in all.
        let files = disc.files();
        assert_eq!(files.len(), 156);
        let bytes: u64 = files.iter().map(|file| u64::from(file.size)).sum();
        assert_eq!(bytes, 606_599_222);
        // Its root holds SIZES.ROM, within the most the table may hold, and DISK.ROM, of
        // 207,642,624 bytes. The table, read from the disc, lists 1,361 files DISK.ROM holds and
        // 180 the disc keeps outside it.
        let (sizes, disk) = (at_root(&disc, "SIZES.ROM"), at_root(&disc, "DISK.ROM"));
        assert!(sizes.size <= MAX_SIZES_ROM_LEN, "SIZES.ROM is too long");
        assert_eq!(disk.size, 207_642_624);
        let table = disc
            .read(sizes.lba, sizes.size)
            .unwrap_or_else(|error| panic!("SIZES.ROM cannot be read: {}", error.rule()));
        let packed = packed_files(&table, u64::from(disk.size))
            .unwrap_or_else(|error| panic!("SIZES.ROM is refused: {}", error.rule()));
        let outside = packed
            .iter()
            .filter(|file| file.place == PackedPlace::External)
            .count();
        assert_eq!((packed.len() - outside, outside), (1_361, 180));
    }
}
