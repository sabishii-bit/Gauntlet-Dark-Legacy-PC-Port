//! A GameCube disc image as the installer reads one: whole, as an ISO, or sparse, as a CISO, with
//! the header that tells the disc, held to the US disc of the game by one precedence of its rules,
//! which identifying an image and reading it to install from share, and the file table whose
//! `Gauntlet` and `carddemo` trees are installed, judged whole before any file is copied.

use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use gdl_files::{BLOCK_LEN, Job};
use sha2::{Digest, Sha256};

use crate::dump_file::open_regular;
use crate::{Container, DumpRefusal, GameCubeRule, MediaError, Region, Sha256Digest, Verdict};

mod ciso;
mod fst;

pub(crate) use ciso::CISO_MARK;
use ciso::Ciso;
pub use fst::{DiscEntry, DiscEntryKind, MAX_DISC_ENTRIES};
use fst::{FST_ROW_LEN, Walked};

/// The most bytes a GameCube disc holds: a mini-DVD's. No image is read past it, and the files
/// of its tree together hold no more.
pub const DISC_SIZE: u64 = 1_459_978_240;

/// The most bytes a disc's file table may take, as the installer reads one: far more than the
/// game's needs, and the most any read of an image asks for at once.
pub const MAX_FST_LEN: u32 = 16 << 20;

/// The most bytes one read of an image asks for: a whole file table's. A longer read is made in
/// pieces of this length.
const MOST_READ_LEN: usize = MAX_FST_LEN as usize;

/// The highest revision of the US disc: the game was pressed as revisions 0 and 1.
pub const MAX_REVISION: u8 = 1;

/// The game ID of the US disc: the GameCube's letter, the game's two, North America's, and the
/// publisher's two.
pub const US_GAME_ID: [u8; 6] = *b"GUNE5D";

/// Where a disc's header keeps the mark every GameCube disc carries.
pub(crate) const GAMECUBE_MARK_AT: usize = 0x1C;

/// That mark.
pub(crate) const GAMECUBE_MARK: [u8; 4] = [0xC2, 0x33, 0x9F, 0x3D];

/// How many bytes of a disc's start its header takes, as the installer reads it: through the
/// place of the file table.
const DISC_HEADER_LEN: usize = 0x440;

/// How many bytes the game ID takes, at the header's start.
const GAME_ID_LEN: usize = US_GAME_ID.len();

/// Where the game ID gives the region the disc was made for.
const REGION_AT: usize = 3;

/// Where the header gives the disc's number among the game's discs.
const DISC_NUMBER_AT: usize = 6;

/// The number of the game's one disc.
const US_DISC_NUMBER: u8 = 0;

/// Where the header gives the disc's revision.
const REVISION_AT: usize = 7;

/// Where the header keeps the game's title, ended by a NUL when it is shorter than its field.
const TITLE_AT: usize = 0x20;

/// How many bytes the title's field takes.
const TITLE_LEN: usize = 0x3E0;

/// The most characters of the title that identification shows.
const MAX_TITLE_CHARS: usize = 64;

/// Where an NKit image, a disc rebuilt without its padding, keeps its mark.
const NKIT_AT: usize = 0x200;

/// That mark.
const NKIT_MARK: [u8; 4] = *b"NKIT";

/// Where the header gives the file table's offset on the disc, a big-endian word.
const FST_OFFSET_AT: usize = 0x424;

/// Where the header gives the file table's length, a big-endian word.
const FST_SIZE_AT: usize = 0x428;

/// The fewest bytes a file table takes: its root's row.
const MIN_FST_LEN: u32 = FST_ROW_LEN as u32;

/// The region letters a game ID gives, each with its region.
const REGION_LETTERS: [(u8, Region); 3] = [
    (b'E', Region::NorthAmerica),
    (b'P', Region::Europe),
    (b'J', Region::Japan),
];

/// What the installer answers for a read that reaches past the disc the image holds.
const OUTSIDE_IMAGE: &str = "Disc file extends beyond the image";

/// What it answers for an ISO that ends before a read it is asked for.
const TRUNCATED_ISO: &str = "Truncated ISO data";

/// What it answers for a file table placed inside the header, or of a length no table has.
const INVALID_FST_EXTENT: &str = "Invalid GameCube filesystem extent";

/// A GameCube disc image open for reading, judged whole as the installer judges one before it
/// copies anything: its container, its disc's header held to the US disc, and the file table,
/// every entry of it, with the `Gauntlet` and `carddemo` trees it keeps. The image is only ever
/// read.
pub struct GameCubeImage {
    /// The image, opened only for reading.
    file: File,
    /// Where it was opened, which a failed read names.
    path: PathBuf,
    /// How it keeps the disc's bytes.
    layout: Layout,
    /// How many bytes of disc it holds.
    size: u64,
    /// Its disc's header.
    header: DiscHeader,
    /// The entries of the `Gauntlet` and `carddemo` trees, in the file table's order.
    entries: Vec<DiscEntry>,
    /// How many bytes their files hold together.
    files_len: u64,
}

impl GameCubeImage {
    /// Opens the image at `path` and judges it whole in the installer's order: its container, its
    /// disc's header against the US disc, the file table's extent, then every entry of the table,
    /// and the trees it keeps. The image is opened only once it is seen to be a regular file, so
    /// that a folder, a FIFO or a device is refused unopened. The first rule it breaks is the
    /// refusal, in the installer's words where it has words for it.
    pub fn open(path: &Path) -> Result<GameCubeImage, MediaError> {
        let (mut file, len) = open_regular(path)
            .map_err(|error| MediaError::io(path, error))?
            .ok_or_else(|| MediaError::names_no_regular_file(path))?;
        let judged = judge(&mut file, len, path)?;
        Ok(GameCubeImage {
            file,
            path: path.to_path_buf(),
            layout: judged.layout,
            size: judged.size,
            header: judged.header,
            entries: judged.walked.entries,
            files_len: judged.walked.files_len,
        })
    }

    /// The container the image keeps the disc in: an ISO or a CISO.
    pub fn container(&self) -> Container {
        self.layout.container()
    }

    /// The disc's header.
    pub fn header(&self) -> &DiscHeader {
        &self.header
    }

    /// The disc's revision.
    pub fn revision(&self) -> u8 {
        self.header.revision
    }

    /// The entries of the disc's `Gauntlet` and `carddemo` trees, the two folders included, in
    /// the file table's order and spelt as the disc spells them.
    pub fn entries(&self) -> &[DiscEntry] {
        &self.entries
    }

    /// How many bytes the files of those trees hold together.
    pub fn files_len(&self) -> u64 {
        self.files_len
    }

    /// The bytes of the file `entry`, read whole when it holds no more than `most` bytes, and
    /// otherwise its first `most` + 1, so that a caller can tell a file longer than it takes
    /// without reading it all. `most` bounds what is read and the room made for it, so a caller
    /// passes the most it means to take, such as [`MAX_TPL_LEN`](crate::MAX_TPL_LEN) for an
    /// icon. A folder holds none.
    pub fn read_entry(&mut self, entry: &DiscEntry, most: u64) -> Result<Vec<u8>, MediaError> {
        let DiscEntryKind::File { offset, size } = entry.kind else {
            return Ok(Vec::new());
        };
        let len = size.min(most.saturating_add(1));
        self.layout
            .read_vec(&mut self.file, &self.path, self.size, offset, len)
    }

    /// Writes the bytes of the file `entry` to `to` and gives the SHA-256 digest of what it wrote,
    /// read at most [`BLOCK_LEN`] bytes at a time: the job's cancel is looked at before each block
    /// and its progress told the bytes of the file done after each. A write `to` refuses fails
    /// the copy as [`MediaError::Write`], which names the file and keeps what `to` answered, so
    /// that a caller can tell a full disk from an image it could not read. A folder copies
    /// nothing, and gives the digest of no bytes.
    pub fn copy(
        &mut self,
        entry: &DiscEntry,
        to: &mut dyn Write,
        job: &Job<'_>,
    ) -> Result<Sha256Digest, MediaError> {
        let mut hasher = Sha256::new();
        if let DiscEntryKind::File { offset, size } = entry.kind {
            let room = usize::try_from(size).map_or(BLOCK_LEN, |size| size.min(BLOCK_LEN));
            let mut block = vec![0; room];
            let mut done = 0;
            while done < size {
                job.cancel.check()?;
                let count = usize::try_from(size - done).map_or(room, |left| left.min(room));
                let part = &mut block[..count];
                let at = offset.saturating_add(done);
                self.layout
                    .read_at(&mut self.file, &self.path, self.size, at, part)?;
                to.write_all(part).map_err(|source| MediaError::Write {
                    file: entry.path.clone(),
                    source,
                })?;
                hasher.update(&*part);
                done += count as u64;
                job.progress.advance(done, size, Some(&entry.path));
            }
        }
        Ok(Sha256Digest(hasher.finalize().into()))
    }
}

impl fmt::Debug for GameCubeImage {
    /// How the image keeps the disc, how long the disc is, its header, and how many entries and
    /// bytes its kept trees hold: never a path, the image's or one of the disc's tree, which say
    /// what the player keeps where, nor a byte of a file.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        to.debug_struct("GameCubeImage")
            .field("layout", &self.layout)
            .field("size", &self.size)
            .field("header", &self.header)
            .field("entries", &self.entries.len())
            .field("files_len", &self.files_len)
            .finish_non_exhaustive()
    }
}

/// What a GameCube disc's header says of the disc, as far as the launcher reads it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscHeader {
    /// Whether the header carries the mark every GameCube disc carries.
    pub marked: bool,
    /// The game ID: the console's letter, the game's two, the region's one and the publisher's
    /// two.
    pub game_id: [u8; 6],
    /// The disc's number among the game's discs, from 0.
    pub disc_number: u8,
    /// The disc's revision.
    pub revision: u8,
    /// The title the header gives, up to its first NUL, each byte outside printable ASCII shown
    /// as U+FFFD, without the spaces around it and cut after 64 characters, or `None` when nothing
    /// is left.
    pub title: Option<String>,
    /// Whether the header carries the mark of an NKit image.
    pub nkit: bool,
}

impl DiscHeader {
    /// The header the first [`DISC_HEADER_LEN`] bytes of a disc, `bytes`, give.
    fn of(bytes: &[u8; DISC_HEADER_LEN]) -> DiscHeader {
        DiscHeader {
            marked: bytes[GAMECUBE_MARK_AT..].starts_with(&GAMECUBE_MARK),
            game_id: bytes
                .first_chunk::<GAME_ID_LEN>()
                .copied()
                .unwrap_or_default(),
            disc_number: bytes[DISC_NUMBER_AT],
            revision: bytes[REVISION_AT],
            title: title_of(&bytes[TITLE_AT..TITLE_AT + TITLE_LEN]),
            nkit: bytes[NKIT_AT..].starts_with(&NKIT_MARK),
        }
    }

    /// The region the game ID names by its letter, or `None` for a letter of no region told here.
    pub fn region(&self) -> Option<Region> {
        let letter = self.game_id[REGION_AT];
        let named = REGION_LETTERS.iter().find(|(held, _)| *held == letter);
        named.map(|(_, region)| *region)
    }
}

/// How a disc image keeps the disc's bytes.
#[derive(Debug)]
pub(crate) enum Layout {
    /// Whole, each byte of the disc at its own offset.
    Iso,
    /// Sparse, as a CISO keeps them.
    Ciso(Ciso),
}

impl Layout {
    /// The container an image of the layout is.
    pub(crate) fn container(&self) -> Container {
        match self {
            Layout::Iso => Container::GameCubeIso,
            Layout::Ciso(_) => Container::GameCubeCiso,
        }
    }

    /// The `len` bytes of the disc from `offset` on, read as [`Layout::read_at`] reads them: a
    /// read that reaches past the disc is refused before room for it is made.
    fn read_vec<R: Read + Seek>(
        &self,
        source: &mut R,
        path: &Path,
        size: u64,
        offset: u64,
        len: u64,
    ) -> Result<Vec<u8>, MediaError> {
        let end = offset.checked_add(len);
        if end.is_none_or(|end| end > size) {
            return Err(outside_image());
        }
        let mut bytes = vec![0; usize::try_from(len).map_err(|_| outside_image())?];
        self.read_at(source, path, size, offset, &mut bytes)?;
        Ok(bytes)
    }

    /// Fills `buffer` with the bytes of the disc from `offset` on, read from `source`, the image
    /// at `path`, whose disc is `size` bytes long, asking `source` for no more than
    /// [`MAX_FST_LEN`] bytes at once. A read that reaches past the disc is refused before
    /// anything is read, and one the image ends before is refused as cut short.
    pub(crate) fn read_at<R: Read + Seek>(
        &self,
        source: &mut R,
        path: &Path,
        size: u64,
        offset: u64,
        buffer: &mut [u8],
    ) -> Result<(), MediaError> {
        let end = offset.checked_add(buffer.len() as u64);
        if end.is_none_or(|end| end > size) {
            return Err(outside_image());
        }
        match self {
            Layout::Iso => {
                let failed = |error| MediaError::io(path, error);
                source.seek(SeekFrom::Start(offset)).map_err(failed)?;
                for piece in buffer.chunks_mut(MOST_READ_LEN) {
                    if fill(source, piece).map_err(failed)? != piece.len() {
                        return Err(MediaError::gamecube(
                            GameCubeRule::TruncatedIso,
                            TRUNCATED_ISO,
                        ));
                    }
                }
                Ok(())
            }
            Layout::Ciso(ciso) => ciso.read_at(source, path, offset, buffer),
        }
    }
}

/// The layout of the disc image `source`, the file at `path` of `len` bytes, how many bytes of
/// disc it holds, and the header of that disc, read as the installer reads them: a CISO by its
/// mark, its header checked whole, and anything else as an ISO; either holding no more than a
/// whole disc, and at least its header. The header is given whatever it says: holding it to the
/// US disc is [`header_rule`]'s. Nothing past the header is read.
pub(crate) fn read_header<R: Read + Seek>(
    source: &mut R,
    len: u64,
    path: &Path,
) -> Result<(Layout, u64, DiscHeader), MediaError> {
    let (layout, size, start) = read_start(source, len, path)?;
    Ok((layout, size, DiscHeader::of(&start)))
}

/// What judging an image finds: its layout, how many bytes of disc it holds, its disc's header,
/// and what the walk of its file table keeps.
pub(crate) struct Judged {
    /// How the image keeps the disc's bytes.
    pub(crate) layout: Layout,
    /// How many bytes of disc it holds.
    pub(crate) size: u64,
    /// The disc's header.
    pub(crate) header: DiscHeader,
    /// The kept trees' entries and how many bytes their files hold.
    pub(crate) walked: Walked,
}

/// The disc image `source`, the file at `path` of `len` bytes, judged whole in the installer's
/// order: its container and its disc's header as [`read_header`] reads them, the header held to
/// the US disc by [`header_rule`]; the file table's extent, after the header and of a length from
/// its root's row to [`MAX_FST_LEN`]; then the table, read whole and walked, every entry judged
/// and the `Gauntlet` and `carddemo` trees kept. The first rule broken is the refusal, in the
/// installer's words where it has words for it. No read reaches past the disc the image holds, nor
/// asks for more than [`MAX_FST_LEN`] bytes at once.
pub(crate) fn judge<R: Read + Seek>(
    source: &mut R,
    len: u64,
    path: &Path,
) -> Result<Judged, MediaError> {
    let (layout, size, start) = read_start(source, len, path)?;
    let header = DiscHeader::of(&start);
    if let Some(refusal) = header_refusal(&header) {
        return Err(refusal);
    }
    let fst_offset = word_at(&start, FST_OFFSET_AT);
    let fst_len = word_at(&start, FST_SIZE_AT);
    if fst_offset < DISC_HEADER_LEN as u32 || !(MIN_FST_LEN..=MAX_FST_LEN).contains(&fst_len) {
        return Err(MediaError::gamecube(
            GameCubeRule::FstExtent,
            INVALID_FST_EXTENT,
        ));
    }
    let table = layout.read_vec(
        source,
        path,
        size,
        u64::from(fst_offset),
        u64::from(fst_len),
    )?;
    let walked = fst::walk(&table, size)?;
    Ok(Judged {
        layout,
        size,
        header,
        walked,
    })
}

/// The layout of the disc image `source`, the file at `path` of `len` bytes, how many bytes of
/// disc it holds, and the first [`DISC_HEADER_LEN`] bytes of that disc, read as [`read_header`]
/// describes.
fn read_start<R: Read + Seek>(
    source: &mut R,
    len: u64,
    path: &Path,
) -> Result<(Layout, u64, [u8; DISC_HEADER_LEN]), MediaError> {
    let failed = |error| MediaError::io(path, error);
    let mut mark = [0; CISO_MARK.len()];
    source.seek(SeekFrom::Start(0)).map_err(failed)?;
    // A file shorter than the mark is no CISO, and is not read past its end to be told so.
    let read = if len < mark.len() as u64 {
        0
    } else {
        fill(source, &mut mark).map_err(failed)?
    };
    let (layout, size) = if read == mark.len() && mark == CISO_MARK {
        let ciso = Ciso::read(source, len, path)?;
        let size = ciso.disc_len();
        (Layout::Ciso(ciso), size)
    } else {
        (Layout::Iso, len.min(DISC_SIZE))
    };
    let mut start = [0; DISC_HEADER_LEN];
    layout.read_at(source, path, size, 0, &mut start)?;
    Ok((layout, size, start))
}

/// The big-endian word at `at` of a disc's header, `start`, which holds it.
fn word_at(start: &[u8; DISC_HEADER_LEN], at: usize) -> u32 {
    start
        .get(at..)
        .and_then(<[u8]>::first_chunk)
        .map_or(0, |word| u32::from_be_bytes(*word))
}

/// How the disc whose header is `header` stands against the US disc, by the first rule of the
/// header it breaks, in the order the judging of an image to install from goes by too: the US
/// disc when it breaks none; content the launcher does not know when it carries no GameCube disc's
/// mark; another game for another game ID; another revision for the game of another region,
/// another disc number or a revision later than [`MAX_REVISION`]; and a container the launcher
/// does not read for an NKit image.
pub fn gamecube_verdict(header: &DiscHeader) -> Verdict {
    header_rule(header).map_or(Verdict::Supported, |rule| Verdict::Refused(rule.refusal()))
}

/// A rule of a disc's header that only the US disc's keeps.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HeaderRule {
    /// The header carries no GameCube disc's mark.
    NotGameCube,
    /// The game ID is another game's.
    OtherGame,
    /// The game ID is the game's, of another region.
    OtherRegion,
    /// The disc is another of the game's discs.
    DiscNumber,
    /// The revision is later than any the US disc was pressed as.
    Revision,
    /// The image is an NKit image.
    Nkit,
}

impl HeaderRule {
    /// How a disc whose header breaks the rule stands against the US disc.
    fn refusal(self) -> DumpRefusal {
        match self {
            HeaderRule::NotGameCube => DumpRefusal::Unrecognized,
            HeaderRule::OtherGame => DumpRefusal::OtherGame,
            HeaderRule::OtherRegion | HeaderRule::DiscNumber | HeaderRule::Revision => {
                DumpRefusal::OtherRevision
            }
            HeaderRule::Nkit => DumpRefusal::UnsupportedContainer,
        }
    }
}

/// The first rule `header` breaks, in the one order the judging and identification both go by:
/// what the disc is before the form its image takes, so that an NKit image of another region's
/// disc is that region's disc. `None` for the US disc.
fn header_rule(header: &DiscHeader) -> Option<HeaderRule> {
    let mut regionless = header.game_id;
    regionless[REGION_AT] = US_GAME_ID[REGION_AT];
    let rule = if !header.marked {
        HeaderRule::NotGameCube
    } else if regionless != US_GAME_ID {
        HeaderRule::OtherGame
    } else if header.game_id != US_GAME_ID {
        HeaderRule::OtherRegion
    } else if header.disc_number != US_DISC_NUMBER {
        HeaderRule::DiscNumber
    } else if header.revision > MAX_REVISION {
        HeaderRule::Revision
    } else if header.nkit {
        HeaderRule::Nkit
    } else {
        return None;
    };
    Some(rule)
}

/// The refusal of the disc whose header is `header`, by the rule [`header_rule`] gives it and in
/// the installer's words where it has words for it, or `None` for the US disc.
fn header_refusal(header: &DiscHeader) -> Option<MediaError> {
    /// What the installer answers for any disc but the US disc of the game.
    const SELECT_THE_US_DISC: &str = "Select a USA GameCube Gauntlet Dark Legacy disc (GUNE5D)";
    /// What it answers for another of the game's discs.
    const UNSUPPORTED_DISC_NUMBER: &str = "Unsupported disc number";
    /// What the launcher answers for a revision past the US disc's, before the revision.
    const UNSUPPORTED_REVISION: &str = "Unsupported disc revision";
    /// What it answers for an NKit image.
    const NKIT_NOT_READ: &str = "NKit images are not read: use the disc's plain ISO or CISO dump";

    let refusal = match header_rule(header)? {
        HeaderRule::NotGameCube => {
            MediaError::gamecube(GameCubeRule::NotGameCube, SELECT_THE_US_DISC)
        }
        HeaderRule::OtherGame => MediaError::gamecube(GameCubeRule::OtherGame, SELECT_THE_US_DISC),
        HeaderRule::OtherRegion => {
            MediaError::gamecube(GameCubeRule::OtherRegion, SELECT_THE_US_DISC)
        }
        HeaderRule::DiscNumber => {
            MediaError::gamecube(GameCubeRule::DiscNumber, UNSUPPORTED_DISC_NUMBER)
        }
        HeaderRule::Revision => MediaError::gamecube(
            GameCubeRule::Revision,
            format!("{UNSUPPORTED_REVISION}: {}", header.revision),
        ),
        HeaderRule::Nkit => MediaError::gamecube(GameCubeRule::Nkit, NKIT_NOT_READ),
    };
    Some(refusal)
}

/// The title a header's field `field` gives: up to its first NUL, each byte outside printable
/// ASCII as U+FFFD, without the spaces around it, cut after [`MAX_TITLE_CHARS`] characters, or
/// `None` when nothing is left.
fn title_of(field: &[u8]) -> Option<String> {
    let shown: String = field
        .iter()
        .take_while(|&&byte| byte != 0)
        .map(|&byte| {
            if byte == b' ' || byte.is_ascii_graphic() {
                char::from(byte)
            } else {
                char::REPLACEMENT_CHARACTER
            }
        })
        .collect();
    let title: String = shown.trim().chars().take(MAX_TITLE_CHARS).collect();
    (!title.is_empty()).then_some(title)
}

/// The refusal of a read that reaches past the disc an image holds.
fn outside_image() -> MediaError {
    MediaError::gamecube(GameCubeRule::OutsideImage, OUTSIDE_IMAGE)
}

/// Fills `buffer` from `source`'s place on, as far as `source` reaches, and gives how many bytes
/// it filled: fewer than `buffer` holds only where `source` ends.
fn fill<R: Read>(source: &mut R, buffer: &mut [u8]) -> io::Result<usize> {
    let mut filled = 0;
    while filled < buffer.len() {
        match source.read(&mut buffer[filled..]) {
            Ok(0) => break,
            Ok(read) => filled += read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(filled)
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::fmt;
    use std::fs;
    use std::io::{self, Cursor, Read, Seek, SeekFrom, Write};
    use std::path::Path;
    use std::sync::Mutex;

    use gdl_files::{BLOCK_LEN, Cancel, Job, NoProgress, PhaseId, Progress, RelPath};
    use gdl_fixtures::{
        FstEntry, SparseFile, edited, fst, icon_iso, iso_of, synthetic_ciso, synthetic_iso, tpl,
        unhex,
    };
    use serde_json::Value;
    use tempfile::TempDir;

    use super::fst::tests::{EXTRAS_AT, GAME_ENTRIES, game_rows, game_table};
    use super::{
        DiscEntry, DiscEntryKind, DiscHeader, GameCubeImage, Judged, Layout, MAX_REVISION,
        US_GAME_ID, gamecube_verdict, header_refusal, header_rule, judge, read_header,
    };
    use crate::{Container, DumpRefusal, MediaError, Region, Sha256Digest, Verdict};

    /// The installer's disc cases, which the reader is held to.
    const GAMECUBE_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/installer/gamecube-vectors.json"
    ));

    /// What the vectors hold, each once: a key they gain is refused until a test reads it.
    const VECTOR_KEYS: [&str; 6] = ["cases", "entries", "format", "images", "reads", "version"];

    /// The path the tests' images are named by in the refusals of their reads.
    const IMAGE: &str = "disc.iso";

    /// The cases the installer refused in words of its own that the launcher does not repeat: a
    /// name it quoted as Python quotes one, and a name of a byte outside ASCII, which it refused
    /// only as its decoder failed.
    const OWN_WORDS: [&str; 2] = ["name", "nonAscii"];

    /// The rules the vectors' cases refuse an image by: all but those whose images are too large
    /// or too odd for the vectors, which tests of their own reach.
    const VECTOR_RULES: [&str; 21] = [
        "cisoHeader",
        "cisoBlockSize",
        "cisoMap",
        "cisoData",
        "outsideImage",
        "notGameCube",
        "otherGame",
        "otherRegion",
        "discNumber",
        "revision",
        "nkit",
        "fstExtent",
        "fstRoot",
        "fstEntry",
        "unsafeName",
        "duplicatePath",
        "nesting",
        "truncatedFile",
        "rootNotFolder",
        "missingRoots",
        "missingData",
    ];

    /// How many bytes of a disc's start its header takes. The format's facts are given here apart
    /// from the reader's own constants, so that a reader that looks elsewhere is caught.
    const FORMAT_HEADER_LEN: usize = 0x440;

    /// How many blocks a CISO's map flags.
    const FORMAT_CISO_BLOCKS: u64 = 32_760;

    /// Where a disc's header gives the region's letter of its game ID.
    const FORMAT_REGION_AT: usize = 3;

    /// Where the header gives the disc's revision.
    const FORMAT_REVISION_AT: usize = 7;

    /// Where the header keeps the title.
    const FORMAT_TITLE_AT: usize = 0x20;

    /// How many bytes the title's field takes.
    const FORMAT_TITLE_LEN: usize = 0x3E0;

    /// Where an NKit image keeps its mark.
    const FORMAT_NKIT_AT: usize = 0x200;

    /// Where the header gives the file table's offset, a big-endian word.
    const FORMAT_FST_OFFSET_AT: usize = 0x424;

    /// Where it gives the file table's length, a big-endian word.
    const FORMAT_FST_SIZE_AT: usize = 0x428;

    /// How many bytes a row of the file table takes, the root's included.
    const FORMAT_FST_ROW_LEN: usize = 12;

    /// Where the root's row gives how many rows the table holds.
    const FORMAT_ROOT_COUNT_AT: usize = 8;

    /// Where the installer tests' disc keeps its file table.
    const FORMAT_FST_AT: usize = 0x800;

    /// How many bytes that table takes.
    const FORMAT_FST_LEN: usize = 172;

    /// The most bytes a file table may take, and so the most any read asks for at once.
    const FORMAT_MAX_FST_LEN: usize = 16 << 20;

    /// The most bytes a GameCube disc holds.
    const FORMAT_DISC_SIZE: u64 = 1_459_978_240;

    /// The most entries the kept trees may hold.
    const FORMAT_MAX_ENTRIES: usize = 10_000;

    /// Where the installer tests' disc's icon lies, and how long its four-byte stand-in is.
    const FORMAT_ICON_AT: usize = 0x2900;

    /// The vectors, read, of their format and version and holding nothing but what the tests know.
    fn vectors() -> Value {
        let vectors: Value =
            serde_json::from_str(GAMECUBE_VECTORS).expect("the GameCube vectors are JSON");
        assert_eq!(vectors["format"], "gdl-gamecube-vectors");
        assert_eq!(vectors["version"], 1);
        let object = vectors.as_object().expect("the vectors are an object");
        let keys: Vec<&str> = object.keys().map(String::as_str).collect();
        assert_eq!(keys, VECTOR_KEYS, "the vectors hold other keys");
        vectors
    }

    /// A string of the vectors.
    fn text(value: &Value) -> &str {
        value.as_str().expect("a string")
    }

    /// A number of the vectors.
    fn number(value: &Value) -> u64 {
        value.as_u64().expect("a number")
    }

    /// The image the vectors name `name`, as the installer's tests build it.
    fn image(name: &str) -> Vec<u8> {
        match name {
            "iso" => synthetic_iso(),
            "ciso" => synthetic_ciso(&synthetic_iso()),
            "isoWithIcon" => icon_iso(),
            "empty" => Vec::new(),
            other => panic!("the vectors name no image {other:?}"),
        }
    }

    /// The image of the vectors' case `case`: its base with its edits made.
    fn case_image(case: &Value) -> Vec<u8> {
        edited(&image(text(&case["base"])), &case["edits"])
    }

    /// The layout, the disc's length and the header of the image `bytes`, read in memory.
    fn header_of(bytes: &[u8]) -> Result<(Layout, u64, DiscHeader), MediaError> {
        read_header(
            &mut Cursor::new(bytes),
            bytes.len() as u64,
            Path::new(IMAGE),
        )
    }

    /// The image `bytes` judged whole in memory.
    fn judged(bytes: &[u8]) -> Result<Judged, MediaError> {
        judge(
            &mut Cursor::new(bytes),
            bytes.len() as u64,
            Path::new(IMAGE),
        )
    }

    /// The image `bytes` judged whole in memory, without what the judging keeps of it.
    fn judged_unit(bytes: &[u8]) -> Result<(), MediaError> {
        judged(bytes).map(|_| ())
    }

    /// The image `bytes` written to a file of a folder of its own, and opened from there.
    fn opened(bytes: &[u8]) -> (TempDir, Result<GameCubeImage, MediaError>) {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(IMAGE);
        fs::write(&path, bytes).unwrap();
        let opened = GameCubeImage::open(&path);
        (dir, opened)
    }

    /// The image `bytes`, opened, which must open.
    fn open(bytes: &[u8]) -> (TempDir, GameCubeImage) {
        let (dir, opened) = opened(bytes);
        (dir, opened.unwrap_or_else(|error| panic!("{error}")))
    }

    /// The rule and the words of `outcome`'s refusal, which must be one by a rule of the format.
    fn refusal<T: fmt::Debug>(outcome: Result<T, MediaError>) -> (&'static str, String) {
        let error = outcome.expect_err("a refusal");
        assert!(matches!(error, MediaError::GameCube { .. }), "{error:?}");
        (error.rule(), error.to_string())
    }

    /// The disc of the smallest tree the walk takes, with `extras` inside its `Gauntlet` folder,
    /// every file empty: no longer than its header and its file table.
    fn game_disc(extras: &[FstEntry]) -> Vec<u8> {
        let table = game_table(extras);
        let len = FORMAT_FST_AT + table.len();
        iso_of(&table, &[], len)
    }

    /// `image` with `bytes` written over it from `at`.
    fn with_bytes(image: &[u8], at: usize, bytes: &[u8]) -> Vec<u8> {
        let mut changed = image.to_vec();
        changed[at..at + bytes.len()].copy_from_slice(bytes);
        changed
    }

    /// `image` with the big-endian word `value` written at `at`.
    fn with_word(image: &[u8], at: usize, value: usize) -> Vec<u8> {
        with_bytes(image, at, &u32::try_from(value).unwrap().to_be_bytes())
    }

    /// An image of `len` bytes held in memory, `start` from its first byte and noughts after,
    /// read through a reader that keeps how far and how much it is asked to read.
    fn held(start: Vec<u8>, len: u64) -> Counting<SparseFile> {
        Counting::new(SparseFile {
            len,
            chunks: vec![(0, start)],
            at: 0,
        })
    }

    /// The header of the image `bytes`, which must have one.
    fn header(bytes: &[u8]) -> DiscHeader {
        let (_, _, header) = header_of(bytes).unwrap();
        header
    }

    /// The installer tests' disc's header: the US disc's, revision 1, without a title.
    fn us_header() -> DiscHeader {
        DiscHeader {
            marked: true,
            game_id: US_GAME_ID,
            disc_number: 0,
            revision: 1,
            title: None,
            nkit: false,
        }
    }

    /// The entry of `image` whose path is `path`.
    fn entry(image: &GameCubeImage, path: &str) -> DiscEntry {
        let found = image
            .entries()
            .iter()
            .find(|entry| entry.path.as_str() == path);
        found
            .cloned()
            .unwrap_or_else(|| panic!("the disc holds no {path}"))
    }

    /// A reader that keeps how far any read it is asked for reaches and the longest it is asked
    /// for, of whatever it reads.
    struct Counting<R> {
        inner: R,
        at: u64,
        furthest: u64,
        longest: usize,
    }

    impl<R> Counting<R> {
        fn new(inner: R) -> Self {
            Counting {
                inner,
                at: 0,
                furthest: 0,
                longest: 0,
            }
        }
    }

    impl<R: Read> Read for Counting<R> {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            self.furthest = self.furthest.max(self.at + buffer.len() as u64);
            self.longest = self.longest.max(buffer.len());
            let read = self.inner.read(buffer)?;
            self.at += read as u64;
            Ok(read)
        }
    }

    impl<R: Seek> Seek for Counting<R> {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.at = self.inner.seek(to)?;
            Ok(self.at)
        }
    }

    /// Progress that writes down every step told, and asks `cancel` to stop at the first when it
    /// is given one.
    #[derive(Default)]
    struct Told<'a> {
        steps: Mutex<Vec<(u64, u64, String)>>,
        cancel: Option<&'a Cancel>,
    }

    impl Told<'_> {
        fn steps(&self) -> Vec<(u64, u64, String)> {
            self.steps.lock().unwrap().clone()
        }
    }

    impl Progress for Told<'_> {
        fn phase(&self, _: PhaseId) {}

        fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
            let item = item.map_or_else(String::new, |item| item.as_str().to_owned());
            self.steps.lock().unwrap().push((done, total, item));
            if let Some(cancel) = self.cancel {
                cancel.cancel();
            }
        }
    }

    /// A writer that takes nothing, as one on a full disk does.
    struct Full;

    impl Write for Full {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::ErrorKind::StorageFull.into())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn cases_follow_the_shared_vectors() {
        let vectors = vectors();
        let mut rules = BTreeSet::new();
        for case in vectors["cases"].as_array().unwrap() {
            let name = text(&case["name"]);
            let (_dir, opened) = opened(&case_image(case));
            if case["rule"].is_null() {
                // Taken by the installer and the launcher alike.
                assert_eq!(case["python"], "accepted", "{name}");
                assert!(case["detail"].is_null(), "{name}");
                opened.unwrap_or_else(|error| panic!("{name}: {error}"));
                continue;
            }
            // Refused by the vectors' rule and in the vectors' words, the installer's own where
            // it refused the image too and said why in words of its format.
            let expected = (text(&case["rule"]), text(&case["detail"]).to_owned());
            assert_eq!(refusal(opened), expected, "{name}");
            rules.insert(expected.0);
            if case["python"] == "refused" {
                if OWN_WORDS.contains(&name) {
                    assert_ne!(case["message"], case["detail"], "{name}");
                } else {
                    assert_eq!(case["message"], case["detail"], "{name}");
                }
            }
        }
        assert_eq!(rules, BTreeSet::from(VECTOR_RULES));
    }

    #[test]
    fn images_follow_the_shared_vectors() {
        let vectors = vectors();
        let images = &vectors["images"];
        assert_eq!(images["iso"]["revision"], 1);
        let icon = tpl(5, 8, 8);
        for (name, container) in [
            ("iso", Container::GameCubeIso),
            ("ciso", Container::GameCubeCiso),
            ("isoWithIcon", Container::GameCubeIso),
        ] {
            let bytes = image(name);
            assert_eq!(bytes.len(), images[name]["length"], "{name}");
            let (_dir, mut image) = open(&bytes);
            assert_eq!(image.container(), container, "{name}");
            assert_eq!(image.revision(), 1, "{name}");
            assert_eq!(*image.header(), us_header(), "{name}");
            let listed = vectors["entries"].as_array().unwrap();
            let entries = image.entries().to_vec();
            assert_eq!(entries.len(), listed.len(), "{name}");
            let cancel = Cancel::default();
            let job = Job {
                cancel: &cancel,
                progress: &NoProgress,
            };
            let mut files_len = 0;
            for (entry, listed) in entries.iter().zip(listed) {
                assert_eq!(entry.path.as_str(), text(&listed["path"]), "{name}");
                let mut copied = Vec::new();
                let digest = image.copy(entry, &mut copied, &job).unwrap();
                if listed["folder"] == true {
                    assert_eq!(entry.kind, DiscEntryKind::Folder, "{name}");
                    assert!(copied.is_empty());
                    assert_eq!(digest, Sha256Digest::of(&[]));
                    continue;
                }
                // The disc with an icon keeps the icon tests' TPL where the stand-in was.
                let (size, sha256) = if name == "isoWithIcon" && entry.path.first() == "carddemo" {
                    (icon.len() as u64, Sha256Digest::of(&icon).hex())
                } else {
                    (number(&listed["size"]), text(&listed["sha256"]).to_owned())
                };
                let offset = number(&listed["offset"]);
                assert_eq!(entry.kind, DiscEntryKind::File { offset, size }, "{name}");
                assert_eq!(digest.hex(), sha256, "{name}: {}", entry.path.as_str());
                assert_eq!(Sha256Digest::of(&copied), digest);
                assert_eq!(copied.len() as u64, size);
                files_len += size;
            }
            assert_eq!(image.files_len(), files_len, "{name}");
        }
    }

    #[test]
    fn an_iso_and_its_ciso_give_one_disc() {
        for iso in [synthetic_iso(), icon_iso()] {
            let (_iso_dir, mut whole) = open(&iso);
            let (_ciso_dir, mut sparse) = open(&synthetic_ciso(&iso));
            assert_eq!(whole.entries(), sparse.entries());
            assert_eq!(whole.header(), sparse.header());
            assert_eq!(whole.files_len(), sparse.files_len());
            assert_eq!((whole.revision(), sparse.revision()), (1, 1));
            let cancel = Cancel::default();
            let job = Job {
                cancel: &cancel,
                progress: &NoProgress,
            };
            for entry in whole.entries().to_vec() {
                let (mut from_whole, mut from_sparse) = (Vec::new(), Vec::new());
                let whole_digest = whole.copy(&entry, &mut from_whole, &job).unwrap();
                let sparse_digest = sparse.copy(&entry, &mut from_sparse, &job).unwrap();
                assert_eq!(from_whole, from_sparse, "{}", entry.path.as_str());
                assert_eq!(whole_digest, sparse_digest);
            }
            // The war's WAD runs from a block the CISO keeps into one it leaves out.
            let war = entry(&sparse, "Gauntlet/PDATA/WAR.WAD");
            let expected = [vec![b'A'; 16], vec![0; 16]].concat();
            assert_eq!(sparse.read_entry(&war, u64::MAX).unwrap(), expected);
        }
    }

    #[test]
    fn reads_follow_the_shared_vectors() {
        let vectors = vectors();
        for read in vectors["reads"].as_array().unwrap() {
            let bytes = image(text(&read["image"]));
            let (layout, size, _) = header_of(&bytes).unwrap();
            let mut buffer = vec![0xEE; usize::try_from(number(&read["size"])).unwrap()];
            let mut source = Cursor::new(bytes.as_slice());
            let offset = number(&read["offset"]);
            let path = Path::new(IMAGE);
            let outcome = layout.read_at(&mut source, path, size, offset, &mut buffer);
            if read["rule"].is_null() {
                assert_eq!(read["python"], "accepted", "{read}");
                outcome.unwrap();
                assert_eq!(buffer, unhex(&read["hex"]), "{read}");
            } else {
                let error = outcome.unwrap_err();
                assert!(matches!(error, MediaError::GameCube { .. }), "{error:?}");
                assert_eq!(error.rule(), text(&read["rule"]), "{read}");
                assert_eq!(error.to_string(), text(&read["detail"]), "{read}");
                assert_eq!(read["message"], read["detail"], "{read}");
            }
        }
    }

    #[test]
    fn a_ciso_reads_as_the_image_it_keeps() {
        for iso in [synthetic_iso(), icon_iso()] {
            let ciso = synthetic_ciso(&iso);
            let (iso_layout, iso_size, iso_header) = header_of(&iso).unwrap();
            let (ciso_layout, ciso_size, ciso_header) = header_of(&ciso).unwrap();
            assert!(matches!(iso_layout, Layout::Iso));
            assert!(matches!(ciso_layout, Layout::Ciso(_)));
            assert_eq!(ciso_header, iso_header);
            assert_eq!(iso_header, us_header());
            // An ISO holds the disc its bytes do; a CISO's map covers every block it flags.
            assert_eq!(iso_size, iso.len() as u64);
            assert_eq!(ciso_size, FORMAT_CISO_BLOCKS * 512);
            let path = Path::new(IMAGE);
            let mut source = Cursor::new(ciso.as_slice());
            let mut whole = vec![0xEE; iso.len()];
            ciso_layout
                .read_at(&mut source, path, ciso_size, 0, &mut whole)
                .unwrap();
            assert_eq!(whole, iso);
            // A read across each block's end gives what the ISO holds there.
            for end in (512..iso.len()).step_by(512) {
                let mut across = [0xEE; 3];
                let at = end as u64 - 1;
                ciso_layout
                    .read_at(&mut source, path, ciso_size, at, &mut across)
                    .unwrap();
                assert_eq!(across, iso[end - 1..end + 2], "{end}");
            }
            // Past the ISO's bytes the CISO's disc holds zeros, and past its map nothing.
            let mut past = [0xEE; 16];
            let at = iso.len() as u64;
            ciso_layout
                .read_at(&mut source, path, ciso_size, at, &mut past)
                .unwrap();
            assert_eq!(past, [0; 16]);
            let error = ciso_layout
                .read_at(&mut source, path, ciso_size, ciso_size - 1, &mut past)
                .unwrap_err();
            assert_eq!(error.rule(), "outsideImage");
        }
    }

    #[test]
    fn a_short_iso_is_refused_as_cut_short() {
        // The disc's length comes from the file's when the image is opened: a file that ends
        // before a read within that length, as one cut short since then does, is refused so.
        let iso = synthetic_iso();
        let (layout, size, _) = header_of(&iso).unwrap();
        let mut source = Cursor::new(&iso[..iso.len() - 1]);
        let mut last = [0xEE; 2];
        let at = size - 2;
        let error = layout
            .read_at(&mut source, Path::new(IMAGE), size, at, &mut last)
            .unwrap_err();
        assert_eq!(
            (error.rule(), error.to_string()),
            ("truncatedIso", "Truncated ISO data".to_owned())
        );
        // So is a CISO whose file ends inside a block its map keeps.
        let ciso = synthetic_ciso(&iso);
        let (layout, size, _) = header_of(&ciso).unwrap();
        let mut source = Cursor::new(&ciso[..ciso.len() - 1]);
        let mut whole = vec![0xEE; iso.len()];
        let error = layout
            .read_at(&mut source, Path::new(IMAGE), size, 0, &mut whole)
            .unwrap_err();
        assert_eq!(
            (error.rule(), error.to_string()),
            ("cisoData", "Truncated CISO data".to_owned())
        );
    }

    #[test]
    fn the_header_rule_and_the_verdict_agree() {
        // The verdict of each case whose container is read is what its header's rule tells of the
        // disc, and the refusal the image is opened with is that rule's; a case refused only past
        // the header is the US disc's as far as it tells.
        let vectors = vectors();
        for case in vectors["cases"].as_array().unwrap() {
            let name = text(&case["name"]);
            let bytes = case_image(case);
            let Ok((_, _, header)) = header_of(&bytes) else {
                continue;
            };
            let rule = case["rule"].as_str();
            let expected = match rule {
                Some("notGameCube") => Verdict::Refused(DumpRefusal::Unrecognized),
                Some("otherGame") => Verdict::Refused(DumpRefusal::OtherGame),
                Some("otherRegion" | "discNumber" | "revision") => {
                    Verdict::Refused(DumpRefusal::OtherRevision)
                }
                Some("nkit") => Verdict::Refused(DumpRefusal::UnsupportedContainer),
                _ => Verdict::Supported,
            };
            assert_eq!(gamecube_verdict(&header), expected, "{name}");
            assert_eq!(
                header_rule(&header).is_none(),
                expected == Verdict::Supported
            );
            if expected != Verdict::Supported {
                let refused = header_refusal(&header).map(|refusal| refusal.rule());
                assert_eq!(refused, rule, "{name}");
                assert_eq!(judged(&bytes).err().map(|error| error.rule()), rule);
            }
        }
    }

    #[test]
    fn a_header_is_told_by_the_first_rule_it_breaks() {
        // A header that breaks every rule at once, then one rule fewer each time: what the disc is
        // comes before the form its image takes.
        let mut header = DiscHeader {
            marked: false,
            game_id: *b"GALE01",
            disc_number: 1,
            revision: MAX_REVISION + 1,
            title: None,
            nkit: true,
        };
        let refused = |header: &DiscHeader| header_refusal(header).map(|error| error.rule());
        assert_eq!(refused(&header), Some("notGameCube"));
        header.marked = true;
        assert_eq!(refused(&header), Some("otherGame"));
        header.game_id = *b"GUNP5D";
        assert_eq!(refused(&header), Some("otherRegion"));
        header.game_id = US_GAME_ID;
        assert_eq!(refused(&header), Some("discNumber"));
        header.disc_number = 0;
        assert_eq!(refused(&header), Some("revision"));
        assert_eq!(
            header_refusal(&header).unwrap().to_string(),
            "Unsupported disc revision: 2"
        );
        header.revision = MAX_REVISION;
        assert_eq!(refused(&header), Some("nkit"));
        header.nkit = false;
        assert_eq!(refused(&header), None);
        assert_eq!(gamecube_verdict(&header), Verdict::Supported);
        // Revisions 0 and 1 are the US disc's.
        header.revision = 0;
        assert_eq!(refused(&header), None);
        // A game ID that differs from the game's but for its region's letter is another game's.
        for id in [b"GUNE5E", b"XUNE5D", b"GUNE6D", b"gune5d"] {
            header.game_id = *id;
            assert_eq!(refused(&header), Some("otherGame"), "{id:?}");
        }
    }

    #[test]
    fn the_title_is_read_and_bounded() {
        let iso = synthetic_iso();
        let title = |bytes: &[u8]| header(&with_bytes(&iso, FORMAT_TITLE_AT, bytes)).title;
        // The installer tests' disc gives none.
        assert_eq!(header(&iso).title, None);
        assert_eq!(title(b"Gauntlet"), Some("Gauntlet".to_owned()));
        // The spaces around it go, and nothing after its first NUL is read.
        assert_eq!(
            title(b"  Gauntlet Dark Legacy  \0Other"),
            Some("Gauntlet Dark Legacy".to_owned())
        );
        assert_eq!(title(b"   "), None);
        // A byte outside printable ASCII shows as U+FFFD.
        assert_eq!(
            title(b"Gauntl\xe9t\tII"),
            Some("Gauntl\u{fffd}t\u{fffd}II".to_owned())
        );
        // A field without a NUL is read to its end and cut after 64 characters.
        let full = vec![b'A'; FORMAT_TITLE_LEN];
        assert_eq!(title(&full), Some("A".repeat(64)));
        let cut = [vec![b' '; 10], vec![0xE9; FORMAT_TITLE_LEN - 10]].concat();
        assert_eq!(title(&cut), Some("\u{fffd}".repeat(64)));
        // An image whose header gives a title opens with it.
        let titled = with_bytes(&iso, FORMAT_TITLE_AT, b"Gauntlet\0");
        let (_dir, image) = open(&titled);
        assert_eq!(image.header().title.as_deref(), Some("Gauntlet"));
    }

    #[test]
    fn the_header_gives_the_discs_fields_and_its_region() {
        let iso = synthetic_iso();
        assert_eq!(header(&iso), us_header());
        assert_eq!(header(&iso).region(), Some(Region::NorthAmerica));
        let nkit = with_bytes(&iso, FORMAT_NKIT_AT, b"NKIT");
        assert!(header(&nkit).nkit);
        let revision = with_bytes(&iso, FORMAT_REVISION_AT, &[7]);
        assert_eq!(header(&revision).revision, 7);
        let regions = [
            (b'E', Some(Region::NorthAmerica)),
            (b'P', Some(Region::Europe)),
            (b'J', Some(Region::Japan)),
            (b'K', None),
            (b'e', None),
        ];
        for (letter, region) in regions {
            let lettered = with_bytes(&iso, FORMAT_REGION_AT, &[letter]);
            assert_eq!(header(&lettered).region(), region, "{letter}");
        }
        // Only the header's bytes are read: a disc of no more than them is read whole.
        let bare = iso[..FORMAT_HEADER_LEN].to_vec();
        assert_eq!(header(&bare), us_header());
        assert_eq!(
            refusal(header_of(&iso[..FORMAT_HEADER_LEN - 1])),
            (
                "outsideImage",
                "Disc file extends beyond the image".to_owned()
            )
        );
    }

    #[test]
    fn a_copy_stops_at_a_cancel_and_names_a_writers_failure() {
        // A file of two whole blocks and three bytes more, each byte telling its place.
        let len = 2 * BLOCK_LEN + 3;
        let at = 0x1_0000;
        let data: Vec<u8> = (0..len).map(|place| (place % 251) as u8).collect();
        let big = FstEntry::file("BIG.BIN", at, u32::try_from(len).unwrap());
        let table = game_table(&[big]);
        let disc = iso_of(&table, &[(at as usize, &data)], at as usize + len);
        let (_dir, mut image) = open(&disc);
        let big = entry(&image, "Gauntlet/BIG.BIN");

        // Copied whole, a block at a time, each block told as the file's bytes done.
        let cancel = Cancel::default();
        let told = Told::default();
        let job = Job {
            cancel: &cancel,
            progress: &told,
        };
        let mut copied = Vec::new();
        let digest = image.copy(&big, &mut copied, &job).unwrap();
        assert_eq!(copied, data);
        assert_eq!(digest, Sha256Digest::of(&data));
        let total = len as u64;
        let item = "Gauntlet/BIG.BIN".to_owned();
        let block = BLOCK_LEN as u64;
        assert_eq!(
            told.steps(),
            [
                (block, total, item.clone()),
                (2 * block, total, item.clone()),
                (total, total, item.clone()),
            ]
        );

        // A cancel asked for before the copy writes nothing; one asked for after the first block
        // stops it before the second.
        let cancelled = Cancel::default();
        cancelled.cancel();
        let job = Job {
            cancel: &cancelled,
            progress: &told,
        };
        let mut written = Vec::new();
        let error = image.copy(&big, &mut written, &job).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        assert!(written.is_empty());
        let cancel = Cancel::default();
        let stopping = Told {
            cancel: Some(&cancel),
            ..Told::default()
        };
        let job = Job {
            cancel: &cancel,
            progress: &stopping,
        };
        let error = image.copy(&big, &mut written, &job).unwrap_err();
        assert!(matches!(error, MediaError::Cancelled), "{error:?}");
        assert_eq!(written, data[..BLOCK_LEN]);
        assert_eq!(stopping.steps(), [(block, total, item)]);

        // A writer that refuses the bytes fails the copy, naming the file in the disc's tree and
        // keeping what the writer answered.
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let error = image.copy(&big, &mut Full, &job).unwrap_err();
        let MediaError::Write { file, source } = &error else {
            panic!("{error:?}");
        };
        assert_eq!(*file, big.path);
        assert_eq!(source.kind(), io::ErrorKind::StorageFull);
        assert_eq!(error.rule(), "io");
        // An empty file copies nothing and tells nothing.
        let war = entry(&image, "Gauntlet/PDATA/WAR.WAD");
        let told = Told::default();
        let job = Job {
            cancel: &cancel,
            progress: &told,
        };
        let digest = image.copy(&war, &mut Full, &job).unwrap();
        assert_eq!(digest, Sha256Digest::of(&[]));
        assert!(told.steps().is_empty());
    }

    #[test]
    fn an_entry_is_read_up_to_a_bound() {
        let (_dir, mut image) = open(&icon_iso());
        let icon = entry(&image, "carddemo/icon.tpl");
        let tpl = tpl(5, 8, 8);
        // A file no longer than the bound is read whole; a longer one, a byte past the bound.
        for (most, read) in [
            (u64::MAX, tpl.len()),
            (192, 192),
            (191, 192),
            (10, 11),
            (0, 1),
        ] {
            let bytes = image.read_entry(&icon, most).unwrap();
            assert_eq!(bytes, tpl[..read], "{most}");
        }
        // A folder holds no bytes.
        let folder = entry(&image, "carddemo");
        assert!(image.read_entry(&folder, u64::MAX).unwrap().is_empty());
        // An entry that reaches past the disc, which no image of the walk gives, is refused.
        let past = DiscEntry {
            path: icon.path.clone(),
            kind: DiscEntryKind::File {
                offset: u64::MAX - 1,
                size: 4,
            },
        };
        assert_eq!(refusal(image.read_entry(&past, 8)).0, "outsideImage");
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        assert_eq!(
            refusal(image.copy(&past, &mut Vec::new(), &job)).0,
            "outsideImage"
        );
    }

    #[test]
    fn the_entry_bound_holds_at_ten_thousand() {
        let fillers = |count: usize| -> Vec<FstEntry> {
            (0..count)
                .map(|index| FstEntry::file(&format!("F{index:05}"), 0, 0))
                .collect()
        };
        // The kept trees of ten thousand entries are taken whole.
        let most = fillers(FORMAT_MAX_ENTRIES - GAME_ENTRIES);
        let bound = judged(&game_disc(&most)).unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(bound.walked.entries.len(), FORMAT_MAX_ENTRIES);
        // One more is refused, by the bound's own words.
        let over = fillers(FORMAT_MAX_ENTRIES - GAME_ENTRIES + 1);
        assert_eq!(
            refusal(judged_unit(&game_disc(&over))),
            (
                "tooManyEntries",
                "The disc's Gauntlet and carddemo folders hold more than 10000 entries".to_owned()
            )
        );
        // Entries outside the kept trees count for nothing.
        let mut rows = game_rows(&most);
        rows.push(FstEntry::file("opening.bnr", 0, 0));
        let table = fst(&rows);
        let disc = iso_of(&table, &[], FORMAT_FST_AT + table.len());
        judged_unit(&disc).unwrap_or_else(|error| panic!("{error}"));
    }

    #[test]
    fn a_path_of_220_characters_passes_and_221_is_too_long() {
        // `Gauntlet/`, a folder of 200 characters, `/` and a file: 210 characters and the file's.
        let deep = |file: &str| {
            game_disc(&[
                FstEntry::folder(&"a".repeat(200), 1, EXTRAS_AT + 2),
                FstEntry::file(file, 0, 0),
            ])
        };
        let longest = "b".repeat(10);
        let (_dir, image) = open(&deep(&longest));
        let path = format!("Gauntlet/{}/{longest}", "a".repeat(200));
        assert_eq!(path.len(), 220);
        assert!(
            image
                .entries()
                .iter()
                .any(|entry| entry.path.as_str() == path)
        );
        let over = "b".repeat(11);
        let path = format!("Gauntlet/{}/{over}", "a".repeat(200));
        assert_eq!(path.len(), 221);
        let said = format!("Duplicate or overlong disc path: {path}");
        assert_eq!(refusal(opened(&deep(&over)).1), ("pathTooLong", said));
    }

    #[test]
    fn refusals_quote_what_they_take_up_to_one_bound() {
        // A path too long is said whole up to 256 characters; past that, by its length and its
        // start, quoted.
        let deep = game_disc(&[
            FstEntry::folder(&"a".repeat(200), 1, EXTRAS_AT + 3),
            FstEntry::folder(&"b".repeat(10), EXTRAS_AT, EXTRAS_AT + 3),
            FstEntry::file(&"c".repeat(200), 0, 0),
        ]);
        let path = format!(
            "Gauntlet/{}/{}/{}",
            "a".repeat(200),
            "b".repeat(10),
            "c".repeat(200)
        );
        assert_eq!(path.len(), 421);
        let said = format!(
            "Duplicate or overlong disc path of 421 characters beginning {:?}",
            &path[..256]
        );
        assert_eq!(refusal(judged_unit(&deep)), ("pathTooLong", said));
        // A name is quoted whole up to 256 characters, a name too long for a component included,
        // and past that by its length and its start.
        for (len, said) in [
            (
                201,
                format!("Unsafe disc path component: {:?}", "n".repeat(201)),
            ),
            (
                256,
                format!("Unsafe disc path component: {:?}", "n".repeat(256)),
            ),
            (
                300,
                format!(
                    "Unsafe disc path component of 300 characters beginning {:?}",
                    "n".repeat(256)
                ),
            ),
        ] {
            let disc = game_disc(&[FstEntry::file(&"n".repeat(len), 0, 0)]);
            assert_eq!(refusal(judged_unit(&disc)), ("unsafeName", said), "{len}");
        }
    }

    #[test]
    fn the_total_is_bounded_by_the_disc_size() {
        // Two files of a GiB each, both from the disc's start: each lies on a disc as long as a
        // whole one, and together they hold more than it.
        let gib = 0x4000_0000;
        let table = game_table(&[
            FstEntry::file("ONE.BIN", 0, gib),
            FstEntry::file("TWO.BIN", 0, gib),
        ]);
        let start = iso_of(&table, &[], FORMAT_FST_AT + table.len());
        let read = |table_end: usize, start: Vec<u8>, len: u64| {
            let mut source = held(start, len);
            let outcome = judge(&mut source, len, Path::new(IMAGE));
            // Nothing past the header and the file table is read.
            assert!(source.furthest <= table_end as u64, "{}", source.furthest);
            outcome
        };
        let table_end = FORMAT_FST_AT + table.len();
        assert_eq!(
            refusal(read(table_end, start, FORMAT_DISC_SIZE).map(|_| ())),
            ("totalSize", "Implausible total extracted size".to_owned())
        );
        // One file as long as the disc is the most the trees may hold.
        let whole = u32::try_from(FORMAT_DISC_SIZE).unwrap();
        let table = game_table(&[FstEntry::file("ALL.BIN", 0, whole)]);
        let table_end = FORMAT_FST_AT + table.len();
        let start = iso_of(&table, &[], table_end);
        let whole_disc =
            read(table_end, start, FORMAT_DISC_SIZE).unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(whole_disc.walked.files_len, FORMAT_DISC_SIZE);
        // A byte further and the file runs past the disc, even in an image a byte longer: an ISO
        // holds no more than a whole disc, whatever follows it.
        let table = game_table(&[FstEntry::file("ALL.BIN", 1, whole)]);
        let table_end = FORMAT_FST_AT + table.len();
        let start = iso_of(&table, &[], table_end);
        let past = (
            "truncatedFile",
            "Truncated disc file: Gauntlet/ALL.BIN".to_owned(),
        );
        for len in [FORMAT_DISC_SIZE, FORMAT_DISC_SIZE + 1] {
            let outcome = read(table_end, start.clone(), len);
            assert_eq!(refusal(outcome.map(|_| ())), past, "{len}");
        }
        let table = game_table(&[]);
        let table_end = FORMAT_FST_AT + table.len();
        let longer = read(
            table_end,
            iso_of(&table, &[], table_end),
            FORMAT_DISC_SIZE + 1,
        );
        let longer = longer.unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(longer.size, FORMAT_DISC_SIZE);
    }

    #[test]
    fn the_file_tables_extent_is_taken_up_to_its_edges() {
        // Each image is as long as a whole disc and held in memory: what the judging finds, how
        // far it read and the most it read at once.
        let judged_whole = |start: Vec<u8>| {
            let mut source = held(start, FORMAT_DISC_SIZE);
            let outcome = judge(&mut source, FORMAT_DISC_SIZE, Path::new(IMAGE)).map(|_| ());
            (outcome, source.furthest, source.longest)
        };
        let table = game_table(&[]);
        let disc = iso_of(&table, &[], FORMAT_FST_AT + table.len());
        // A table right after the header is read.
        let after = [&disc[..FORMAT_HEADER_LEN], table.as_slice()].concat();
        let after = with_word(&after, FORMAT_FST_OFFSET_AT, FORMAT_HEADER_LEN);
        let (outcome, furthest, _) = judged_whole(after);
        outcome.unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(furthest, (FORMAT_HEADER_LEN + table.len()) as u64);
        // A table of its root's row alone, the shortest, is walked, and holds nothing the game
        // needs.
        let root_only = with_word(&disc, FORMAT_FST_SIZE_AT, FORMAT_FST_ROW_LEN);
        let root_only = with_word(&root_only, FORMAT_FST_AT + FORMAT_ROOT_COUNT_AT, 1);
        let (outcome, ..) = judged_whole(root_only);
        assert_eq!(refusal(outcome).0, "missingRoots");
        // A table of the most bytes a table may take is read in one read of that many, and the
        // noughts after its names are no rows of it.
        let most = with_word(&disc, FORMAT_FST_SIZE_AT, FORMAT_MAX_FST_LEN);
        let (outcome, furthest, longest) = judged_whole(most);
        outcome.unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(longest, FORMAT_MAX_FST_LEN);
        assert_eq!(furthest, (FORMAT_FST_AT + FORMAT_MAX_FST_LEN) as u64);
        // A byte more is refused by the extent before anything past the header is read.
        let over = with_word(&disc, FORMAT_FST_SIZE_AT, FORMAT_MAX_FST_LEN + 1);
        let (outcome, furthest, _) = judged_whole(over);
        assert_eq!(
            refusal(outcome),
            ("fstExtent", "Invalid GameCube filesystem extent".to_owned())
        );
        assert_eq!(furthest, FORMAT_HEADER_LEN as u64);
    }

    #[test]
    fn a_long_read_is_made_in_pieces_of_a_file_tables_most() {
        // A file longer than the most a file table takes, read whole as an entry is read, is asked
        // of the image in pieces no longer than that, and given whole.
        let len = FORMAT_MAX_FST_LEN + 3;
        let at = 5;
        let marks = [
            (0, [1, 2]),
            (FORMAT_MAX_FST_LEN - 1, [3, 4]),
            (len - 2, [5, 6]),
        ];
        let sparse = SparseFile {
            len: FORMAT_DISC_SIZE,
            chunks: marks
                .iter()
                .map(|(place, mark)| ((at + place) as u64, mark.to_vec()))
                .collect(),
            at: 0,
        };
        let mut source = Counting::new(sparse);
        let bytes = Layout::Iso
            .read_vec(
                &mut source,
                Path::new(IMAGE),
                FORMAT_DISC_SIZE,
                at as u64,
                len as u64,
            )
            .unwrap();
        assert_eq!(bytes.len(), len);
        for (place, mark) in marks {
            assert_eq!(bytes[place..place + 2], mark, "{place}");
        }
        assert_eq!(source.longest, FORMAT_MAX_FST_LEN);
        assert_eq!(source.furthest, (at + len) as u64);
    }

    #[test]
    fn every_cut_and_every_changed_byte_is_refused_or_read() {
        // Each image is judged whole or refused by a rule of its format, never with a panic,
        // never reading past its length and never asking for more than a file table's most in one
        // read.
        let check = |bytes: &[u8]| -> bool {
            let len = bytes.len() as u64;
            let mut source = Counting::new(Cursor::new(bytes));
            let opened = match judge(&mut source, len, Path::new(IMAGE)) {
                Ok(judged) => {
                    for entry in &judged.walked.entries {
                        if let DiscEntryKind::File { offset, size } = entry.kind {
                            assert!(offset + size <= judged.size, "{} bytes", bytes.len());
                        }
                    }
                    true
                }
                Err(MediaError::GameCube { .. }) => false,
                Err(other) => panic!("{} bytes: {other:?}", bytes.len()),
            };
            assert!(source.furthest <= len, "{} bytes", bytes.len());
            assert!(
                source.longest <= FORMAT_MAX_FST_LEN,
                "{} bytes",
                bytes.len()
            );
            opened
        };
        // A cut ISO is read once it holds every file its table lists, the icon last of them; a
        // cut CISO, only whole.
        let iso = synthetic_iso();
        let ciso = synthetic_ciso(&iso);
        for cut in 0..=iso.len() {
            assert_eq!(check(&iso[..cut]), cut >= FORMAT_ICON_AT + 4, "{cut}");
        }
        for cut in 0..=ciso.len() {
            assert_eq!(check(&ciso[..cut]), cut == ciso.len(), "{cut}");
        }
        let changed = (0..FORMAT_HEADER_LEN).chain(FORMAT_FST_AT..FORMAT_FST_AT + FORMAT_FST_LEN);
        for at in changed {
            for value in [0x00, 0xFF, iso[at].wrapping_add(1)] {
                check(&with_bytes(&iso, at, &[value]));
            }
        }
    }

    #[test]
    fn an_images_debug_form_names_none_of_its_paths() {
        let (dir, image) = open(&synthetic_iso());
        let shown = format!("{image:?}");
        assert!(
            shown.starts_with("GameCubeImage { layout: Iso, size: 16384,"),
            "{shown}"
        );
        assert!(shown.contains("entries: 8, files_len: 41"), "{shown}");
        for name in ["Gauntlet", "carddemo", "PDATA", "WAR", "icon", IMAGE] {
            assert!(!shown.contains(name), "{shown}");
        }
        assert!(!shown.contains(&*dir.path().to_string_lossy()), "{shown}");
        let (_dir, sparse) = open(&synthetic_ciso(&synthetic_iso()));
        let shown = format!("{sparse:?}");
        assert!(
            shown.contains("layout: Ciso(Ciso { block: 512, kept: 5 })"),
            "{shown}"
        );
    }

    #[test]
    fn what_is_no_regular_file_is_refused_unopened() {
        let dir = tempfile::tempdir().unwrap();
        let error = GameCubeImage::open(dir.path()).unwrap_err();
        let MediaError::Io { path, source } = &error else {
            panic!("{error:?}");
        };
        assert_eq!(path, dir.path());
        assert_eq!(source.kind(), io::ErrorKind::InvalidInput);
        let missing = dir.path().join("missing.iso");
        let error = GameCubeImage::open(&missing).unwrap_err();
        let MediaError::Io { source, .. } = &error else {
            panic!("{error:?}");
        };
        assert_eq!(source.kind(), io::ErrorKind::NotFound);
    }

    #[cfg(unix)]
    #[test]
    fn a_fifo_is_never_opened() {
        use crate::dump_file::fifo;

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(IMAGE);
        fifo::make(&path);
        let opening = path.clone();
        let error = fifo::without_waiting(&path, move || {
            GameCubeImage::open(&opening)
                .err()
                .map(|error| error.to_string())
        });
        let said = format!("{}: the path names no regular file", path.display());
        assert_eq!(error, Some(said));
    }
}
