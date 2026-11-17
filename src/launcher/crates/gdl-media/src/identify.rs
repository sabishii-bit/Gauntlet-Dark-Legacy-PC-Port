//! What a dump is, told from its content and never from its name, for the reports that describe
//! it: the console it is for, the container it comes in, what it says of itself, and how it
//! stands against the dumps the launcher installs the expansions from. An N64 ROM is known by its
//! first word, a Dreamcast disc by its sheet, and every container the launcher does not read by a
//! mark of its own; each is read no further than telling it takes.

use std::fs;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use gdl_files::{Cancel, Job, NoProgress};

use crate::dump_file::open_regular;
use crate::gdrom::{head_of, names_gauntlet_legends, sheet_naming, sheet_of, without_mark};
use crate::{
    AREA, ByteOrder, DiscRule, IpBin, MAX_SHEET_LEN, MediaError, N64Rom, PRODUCT, RomHeader,
    SCRIPT_RULES, Sha1Digest, Sha256Digest, Sheet, SheetKind, SourceName, TrackMode, US_SHA1,
    VERSION, ascii_field, find_sheet, read_sheet,
};

/// How many bytes at a file's end identification reads: as many as the marks kept there take.
const TAIL_LEN: u64 = 16;

/// What a GameCube disc image kept sparse begins with.
const CISO_MARK: &[u8] = b"CISO";

/// Where a GameCube disc's header keeps the mark every GameCube disc carries.
const GAMECUBE_MARK_AT: usize = 0x1C;

/// That mark.
const GAMECUBE_MARK: &[u8] = &[0xC2, 0x33, 0x9F, 0x3D];

/// What a zip archive's first entry begins with.
const ZIP_MARK: &[u8] = b"PK\x03\x04";

/// What a 7-Zip archive begins with.
const SEVEN_ZIP_MARK: &[u8] = &[0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C];

/// What a CHD, a disc image compressed in hunks, begins with.
const CHD_MARK: &[u8] = b"MComprHD";

/// What the descriptor of an Alcohol 120% image, the MDS beside its MDF, begins with.
const MDS_MARK: &[u8] = b"MEDIA DESCRIPTOR";

/// How far before a DiscJuggler image's end it gives its version, a little-endian word.
const CDI_VERSION_FROM_END: usize = 8;

/// The versions a DiscJuggler image gives itself: 2, 3 and 3.5.
const CDI_VERSIONS: [u32; 3] = [0x8000_0004, 0x8000_0005, 0x8000_0006];

/// What a Nero image of the format's second version ends with, before the 64-bit place of its
/// chunks.
const NRG_MARK: [u8; 4] = *b"NER5";

/// How far before the image's end that mark lies.
const NRG_MARK_FROM_END: usize = 12;

/// What a Nero image of the format's first version ends with, before the 32-bit place of its
/// chunks.
const NRG_FIRST_MARK: [u8; 4] = *b"NERO";

/// How far before the image's end that mark lies.
const NRG_FIRST_MARK_FROM_END: usize = 8;

/// What the control file of a CloneCD image begins with, after any byte order mark.
const CLONE_CD_MARK: &[u8] = b"[CloneCD]";

/// Where an ISO 9660 image of cooked 2048-byte sectors keeps the identifier of its first volume
/// descriptor: sixteen sectors in, past the system area, after the descriptor's type.
const PLAIN_ISO_MARK_AT: usize = 0x8001;

/// That identifier.
const PLAIN_ISO_MARK: &[u8] = b"CD001";

/// The game code of the US cartridge: a cartridge's letter, the game's two letters and North
/// America's.
const US_GAME_CODE: [u8; 4] = *b"NGXE";

/// What the game code of each of the game's cartridges begins with: a cartridge's letter, then the
/// game's two letters, `GX`, or `GD` on the Japanese cartridge.
const GAME_CODE_STARTS: [&[u8]; 2] = [b"NGX", b"NGD"];

/// The letter that names each region in a boot area's field of areas, each in a place of its
/// own: Japan's first, then North America's, then Europe's.
const AREA_SYMBOLS: [(u8, Region); 3] = [
    (b'J', Region::Japan),
    (b'U', Region::NorthAmerica),
    (b'E', Region::Europe),
];

/// The console a dump is for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Console {
    /// The GameCube, whose disc the base game is installed from.
    GameCube,
    /// The Nintendo 64, whose cartridge the N64 Legends expansion is extracted from.
    Nintendo64,
    /// The Dreamcast, whose GD-ROM the Dreamcast Legends expansion is extracted from.
    Dreamcast,
}

impl Console {
    /// The console's stable machine name, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            Console::GameCube => "gamecube",
            Console::Nintendo64 => "n64",
            Console::Dreamcast => "dreamcast",
        }
    }
}

/// The container a dump comes in, as its content shows it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Container {
    /// An N64 ROM, in any of the three byte orders cartridges are copied in.
    N64Rom,
    /// A cue sheet, beside the raw files of a disc's tracks.
    CueSheet,
    /// A GDI, beside the raw files of a GD-ROM's tracks.
    Gdi,
    /// A GameCube disc image, whole.
    GameCubeIso,
    /// A GameCube disc image kept sparse, as a CISO keeps one.
    GameCubeCiso,
    /// A zip archive.
    Zip,
    /// A 7-Zip archive.
    SevenZip,
    /// A CHD, a disc image compressed in hunks.
    Chd,
    /// A DiscJuggler image.
    Cdi,
    /// An Alcohol 120% image's descriptor.
    MdsMdf,
    /// A Nero image.
    Nrg,
    /// A CloneCD image's control file.
    CloneCd,
    /// An ISO 9660 image of cooked 2048-byte sectors, which keeps none of a raw sector's headers.
    PlainIso,
    /// Content none of these is known by.
    Unknown,
}

impl Container {
    /// The container's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            Container::N64Rom => "n64Rom",
            Container::CueSheet => "cueSheet",
            Container::Gdi => "gdi",
            Container::GameCubeIso => "gameCubeIso",
            Container::GameCubeCiso => "gameCubeCiso",
            Container::Zip => "zip",
            Container::SevenZip => "sevenZip",
            Container::Chd => "chd",
            Container::Cdi => "cdi",
            Container::MdsMdf => "mdsMdf",
            Container::Nrg => "nrg",
            Container::CloneCd => "cloneCd",
            Container::PlainIso => "plainIso",
            Container::Unknown => "unknown",
        }
    }
}

/// The region of the world a dump was made for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Region {
    /// Japan.
    Japan,
    /// North America.
    NorthAmerica,
    /// Europe.
    Europe,
}

/// Why a dump is refused: its container, or its identity against the values it is held to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DumpRefusal {
    /// It is a dump of another game.
    OtherGame,
    /// It is a dump of the game, but of another region or revision than the one held to.
    OtherRevision,
    /// It is the cartridge held to by its header, but its bytes are not the ones held to.
    HashMismatch,
    /// It comes in a container the launcher does not read.
    UnsupportedContainer,
    /// It is a DiscJuggler image, a copy written for a CD-R, which cannot keep a GD-ROM's
    /// high-density area where the disc keeps it.
    Cdi,
    /// It is an archive, to unpack first.
    Archive,
    /// It is nothing the launcher knows.
    Unrecognized,
}

impl DumpRefusal {
    /// The refusal's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            DumpRefusal::OtherGame => "otherGame",
            DumpRefusal::OtherRevision => "otherRevision",
            DumpRefusal::HashMismatch => "hashMismatch",
            DumpRefusal::UnsupportedContainer => "unsupportedContainer",
            DumpRefusal::Cdi => "cdi",
            DumpRefusal::Archive => "archive",
            DumpRefusal::Unrecognized => "unrecognized",
        }
    }
}

/// How a dump stands against the values it is held to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// It is the dump held to.
    Supported,
    /// It is refused, for the reason given.
    Refused(DumpRefusal),
    /// It is for a console whose dumps are judged when they are read to install from, not here.
    Unchecked,
}

/// The fields of a Dreamcast disc's boot area that tell the disc apart from other releases of the
/// game, each exactly as the boot area keeps it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DiscIdentity {
    /// The product's number, padded with spaces.
    pub product: [u8; 10],
    /// The product's version.
    pub version: [u8; 6],
    /// The areas of the world the disc was made for, a letter each in its own place.
    pub area: [u8; 8],
}

/// The US disc, which the Dreamcast Legends expansion is extracted from: product T9710N, version
/// 1.003, made for North America alone.
pub const US_DISC: DiscIdentity = DiscIdentity {
    product: *b"T9710N    ",
    version: *b"V1.003",
    area: *b" U      ",
};

/// A track a disc's sheet lists, and the file beside the sheet that holds it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TrackFile {
    /// The file's name, as the sheet spells it.
    pub name: String,
    /// What the track's sectors hold.
    pub mode: TrackMode,
    /// How many bytes the file holds, or `None` when no regular file is there.
    pub size: Option<u64>,
}

/// What identification read of a dump, which a record of where an expansion came from keeps.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Source {
    /// An N64 ROM, as the N64 extractor's manifest records the dump it read.
    N64 {
        /// The dump's file name as the system gave it.
        name: SourceName,
        /// How many bytes the dump holds.
        size: u64,
        /// The byte order the dump keeps the cartridge's words in.
        byte_order: ByteOrder,
        /// The SHA-256 digest of the dump as it was given.
        sha256: Sha256Digest,
        /// The SHA-1 digest of the ROM in the big-endian byte order, the cartridge's own.
        canonical_sha1: Sha1Digest,
        /// The SHA-256 digest of the ROM in that order.
        canonical_sha256: Sha256Digest,
    },
    /// A Dreamcast disc, by its sheet and the files beside it that the sheet names.
    Dreamcast {
        /// Where the sheet lies, as it was given or found.
        sheet: PathBuf,
        /// The sheet's file name as the system gave it.
        sheet_name: SourceName,
        /// What kind of sheet it is.
        kind: SheetKind,
        /// Every track the sheet lists, in its order.
        tracks: Vec<TrackFile>,
    },
    /// Nothing past the marks of its container.
    Unread,
}

/// What a dump is, as its content says.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Identity {
    /// The console it is for, when its content tells.
    pub console: Option<Console>,
    /// The container it comes in.
    pub container: Container,
    /// The title it gives itself.
    pub title: Option<String>,
    /// Its serial: an N64 cartridge's game code, or a disc's product number.
    pub serial: Option<String>,
    /// Its revision: an N64 ROM's version, or a disc's.
    pub revision: Option<String>,
    /// The region of the world it was made for, when it names exactly one.
    pub region: Option<Region>,
    /// How it stands against the dumps the launcher installs the expansions from.
    pub verdict: Verdict,
    /// What was read of it.
    pub source: Source,
}

/// What the dump at `path` is, told from its content, never from its name. A folder is the disc
/// whose one sheet [`find_sheet`] finds in it, whatever the folder's path ends in, a drive's root
/// or `..` included, since the disc is known by its sheet's name. A file is known by the first of
/// these its content meets: an N64 ROM's first word, in any byte order, when the ROM is read whole
/// and judged against the US cartridge; the marks of a GameCube disc image, whole or sparse, which
/// is judged when it is read to install from; the marks of an archive, a CHD, an Alcohol 120%
/// descriptor, a DiscJuggler image, a Nero image or a CloneCD control file, each refused; a
/// sheet's text, the readers' refusal of it included; a sheet beside it that names it as a track's
/// file; and an ISO 9660 image's volume descriptor in cooked sectors, refused. Anything else is
/// unrecognized. A disc is read as the Dreamcast extractor reads its head, and judged by its boot
/// area against the US disc, a boot area of another game telling only that. The verdict describes
/// the dump and never decides an extraction, which holds the dump to values it is given.
///
/// A path that is no folder and names no file is refused before anything is read, and so is one
/// where what is there is neither a folder nor a regular file. Fails when the dump cannot be read,
/// or when the ROM or the disc breaks a rule of its format. Identification reads no more than a
/// file's first [`MAX_SHEET_LEN`] bytes and its last 16, the small files beside it that may be
/// sheets, a disc's sheet, its data tracks' first sectors and its boot area, and a ROM whole; it
/// only ever reads.
pub fn identify(path: &Path) -> Result<Identity, MediaError> {
    let metadata = fs::metadata(path).map_err(|error| MediaError::io(path, error))?;
    if metadata.is_dir() {
        return found_sheet(&find_sheet(path)?);
    }
    // A file's name goes into what is recorded of the dump, so a path that names none is refused
    // before the file is read.
    let name = SourceName::of(path).ok_or_else(|| MediaError::names_no_file(path))?;
    let ends = Ends::read(path)?;
    let first = ends.head.first_chunk().copied();
    if first.and_then(ByteOrder::of_first_word).is_some() {
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        return Ok(rom_identity(&N64Rom::open(path, &job)?));
    }
    if let Some((console, container, verdict)) = ends.marked() {
        return Ok(unread(console, container, verdict));
    }
    if ends.len <= MAX_SHEET_LEN
        && let Some(read) = sheet_of(&ends.head)
    {
        let (kind, tracks) = read?;
        let sheet = Sheet {
            kind,
            path: path.to_path_buf(),
            tracks,
        };
        return disc_identity(sheet, name);
    }
    if let Some(sheet) = sheet_naming(path)? {
        return found_sheet(&sheet);
    }
    let plain_iso = ends
        .head
        .get(PLAIN_ISO_MARK_AT..)
        .is_some_and(|rest| rest.starts_with(PLAIN_ISO_MARK));
    Ok(if plain_iso {
        unread(
            None,
            Container::PlainIso,
            Verdict::Refused(DumpRefusal::UnsupportedContainer),
        )
    } else {
        unread(
            None,
            Container::Unknown,
            Verdict::Refused(DumpRefusal::Unrecognized),
        )
    })
}

/// How the N64 ROM whose header is `header` and whose bytes in the big-endian byte order have the
/// SHA-1 digest `canonical_sha1` stands against the cartridge whose digest is `trusted`: the
/// cartridge itself when the digests agree; otherwise, by the header's game code, the US
/// cartridge's code on other bytes, a code of another region or revision of the game, or a code of
/// another game.
pub fn n64_verdict(header: &RomHeader, canonical_sha1: Sha1Digest, trusted: Sha1Digest) -> Verdict {
    if canonical_sha1 == trusted {
        return Verdict::Supported;
    }
    let refusal = if header.game_code == US_GAME_CODE {
        DumpRefusal::HashMismatch
    } else if GAME_CODE_STARTS
        .iter()
        .any(|start| header.game_code.starts_with(start))
    {
        DumpRefusal::OtherRevision
    } else {
        DumpRefusal::OtherGame
    };
    Verdict::Refused(refusal)
}

/// How the Dreamcast disc whose boot area is `boot` stands against `trusted`, the identity of the
/// disc it is held to: another game unless the boot area is Gauntlet Legends' on the Dreamcast,
/// its hardware ID first and its title beginning with the game's name; another revision unless its
/// product, version and areas are exactly the trusted ones; and the disc itself otherwise.
pub fn disc_verdict(boot: &IpBin, trusted: &DiscIdentity) -> Verdict {
    if !names_gauntlet_legends(boot.bytes()) {
        return Verdict::Refused(DumpRefusal::OtherGame);
    }
    let held = boot.field(PRODUCT) == trusted.product
        && boot.field(VERSION) == trusted.version
        && boot.field(AREA) == trusted.area;
    if held {
        Verdict::Supported
    } else {
        Verdict::Refused(DumpRefusal::OtherRevision)
    }
}

/// The bytes at either end of a file, which most containers are known by, and its length.
struct Ends {
    /// Its first [`MAX_SHEET_LEN`] bytes, or every byte of a shorter file: the whole of a sheet.
    head: Vec<u8>,
    /// Its last [`TAIL_LEN`] bytes, or every byte of a shorter file.
    tail: Vec<u8>,
    /// Its length, as the system gave it when the file was opened.
    len: u64,
}

impl Ends {
    /// The ends of the regular file at `path`, opened only once seen to be one, so that no FIFO
    /// or device is opened.
    fn read(path: &Path) -> Result<Ends, MediaError> {
        let failed = |error| MediaError::io(path, error);
        let (mut file, len) = open_regular(path)
            .map_err(failed)?
            .ok_or_else(|| MediaError::names_no_regular_file(path))?;
        let room = usize::try_from(len.min(MAX_SHEET_LEN)).unwrap_or_default();
        let mut head = Vec::with_capacity(room);
        file.by_ref()
            .take(MAX_SHEET_LEN)
            .read_to_end(&mut head)
            .map_err(failed)?;
        let tail_len = len.min(TAIL_LEN);
        file.seek(SeekFrom::Start(len - tail_len)).map_err(failed)?;
        let mut tail = Vec::new();
        file.take(tail_len).read_to_end(&mut tail).map_err(failed)?;
        Ok(Ends { head, tail, len })
    }

    /// The console, container and verdict of a file its container's marks tell, in the order the
    /// marks are looked for, or `None` when none is there.
    fn marked(&self) -> Option<(Option<Console>, Container, Verdict)> {
        let head = self.head.as_slice();
        let refused = Verdict::Refused;
        let marked = if head.starts_with(CISO_MARK) {
            (
                Some(Console::GameCube),
                Container::GameCubeCiso,
                Verdict::Unchecked,
            )
        } else if head
            .get(GAMECUBE_MARK_AT..)
            .is_some_and(|rest| rest.starts_with(GAMECUBE_MARK))
        {
            (
                Some(Console::GameCube),
                Container::GameCubeIso,
                Verdict::Unchecked,
            )
        } else if head.starts_with(ZIP_MARK) {
            (None, Container::Zip, refused(DumpRefusal::Archive))
        } else if head.starts_with(SEVEN_ZIP_MARK) {
            (None, Container::SevenZip, refused(DumpRefusal::Archive))
        } else if head.starts_with(CHD_MARK) {
            let refusal = refused(DumpRefusal::UnsupportedContainer);
            (None, Container::Chd, refusal)
        } else if head.starts_with(MDS_MARK) {
            let refusal = refused(DumpRefusal::UnsupportedContainer);
            (None, Container::MdsMdf, refusal)
        } else if self
            .word_before_end(CDI_VERSION_FROM_END)
            .is_some_and(|version| CDI_VERSIONS.contains(&u32::from_le_bytes(version)))
        {
            (
                Some(Console::Dreamcast),
                Container::Cdi,
                refused(DumpRefusal::Cdi),
            )
        } else if self.word_before_end(NRG_MARK_FROM_END) == Some(NRG_MARK)
            || self.word_before_end(NRG_FIRST_MARK_FROM_END) == Some(NRG_FIRST_MARK)
        {
            let refusal = refused(DumpRefusal::UnsupportedContainer);
            (None, Container::Nrg, refusal)
        } else if self.len <= MAX_SHEET_LEN && without_mark(head).starts_with(CLONE_CD_MARK) {
            let refusal = refused(DumpRefusal::UnsupportedContainer);
            (None, Container::CloneCd, refusal)
        } else {
            return None;
        };
        Some(marked)
    }

    /// The four bytes from `back` bytes before the file's end, when it holds that many.
    fn word_before_end(&self, back: usize) -> Option<[u8; 4]> {
        let at = self.tail.len().checked_sub(back)?;
        self.tail[at..].first_chunk().copied()
    }
}

/// A dump known by its container's marks alone: for `console`, in `container`, standing as
/// `verdict` says, with nothing more read of it.
fn unread(console: Option<Console>, container: Container, verdict: Verdict) -> Identity {
    Identity {
        console,
        container,
        title: None,
        serial: None,
        revision: None,
        region: None,
        verdict,
        source: Source::Unread,
    }
}

/// What `rom` is: an N64 ROM, the title, game code, version and region its header gives, judged
/// against the US cartridge, with what the N64 extractor records of the dump it was read from.
fn rom_identity(rom: &N64Rom) -> Identity {
    let header = rom.header();
    let canonical_sha1 = rom.canonical_sha1();
    // A game code is letters and digits; any other byte would show as no character of the code.
    let serial = header
        .game_code
        .iter()
        .all(u8::is_ascii_graphic)
        .then(|| header.game_code_text());
    Identity {
        console: Some(Console::Nintendo64),
        container: Container::N64Rom,
        title: shown(&header.title),
        serial,
        revision: Some(header.version.to_string()),
        region: header.region(),
        verdict: n64_verdict(&header, canonical_sha1, US_SHA1),
        source: Source::N64 {
            name: rom.source_name().clone(),
            size: rom.source_len(),
            byte_order: rom.byte_order(),
            sha256: rom.source_sha256(),
            canonical_sha1,
            canonical_sha256: rom.canonical_sha256(),
        },
    }
}

/// What the disc whose sheet was found at `sheet` is, the sheet read whole.
fn found_sheet(sheet: &Path) -> Result<Identity, MediaError> {
    let name = SourceName::of(sheet).ok_or_else(|| MediaError::names_no_file(sheet))?;
    disc_identity(read_sheet(sheet)?, name)
}

/// What the disc `sheet` lists is, the sheet's file named `sheet_name`: its head read as the
/// Dreamcast extractor reads it, then the title, product, version and region its boot area gives,
/// and the boot area judged against the US disc's. A boot area of another game tells that alone.
fn disc_identity(sheet: Sheet, sheet_name: SourceName) -> Result<Identity, MediaError> {
    let container = match sheet.kind {
        SheetKind::Cue => Container::CueSheet,
        SheetKind::Gdi => Container::Gdi,
    };
    let source = Source::Dreamcast {
        sheet: sheet.path.clone(),
        sheet_name,
        kind: sheet.kind,
        tracks: track_files(&sheet)?,
    };
    let other_game = Identity {
        console: Some(Console::Dreamcast),
        container,
        title: None,
        serial: None,
        revision: None,
        region: None,
        verdict: Verdict::Refused(DumpRefusal::OtherGame),
        source,
    };
    let head = match head_of(sheet, &SCRIPT_RULES) {
        Ok(head) => head,
        Err(MediaError::Disc {
            rule: DiscRule::NotGauntletLegends,
            ..
        }) => return Ok(other_game),
        Err(error) => return Err(error),
    };
    let boot = &head.boot;
    Ok(Identity {
        title: shown(boot.title()),
        serial: shown(boot.product()),
        revision: ascii_field(boot.field(VERSION)).as_deref().and_then(shown),
        region: area_region(boot.field(AREA)),
        verdict: disc_verdict(boot, &US_DISC),
        ..other_game
    })
}

/// Every track `sheet` lists, in its order, with the length of the regular file beside the sheet
/// that holds it, or none when no regular file is there. Fails when the system cannot say what is
/// there.
fn track_files(sheet: &Sheet) -> Result<Vec<TrackFile>, MediaError> {
    let folder = sheet.path.parent().unwrap_or(Path::new(""));
    let mut files = Vec::with_capacity(sheet.tracks.len());
    for track in &sheet.tracks {
        let path = folder.join(&track.file);
        let size = match fs::metadata(&path) {
            Ok(metadata) => metadata.is_file().then_some(metadata.len()),
            Err(error) if error.kind() == io::ErrorKind::NotFound => None,
            Err(error) => return Err(MediaError::io(&path, error)),
        };
        files.push(TrackFile {
            name: track.file.clone(),
            mode: track.mode,
            size,
        });
    }
    Ok(files)
}

/// `text`, a field a dump gives of itself, without the spaces, other ASCII white space and NULs
/// that pad it, or `None` when nothing else is left.
fn shown(text: &str) -> Option<String> {
    let padding = |character: char| character == '\0' || character.is_ascii_whitespace();
    let kept = text.trim_matches(padding);
    (!kept.is_empty()).then(|| kept.to_owned())
}

/// The one region `area`, a boot area's field of the areas a disc was made for, names by its
/// letter in its own place, or `None` when it names none or several.
fn area_region(area: &[u8]) -> Option<Region> {
    let mut named = AREA_SYMBOLS
        .iter()
        .zip(area)
        .filter(|((symbol, _), held)| symbol == *held)
        .map(|((_, region), _)| *region);
    let region = named.next()?;
    named.next().is_none().then_some(region)
}

#[cfg(test)]
mod tests {
    use std::fs::{self, File};
    use std::io::ErrorKind;
    use std::ops::Range;
    use std::path::{Path, PathBuf};

    use gdl_files::{Cancel, Job, NoProgress};
    use gdl_fixtures::{DiscSet, Edc, SparseFile, raw_sector, reorder, snapshot};
    use serde_json::Value;

    use super::{
        Console, Container, DiscIdentity, DumpRefusal, Identity, Region, Source, TrackFile,
        US_DISC, Verdict, disc_verdict, identify, n64_verdict,
    };
    use crate::vectors::{
        dreamcast_vectors, frozen, n64_vectors, named, players_disc, players_rom,
    };
    use crate::{
        ByteOrder, GdRom, IpBin, MAX_ROM_LEN, MediaError, RomHeader, SCRIPT_RULES, Sha1Digest,
        Sha256Digest, SheetKind, TrackMode, US_SHA1, parse_sheet, read_head,
    };

    /// How many bytes a raw sector holds. The format's facts are given here apart from the
    /// readers' own constants, so that a reader that looks elsewhere is caught.
    const FORMAT_RAW_SECTOR: usize = 2352;

    /// Where a raw sector keeps its user data, after its sync and header.
    const FORMAT_USER_DATA_AT: usize = 16;

    /// How many bytes of user data a sector holds.
    const FORMAT_SECTOR: usize = 2048;

    /// How many sectors a boot area takes.
    const FORMAT_BOOT_SECTORS: usize = 16;

    /// Where a boot area keeps its hardware ID.
    const FORMAT_HARDWARE_ID: Range<usize> = 0..16;

    /// Where it keeps the areas of the world the disc was made for.
    const FORMAT_AREA: Range<usize> = 48..56;

    /// Where it keeps the product's number.
    const FORMAT_PRODUCT: Range<usize> = 64..74;

    /// Where it keeps the product's version.
    const FORMAT_VERSION: Range<usize> = 74..80;

    /// Where it keeps the title.
    const FORMAT_TITLE: Range<usize> = 128..256;

    /// Where an N64 ROM's header keeps the title.
    const FORMAT_ROM_TITLE: Range<usize> = 0x20..0x34;

    /// Where it keeps the game code.
    const FORMAT_GAME_CODE: Range<usize> = 0x3B..0x3F;

    /// Where it keeps the version.
    const FORMAT_ROM_VERSION: usize = 0x3F;

    /// The sectors of the full set's first data track.
    const FULL_FIRST_SECTORS: usize = 22;

    /// The sectors the full set's second data track's file keeps before the track's first.
    const FULL_PREGAP_SECTORS: usize = 225;

    /// The LBA of that first sector.
    const FULL_SECOND_LBA: u32 = 45_100;

    /// What a cue sheet that keeps a whole disc in one file says: its second track lies in the
    /// file its first was read from.
    const ONE_FILE_DISC: &str = concat!(
        "FILE \"disc.bin\" BINARY\n",
        "TRACK 01 MODE1/2352\n",
        "INDEX 01 00:00:00\n",
        "TRACK 02 AUDIO\n",
        "INDEX 01 03:00:00\n",
    );

    /// What the launcher answers for a folder given as a dump that holds no sheet.
    const NO_SHEET_IN_FOLDER: &str = "No sheet in the folder lists a disc's tracks";

    /// What `path` is identified as, which must be told.
    fn identified(path: &Path) -> Identity {
        identify(path).unwrap_or_else(|error| panic!("{}: {error:?}", path.display()))
    }

    /// The rule a refusal of a dump's format names, and the words it says.
    fn refusal(outcome: Result<Identity, MediaError>) -> (&'static str, String) {
        let error = outcome.unwrap_err();
        assert!(
            matches!(error, MediaError::Rom { .. } | MediaError::Disc { .. }),
            "{error:?}"
        );
        (error.rule(), error.to_string())
    }

    /// A dump known by its container's marks alone.
    fn marked(console: Option<Console>, container: Container, verdict: Verdict) -> Identity {
        Identity {
            console,
            container,
            title: None,
            serial: None,
            revision: None,
            region: None,
            verdict,
            source: Source::Unread,
        }
    }

    /// Content the launcher does not know.
    fn unrecognized() -> Identity {
        marked(
            None,
            Container::Unknown,
            Verdict::Refused(DumpRefusal::Unrecognized),
        )
    }

    /// `bytes` written into `dir` as the file `name`, and its path.
    fn file_in(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf {
        let path = dir.join(name);
        fs::write(&path, bytes).unwrap();
        path
    }

    /// The track of a sheet held in the file `name`, of `mode`, `size` bytes long when it is
    /// there.
    fn track(name: &str, mode: TrackMode, size: Option<u64>) -> TrackFile {
        TrackFile {
            name: name.to_owned(),
            mode,
            size,
        }
    }

    /// How many bytes the shared vectors give the file `name` of the set `set`.
    fn size_in(vectors: &Value, set: &str, name: &str) -> u64 {
        vectors["sets"][set]["files"][name]["size"]
            .as_u64()
            .unwrap()
    }

    /// The full set's tracks behind its cue sheet, its audio track's file `audio` bytes long when
    /// it is there.
    fn full_tracks(vectors: &Value, audio: Option<u64>) -> Vec<TrackFile> {
        vec![
            track(
                "track3.bin",
                TrackMode::Mode1Raw,
                Some(size_in(vectors, "full", "track3.bin")),
            ),
            track("track4.bin", TrackMode::Audio, audio),
            track(
                "track5.bin",
                TrackMode::Mode1Raw,
                Some(size_in(vectors, "full", "track5.bin")),
            ),
        ]
    }

    /// The disc whose sheet of `kind` lies at `sheet` and lists `tracks`, when it is the US disc
    /// of the game.
    fn us_disc(sheet: &Path, kind: SheetKind, tracks: Vec<TrackFile>) -> Identity {
        let container = match kind {
            SheetKind::Cue => Container::CueSheet,
            SheetKind::Gdi => Container::Gdi,
        };
        let name = sheet.file_name().unwrap().to_str().unwrap();
        Identity {
            console: Some(Console::Dreamcast),
            container,
            title: Some("GAUNTLET LEGENDS".to_owned()),
            serial: Some("T9710N".to_owned()),
            revision: Some("V1.003".to_owned()),
            region: Some(Region::NorthAmerica),
            verdict: Verdict::Supported,
            source: Source::Dreamcast {
                sheet: sheet.to_path_buf(),
                sheet_name: named(name),
                kind,
                tracks,
            },
        }
    }

    /// The file of `set` named `name`.
    fn file_of<'a>(set: &'a mut DiscSet, name: &str) -> &'a mut SparseFile {
        set.files
            .iter_mut()
            .find(|(held, _)| held == name)
            .map(|(_, file)| file)
            .unwrap_or_else(|| panic!("the set holds no file {name}"))
    }

    /// `set`, a cue set, with `bytes` written over its boot area from `at`, a place in the area's
    /// user data within one sector, as a chunk of its first data track's file written after the
    /// set's own.
    fn with_boot_bytes(mut set: DiscSet, at: usize, bytes: &[u8]) -> DiscSet {
        let (sector, within) = (at / FORMAT_SECTOR, at % FORMAT_SECTOR);
        assert!(within + bytes.len() <= FORMAT_SECTOR);
        let place = sector * FORMAT_RAW_SECTOR + FORMAT_USER_DATA_AT + within;
        let track = file_of(&mut set, "track3.bin");
        track.chunks.push((place as u64, bytes.to_vec()));
        set
    }

    /// The boot area of the disc `set` holds, read with its head.
    fn boot_of(set: &DiscSet) -> IpBin {
        let dir = tempfile::tempdir().unwrap();
        let sheet = set.write(dir.path()).unwrap();
        read_head(&sheet, &SCRIPT_RULES).unwrap().boot
    }

    /// `boot` with `bytes` written over it from `at`.
    fn with_field(boot: &IpBin, at: usize, bytes: &[u8]) -> IpBin {
        let mut changed = boot.bytes().to_vec();
        changed[at..at + bytes.len()].copy_from_slice(bytes);
        IpBin::decode(changed).unwrap()
    }

    /// The default N64 image with `title`, `game_code` and `version` written where the format
    /// places them.
    fn rom_with_header(title: &[u8; 20], game_code: &[u8; 4], version: u8) -> Vec<u8> {
        let mut image = frozen(&n64_vectors(), "default");
        image[FORMAT_ROM_TITLE].copy_from_slice(title);
        image[FORMAT_GAME_CODE].copy_from_slice(game_code);
        image[FORMAT_ROM_VERSION] = version;
        image
    }

    /// A header that gives the game code `game_code`.
    fn header_coded(game_code: &[u8; 4]) -> RomHeader {
        RomHeader {
            title: "GAUNTLET LEGENDS".to_owned(),
            game_code: *game_code,
            version: 0,
        }
    }

    #[test]
    fn roms_identify_by_their_first_word_whatever_their_name() {
        let default = frozen(&n64_vectors(), "default");
        let canonical_sha1 = Sha1Digest::of(&default);
        let canonical_sha256 = Sha256Digest::of(&default);
        let dir = tempfile::tempdir().unwrap();
        // The default image in each of the three byte orders, under names no ROM is known by. Its
        // header is zeros but for the first word: no title, version 0, and a game code of four
        // NULs, which is no serial, names no region and is no Legends cartridge's.
        let orders = [
            (default.clone(), ByteOrder::BigEndian),
            (reorder(&default, "pairs"), ByteOrder::ByteSwapped),
            (reorder(&default, "words"), ByteOrder::LittleEndian),
        ];
        for (bytes, byte_order) in orders {
            for name in ["dump.bin", "notes.txt", "disc.cue"] {
                let path = file_in(dir.path(), name, &bytes);
                let expected = Identity {
                    console: Some(Console::Nintendo64),
                    container: Container::N64Rom,
                    title: None,
                    serial: None,
                    revision: Some("0".to_owned()),
                    region: None,
                    verdict: Verdict::Refused(DumpRefusal::OtherGame),
                    source: Source::N64 {
                        name: named(name),
                        size: bytes.len() as u64,
                        byte_order,
                        sha256: Sha256Digest::of(&bytes),
                        canonical_sha1,
                        canonical_sha256,
                    },
                };
                assert_eq!(identified(&path), expected, "{name} {byte_order:?}");
                fs::remove_file(&path).unwrap();
            }
        }

        // A header that names the game, read in the cartridge's own order whatever order the dump
        // keeps: its title, game code, version and region.
        let image = rom_with_header(b"GAUNTLET LEGENDS    ", b"NGXE", 1);
        for (transform, bytes) in [
            ("none", image.clone()),
            ("pairs", reorder(&image, "pairs")),
            ("words", reorder(&image, "words")),
        ] {
            let path = file_in(dir.path(), "cartridge.rom", &bytes);
            let identity = identified(&path);
            assert_eq!(identity.title.as_deref(), Some("GAUNTLET LEGENDS"));
            assert_eq!(identity.serial.as_deref(), Some("NGXE"));
            assert_eq!(identity.revision.as_deref(), Some("1"), "{transform}");
            assert_eq!(identity.region, Some(Region::NorthAmerica));
            // The US cartridge's code on bytes that are not the US cartridge's.
            let verdict = Verdict::Refused(DumpRefusal::HashMismatch);
            assert_eq!(identity.verdict, verdict, "{transform}");
            fs::remove_file(&path).unwrap();
        }
        // A game code that holds a byte no code holds is no serial.
        let path = file_in(
            dir.path(),
            "coded.z64",
            &rom_with_header(b"GAUNTLET LEGENDS    ", b"NGX\0", 0),
        );
        assert_eq!(identified(&path).serial, None);
    }

    #[test]
    fn n64_verdicts_follow_the_header() {
        let other = Sha1Digest::of(b"another cartridge");
        // Against the US cartridge's digest: the US code on other bytes, another region's or
        // revision's code of the game, and another game's.
        let cases = [
            (b"NGXE", DumpRefusal::HashMismatch),
            (b"NGXP", DumpRefusal::OtherRevision),
            (b"NGDJ", DumpRefusal::OtherRevision),
            (b"NGXJ", DumpRefusal::OtherRevision),
            (b"NGDE", DumpRefusal::OtherRevision),
            (b"NABC", DumpRefusal::OtherGame),
            (b"CGXE", DumpRefusal::OtherGame),
            (b"\0\0\0\0", DumpRefusal::OtherGame),
        ];
        for (game_code, refusal) in cases {
            let verdict = n64_verdict(&header_coded(game_code), other, US_SHA1);
            assert_eq!(verdict, Verdict::Refused(refusal), "{game_code:?}");
            // A digest equal to the one held to is the cartridge, whatever its header says.
            let held = n64_verdict(&header_coded(game_code), other, other);
            assert_eq!(held, Verdict::Supported, "{game_code:?}");
        }
        assert_eq!(
            n64_verdict(&header_coded(b"NGXE"), US_SHA1, US_SHA1),
            Verdict::Supported
        );

        // Identification judges a ROM against the US cartridge's digest.
        let dir = tempfile::tempdir().unwrap();
        for (game_code, refusal) in [
            (b"NGXP", DumpRefusal::OtherRevision),
            (b"NGDJ", DumpRefusal::OtherRevision),
            (b"NABC", DumpRefusal::OtherGame),
        ] {
            let bytes = rom_with_header(b"GAUNTLET LEGENDS    ", game_code, 0);
            let path = file_in(dir.path(), "dump.z64", &bytes);
            let identity = identified(&path);
            assert_eq!(identity.verdict, Verdict::Refused(refusal), "{game_code:?}");
            let code = str::from_utf8(game_code).unwrap();
            assert_eq!(identity.serial.as_deref(), Some(code));
            fs::remove_file(&path).unwrap();
        }
        // The regions the codes name.
        let regions = [
            (b"NGXP", Some(Region::Europe)),
            (b"NGDJ", Some(Region::Japan)),
            (b"NGXE", Some(Region::NorthAmerica)),
            (b"NGXU", None),
        ];
        for (game_code, region) in regions {
            let bytes = rom_with_header(b"GAUNTLET LEGENDS    ", game_code, 0);
            let path = file_in(dir.path(), "dump.z64", &bytes);
            assert_eq!(identified(&path).region, region, "{game_code:?}");
            fs::remove_file(&path).unwrap();
        }
    }

    #[test]
    fn containers_are_refused_by_content() {
        let dir = tempfile::tempdir().unwrap();
        let refused = Verdict::Refused;
        let unsupported = Verdict::Refused(DumpRefusal::UnsupportedContainer);
        // A footer of a DiscJuggler image of `version`: its version, then where its header lies.
        let cdi = |version: u32| {
            let mut bytes = vec![0x55; 4096];
            bytes.extend_from_slice(&version.to_le_bytes());
            bytes.extend_from_slice(&4000_u32.to_le_bytes());
            bytes
        };
        // A Nero image ending with `mark` and the place of its chunks, `place` bytes wide.
        let nrg = |mark: &[u8], place: usize| {
            let mut bytes = vec![0x55; 4096];
            bytes.extend_from_slice(mark);
            bytes.extend(std::iter::repeat_n(0, place - 1));
            bytes.push(0x10);
            bytes
        };
        let mut plain_iso = vec![0; 17 * FORMAT_SECTOR];
        plain_iso[16 * FORMAT_SECTOR..16 * FORMAT_SECTOR + 7].copy_from_slice(b"\x01CD001\x01");
        let mut gamecube = vec![0; 0x440];
        gamecube[..6].copy_from_slice(b"GUNE5D");
        gamecube[0x1C..0x20].copy_from_slice(&[0xC2, 0x33, 0x9F, 0x3D]);
        let mut ciso = b"CISO".to_vec();
        ciso.extend_from_slice(&0x8000_u32.to_le_bytes());
        ciso.resize(0x8000, 1);

        let cases = [
            (
                "zip",
                b"PK\x03\x04\x14\x00\x00\x00".to_vec(),
                marked(None, Container::Zip, refused(DumpRefusal::Archive)),
            ),
            (
                "7z",
                vec![0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0, 4],
                marked(None, Container::SevenZip, refused(DumpRefusal::Archive)),
            ),
            (
                "chd",
                b"MComprHD\x00\x00\x00\x7c\x00\x00\x00\x05".to_vec(),
                marked(None, Container::Chd, unsupported),
            ),
            (
                "mds",
                b"MEDIA DESCRIPTOR\x01\x05".to_vec(),
                marked(None, Container::MdsMdf, unsupported),
            ),
            (
                "cdi 2",
                cdi(0x8000_0004),
                marked(
                    Some(Console::Dreamcast),
                    Container::Cdi,
                    refused(DumpRefusal::Cdi),
                ),
            ),
            (
                "cdi 3",
                cdi(0x8000_0005),
                marked(
                    Some(Console::Dreamcast),
                    Container::Cdi,
                    refused(DumpRefusal::Cdi),
                ),
            ),
            (
                "cdi 3.5",
                cdi(0x8000_0006),
                marked(
                    Some(Console::Dreamcast),
                    Container::Cdi,
                    refused(DumpRefusal::Cdi),
                ),
            ),
            (
                "nrg 2",
                nrg(b"NER5", 8),
                marked(None, Container::Nrg, unsupported),
            ),
            (
                "nrg 1",
                nrg(b"NERO", 4),
                marked(None, Container::Nrg, unsupported),
            ),
            (
                "ccd",
                b"[CloneCD]\r\nVersion=3\r\n".to_vec(),
                marked(None, Container::CloneCd, unsupported),
            ),
            (
                "ccd after a byte order mark",
                b"\xef\xbb\xbf[CloneCD]\r\nVersion=3\r\n".to_vec(),
                marked(None, Container::CloneCd, unsupported),
            ),
            (
                "plain iso",
                plain_iso.clone(),
                marked(None, Container::PlainIso, unsupported),
            ),
            (
                "gamecube iso",
                gamecube.clone(),
                marked(
                    Some(Console::GameCube),
                    Container::GameCubeIso,
                    Verdict::Unchecked,
                ),
            ),
            (
                "gamecube ciso",
                ciso.clone(),
                marked(
                    Some(Console::GameCube),
                    Container::GameCubeCiso,
                    Verdict::Unchecked,
                ),
            ),
            // The first mark looked for decides: a CISO's map that happens to hold a GameCube
            // disc's mark, an archive that ends as a DiscJuggler image does, and a plain image
            // that ends so too.
            (
                "ciso with a disc's mark",
                {
                    let mut both = ciso.clone();
                    both[0x1C..0x20].copy_from_slice(&[0xC2, 0x33, 0x9F, 0x3D]);
                    both
                },
                marked(
                    Some(Console::GameCube),
                    Container::GameCubeCiso,
                    Verdict::Unchecked,
                ),
            ),
            (
                "zip ending as a cdi",
                [b"PK\x03\x04".to_vec(), cdi(0x8000_0005)].concat(),
                marked(None, Container::Zip, refused(DumpRefusal::Archive)),
            ),
            (
                "plain iso ending as a cdi",
                [plain_iso.clone(), cdi(0x8000_0006)].concat(),
                marked(
                    Some(Console::Dreamcast),
                    Container::Cdi,
                    refused(DumpRefusal::Cdi),
                ),
            ),
            // Anything else is nothing the launcher knows: no bytes, text, zeros past the most a
            // sheet holds, a CloneCD file's mark past that length, and marks out of place.
            ("empty", Vec::new(), unrecognized()),
            ("text", b"Gauntlet Legends (USA)\n".to_vec(), unrecognized()),
            ("zeros", vec![0; 100_000], unrecognized()),
            (
                "long ccd",
                {
                    let mut long = b"[CloneCD]\r\n".to_vec();
                    long.resize(70_000, b' ');
                    long
                },
                unrecognized(),
            ),
            (
                "late zip",
                b" PK\x03\x04 MComprHD CISO".to_vec(),
                unrecognized(),
            ),
            (
                "tail of seven bytes",
                vec![0x04, 0, 0, 0x80, 0, 0, 0],
                unrecognized(),
            ),
        ];
        for (case, bytes, expected) in cases {
            // Each under a name a dump of the Dreamcast or the N64 might have, which says nothing.
            for name in ["dump.cue", "dump.z64"] {
                let path = file_in(dir.path(), name, &bytes);
                assert_eq!(identified(&path), expected, "{case} as {name}");
                fs::remove_file(&path).unwrap();
            }
        }
        // An N64 ROM's first word decides before any other mark.
        let mut rom = frozen(&n64_vectors(), "default");
        rom[0x1C..0x20].copy_from_slice(&[0xC2, 0x33, 0x9F, 0x3D]);
        rom.extend_from_slice(&0x8000_0004_u32.to_le_bytes());
        rom.extend_from_slice(&[0; 4]);
        let path = file_in(dir.path(), "dump.iso", &rom);
        assert_eq!(identified(&path).container, Container::N64Rom);
    }

    #[test]
    fn a_disc_identifies_from_its_sheet_folder_or_track() {
        let vectors = dreamcast_vectors();
        // The full set behind its cue sheet, its audio track's file left out: from the sheet, from
        // the folder that holds it, and from each data track's file, which the sheet names.
        let dir = tempfile::tempdir().unwrap();
        let sheet = DiscSet::full(Edc::Zero).write(dir.path()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let expected = us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, None));
        let paths = [
            sheet.clone(),
            dir.path().to_path_buf(),
            dir.path().join("track3.bin"),
            dir.path().join("track5.bin"),
        ];
        for path in paths {
            assert_eq!(identified(&path), expected, "{}", path.display());
        }
        // The dump is only ever read.
        assert_eq!(snapshot(dir.path()).unwrap(), before);

        // With its audio track's file, a sector of silence: that file is told by the sheet that
        // names it too.
        let dir = tempfile::tempdir().unwrap();
        let set = DiscSet::full(Edc::Zero).with_audio_files();
        let sheet = set.write(dir.path()).unwrap();
        let silence = Some(FORMAT_RAW_SECTOR as u64);
        let expected = us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, silence));
        for path in [sheet.clone(), dir.path().join("track4.bin")] {
            assert_eq!(identified(&path), expected, "{}", path.display());
        }

        // The full set's sectors behind a GDI, its audio track's file left out too.
        let dir = tempfile::tempdir().unwrap();
        let sheet = DiscSet::full_gdi(Edc::Zero).write(dir.path()).unwrap();
        let tracks = vec![
            track(
                "track03.bin",
                TrackMode::Mode1Raw,
                Some(size_in(&vectors, "fullGdi", "track03.bin")),
            ),
            track("track04.raw", TrackMode::Audio, None),
            track(
                "track05.bin",
                TrackMode::Mode1Raw,
                Some(size_in(&vectors, "fullGdi", "track05.bin")),
            ),
        ];
        let expected = us_disc(&sheet, SheetKind::Gdi, tracks);
        let paths = [
            sheet.clone(),
            dir.path().to_path_buf(),
            dir.path().join("track03.bin"),
        ];
        for path in paths {
            assert_eq!(identified(&path), expected, "{}", path.display());
        }

        // The small set, whose boot area names no area and no version and pads its title with
        // NULs: the game's disc, of another revision than the US disc.
        let dir = tempfile::tempdir().unwrap();
        let sheet = DiscSet::small(Edc::Zero).write(dir.path()).unwrap();
        let expected = Identity {
            console: Some(Console::Dreamcast),
            container: Container::CueSheet,
            title: Some("GAUNTLET LEGENDS".to_owned()),
            serial: Some("T9710N".to_owned()),
            revision: None,
            region: None,
            verdict: Verdict::Refused(DumpRefusal::OtherRevision),
            source: Source::Dreamcast {
                sheet: sheet.clone(),
                sheet_name: named("test.cue"),
                kind: SheetKind::Cue,
                tracks: vec![
                    track(
                        "track3.bin",
                        TrackMode::Mode1Raw,
                        Some(size_in(&vectors, "small", "track3.bin")),
                    ),
                    track(
                        "track5.bin",
                        TrackMode::Mode1Raw,
                        Some(size_in(&vectors, "small", "track5.bin")),
                    ),
                ],
            },
        };
        assert_eq!(identified(&sheet), expected);
        assert_eq!(identified(dir.path()), expected);

        // A track's file the sheet lists that is a folder is no file at all.
        let dir = tempfile::tempdir().unwrap();
        let sheet = DiscSet::full(Edc::Zero).write(dir.path()).unwrap();
        fs::create_dir(dir.path().join("track4.bin")).unwrap();
        let expected = us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, None));
        assert_eq!(identified(&sheet), expected);
    }

    #[test]
    fn a_folder_with_no_or_several_sheets_is_refused() {
        let vectors = dreamcast_vectors();
        // A folder with no sheet: an empty one, and one holding only a disc's track files, whose
        // files no sheet names and so are nothing the launcher knows.
        let empty = tempfile::tempdir().unwrap();
        let no_sheet = ("noSheet", NO_SHEET_IN_FOLDER.to_owned());
        assert_eq!(refusal(identify(empty.path())), no_sheet);
        let mut tracks_only = DiscSet::full(Edc::Zero);
        tracks_only.files.retain(|(name, _)| name != "full.cue");
        let dir = tempfile::tempdir().unwrap();
        tracks_only.write(dir.path()).unwrap();
        assert_eq!(refusal(identify(dir.path())), no_sheet);
        assert_eq!(identified(&dir.path().join("track3.bin")), unrecognized());

        // Two sheets of one disc: the folder holds several, and its track files are named by
        // both. Each sheet given is its own.
        let dir = tempfile::tempdir().unwrap();
        let sheet = DiscSet::full(Edc::Zero).write(dir.path()).unwrap();
        let copy = dir.path().join("copy.cue");
        fs::copy(&sheet, &copy).unwrap();
        let several = "2 sheets in the folder list a disc's tracks";
        let in_folder = ("severalSheets", several.to_owned());
        assert_eq!(refusal(identify(dir.path())), in_folder);
        let several = "2 sheets beside the file name it as a track's file";
        let beside = ("severalSheets", several.to_owned());
        assert_eq!(refusal(identify(&dir.path().join("track3.bin"))), beside);
        let tracks = full_tracks(&vectors, None);
        let expected = us_disc(&copy, SheetKind::Cue, tracks.clone());
        assert_eq!(identified(&copy), expected);
        assert_eq!(
            identified(&sheet).source,
            us_disc(&sheet, SheetKind::Cue, tracks).source
        );

        // Sheets of two discs in one folder: the folder is refused, but each track's file is told
        // by the one sheet that names it.
        let dir = tempfile::tempdir().unwrap();
        let cue = DiscSet::full(Edc::Zero).write(dir.path()).unwrap();
        let gdi = DiscSet::full_gdi(Edc::Zero).write(dir.path()).unwrap();
        assert_eq!(refusal(identify(dir.path())), in_folder);
        let from_cue = identified(&dir.path().join("track3.bin"));
        assert_eq!(from_cue.container, Container::CueSheet);
        assert!(matches!(&from_cue.source, Source::Dreamcast { sheet, .. } if *sheet == cue));
        let from_gdi = identified(&dir.path().join("track03.bin"));
        assert_eq!(from_gdi.container, Container::Gdi);
        assert!(matches!(&from_gdi.source, Source::Dreamcast { sheet, .. } if *sheet == gdi));
    }

    #[test]
    fn disc_verdicts_follow_ip_bin() {
        assert_eq!(
            US_DISC,
            DiscIdentity {
                product: *b"T9710N    ",
                version: *b"V1.003",
                area: *b" U      ",
            }
        );
        let full = boot_of(&DiscSet::full(Edc::Zero));
        assert_eq!(disc_verdict(&full, &US_DISC), Verdict::Supported);
        // Each field the US disc is known by, changed: another revision of the game, another
        // region's disc included, and a disc for every region.
        let revisions: [(Range<usize>, &[u8]); 6] = [
            (FORMAT_PRODUCT, b"T9710P    "),
            (FORMAT_PRODUCT, b"T9710N   1"),
            (FORMAT_VERSION, b"V1.002"),
            (FORMAT_AREA, b"  E     "),
            (FORMAT_AREA, b"JUE     "),
            (FORMAT_AREA, b" U     *"),
        ];
        for (field, bytes) in revisions {
            let changed = with_field(&full, field.start, bytes);
            let verdict = disc_verdict(&changed, &US_DISC);
            assert_eq!(
                verdict,
                Verdict::Refused(DumpRefusal::OtherRevision),
                "{bytes:?}"
            );
        }
        // Another console's hardware ID, or another game's title: another game.
        let games: [(usize, &[u8]); 3] = [
            (FORMAT_HARDWARE_ID.start, b"SEGA SEGASATURN "),
            (FORMAT_HARDWARE_ID.end - 1, b"\0"),
            (FORMAT_TITLE.start + 15, b"!"),
        ];
        for (at, bytes) in games {
            let changed = with_field(&full, at, bytes);
            let verdict = disc_verdict(&changed, &US_DISC);
            assert_eq!(
                verdict,
                Verdict::Refused(DumpRefusal::OtherGame),
                "{bytes:?}"
            );
        }
        // Held to another identity, a disc is that identity's disc, and the US disc is not.
        let small = boot_of(&DiscSet::small(Edc::Zero));
        let refused = Verdict::Refused(DumpRefusal::OtherRevision);
        assert_eq!(disc_verdict(&small, &US_DISC), refused);
        let small_disc = DiscIdentity {
            product: *b"T9710N    ",
            version: [0; 6],
            area: [0; 8],
        };
        assert_eq!(disc_verdict(&small, &small_disc), Verdict::Supported);
        assert_eq!(disc_verdict(&full, &small_disc), refused);

        // Identified from their discs: a changed field the disc's boot area keeps tells another
        // revision, its new value shown; another game's boot area, which the disc's reader
        // refuses, tells another game and nothing more.
        let dir = tempfile::tempdir().unwrap();
        let set = with_boot_bytes(DiscSet::full(Edc::Zero), FORMAT_PRODUCT.start, b"T9710P");
        let sheet = set.write(dir.path()).unwrap();
        let identity = identified(&sheet);
        assert_eq!(identity.serial.as_deref(), Some("T9710P"));
        assert_eq!(identity.verdict, refused);
        let vectors = dreamcast_vectors();
        for (at, bytes) in [
            (FORMAT_HARDWARE_ID.start, &b"SEGA SEGASATURN "[..]),
            (FORMAT_TITLE.start, &b"GAUNTLET LEGEND!"[..]),
        ] {
            let dir = tempfile::tempdir().unwrap();
            let set = with_boot_bytes(DiscSet::full(Edc::Zero), at, bytes);
            let sheet = set.write(dir.path()).unwrap();
            let expected = Identity {
                title: None,
                serial: None,
                revision: None,
                region: None,
                verdict: Verdict::Refused(DumpRefusal::OtherGame),
                ..us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, None))
            };
            for path in [sheet.clone(), dir.path().to_path_buf()] {
                assert_eq!(identified(&path), expected, "{bytes:?}");
            }
        }
        // The regions an area names, one at a time.
        let regions: [(&[u8; 8], Option<Region>); 6] = [
            (b"J       ", Some(Region::Japan)),
            (b" U      ", Some(Region::NorthAmerica)),
            (b"  E     ", Some(Region::Europe)),
            (b"JU      ", None),
            (b"        ", None),
            (b"U       ", None),
        ];
        for (area, region) in regions {
            let dir = tempfile::tempdir().unwrap();
            let set = with_boot_bytes(DiscSet::full(Edc::Zero), FORMAT_AREA.start, area);
            let identity = identified(&set.write(dir.path()).unwrap());
            assert_eq!(identity.region, region, "{area:?}");
        }
    }

    #[test]
    fn identification_reads_headers_only() {
        let vectors = dreamcast_vectors();
        // The full set with every byte of its first data track past the boot area, and every byte
        // of its second data track's file but the header of the track's first sector, 0xFF: no
        // volume descriptor, no directory and no file is left to read.
        let mut set = DiscSet::full(Edc::Zero);
        let boot_end = FORMAT_BOOT_SECTORS * FORMAT_RAW_SECTOR;
        let rest = (FULL_FIRST_SECTORS - FORMAT_BOOT_SECTORS) * FORMAT_RAW_SECTOR;
        let first = file_of(&mut set, "track3.bin");
        first.chunks.push((boot_end as u64, vec![0xFF; rest]));
        let second = file_of(&mut set, "track5.bin");
        let header_at = (FULL_PREGAP_SECTORS * FORMAT_RAW_SECTOR) as u64;
        let header = raw_sector(FULL_SECOND_LBA, &[], Edc::Zero)[..FORMAT_USER_DATA_AT].to_vec();
        let filled = vec![0xFF; usize::try_from(second.len).unwrap()];
        second.chunks = vec![(0, filled), (header_at, header)];
        let dir = tempfile::tempdir().unwrap();
        let sheet = set.write(dir.path()).unwrap();
        // It identifies as the whole set does: from its sheet, its tracks' first sectors and its
        // boot area alone.
        let expected = us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, None));
        assert_eq!(identified(&sheet), expected);
        // A reader that went on to the volume would refuse the disc.
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &NoProgress,
        };
        let opened = GdRom::open(&sheet, &SCRIPT_RULES, &job);
        assert_eq!(opened.unwrap_err().rule(), "sectorHeader");
    }

    #[test]
    fn a_sheet_the_readers_refuse_tells_the_dump_by_its_refusal() {
        // A cue sheet that keeps a whole disc in one file, a cue sheet of cooked sectors and a GDI
        // of cooked sectors are sheets the readers refuse: given themselves, as the folder that
        // holds them, or as the track's file their text names, they refuse the dump by the rule
        // they break, as reading them does.
        let sheets = [
            ("disc.cue", ONE_FILE_DISC, "disc.bin"),
            (
                "cooked.cue",
                "FILE \"t.iso\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n",
                "t.iso",
            ),
            (
                "cooked.gdi",
                "1\r\n1 0 4 2048 track01.iso 0\r\n",
                "track01.iso",
            ),
            (
                "offset.gdi",
                "1\r\n1 45000 4 2352 track01.bin 4\r\n",
                "track01.bin",
            ),
        ];
        for (name, text, track) in sheets {
            let dir = tempfile::tempdir().unwrap();
            let path = file_in(dir.path(), name, text.as_bytes());
            let track = file_in(dir.path(), track, &[0; FORMAT_RAW_SECTOR]);
            let parsed = parse_sheet(text.as_bytes()).unwrap_err();
            let expected = (parsed.rule(), parsed.to_string());
            assert_eq!(refusal(identify(&path)), expected, "{name}");
            assert_eq!(refusal(identify(dir.path())), expected, "{name}");
            assert_eq!(refusal(identify(&track)), expected, "{name}");
        }
        // Text that is no sheet at all is nothing the launcher knows.
        let dir = tempfile::tempdir().unwrap();
        for text in ["", "REM only\n", "Gauntlet Legends\n"] {
            let path = file_in(dir.path(), "dump.cue", text.as_bytes());
            assert_eq!(identified(&path), unrecognized(), "{text:?}");
        }
    }

    #[test]
    fn what_cannot_be_read_or_breaks_its_format_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        // A dump that is not there is a failure to read it, which names its path.
        let missing = dir.path().join("missing.cue");
        let error = identify(&missing).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path, source }
                if *path == missing && source.kind() == ErrorKind::NotFound),
            "{error:?}"
        );
        // A file that begins as a ROM is refused by the ROM's rules: one that does not end on a
        // whole word, and one longer than the largest cartridge, refused by its length.
        let mut short = frozen(&n64_vectors(), "default");
        short.push(0);
        let path = file_in(dir.path(), "short.z64", &short);
        assert_eq!(refusal(identify(&path)).0, "header");
        let path = file_in(dir.path(), "large.z64", &[0x80, 0x37, 0x12, 0x40]);
        let file = File::options().write(true).open(&path).unwrap();
        file.set_len(MAX_ROM_LEN + 4).unwrap();
        drop(file);
        assert_eq!(refusal(identify(&path)).0, "romTooLarge");
        // A disc is refused by the rules its head breaks, however it is given: a data track's file
        // missing, from the sheet or its folder.
        let mut set = DiscSet::full(Edc::Zero);
        set.files.retain(|(name, _)| name != "track5.bin");
        let dir = tempfile::tempdir().unwrap();
        let sheet = set.write(dir.path()).unwrap();
        let missing = (
            "missingTrack",
            "Track file is missing: \"track5.bin\"".to_owned(),
        );
        assert_eq!(refusal(identify(&sheet)), missing);
        assert_eq!(refusal(identify(dir.path())), missing);
        assert_eq!(refusal(identify(&dir.path().join("track3.bin"))), missing);
    }

    #[test]
    fn a_folder_is_known_by_its_sheet_whatever_its_path_ends_in() {
        let vectors = dreamcast_vectors();
        // The full set in a folder of its own, beside an empty one: the folder given by a path
        // that ends in `..`, and so names no file, as a drive's root does, is the disc all the
        // same, its sheet found there and named by its own name.
        let dir = tempfile::tempdir().unwrap();
        let set = dir.path().join("set");
        fs::create_dir_all(set.join("empty")).unwrap();
        DiscSet::full(Edc::Zero).write(&set).unwrap();
        let unnamed = set.join("empty").join("..");
        assert_eq!(unnamed.file_name(), None);
        let sheet = unnamed.join("full.cue");
        let expected = us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, None));
        assert_eq!(identified(&unnamed), expected);
    }

    #[test]
    fn a_relative_path_is_read_beside_what_it_names() {
        // The tests run in the crate's folder, which holds its manifest and no sheet: a relative
        // path that names no folder before its file is looked for there.
        assert_eq!(identified(Path::new("Cargo.toml")), unrecognized());
    }

    #[cfg(unix)]
    #[test]
    fn a_fifo_is_refused_without_waiting_for_a_writer() {
        use crate::dump_file::fifo;

        let dir = tempfile::tempdir().unwrap();
        let sheet = DiscSet::full(Edc::Zero).write(dir.path()).unwrap();
        let path = dir.path().join("dump.bin");
        fifo::make(&path);
        // Given as the dump, it is refused before it is opened.
        let reading = path.clone();
        let error = fifo::without_waiting(&path, move || identify(&reading)).unwrap_err();
        assert!(
            matches!(&error, MediaError::Io { path: at, source }
                if *at == path
                    && source.kind() == ErrorKind::InvalidInput
                    && source.to_string() == "the path names no regular file"),
            "{error:?}"
        );
        // Beside the disc, it is passed over as the disc is identified from its folder and from a
        // track's file.
        let folder = dir.path().to_path_buf();
        let (from_folder, from_track) = fifo::without_waiting(&path, move || {
            (identify(&folder), identify(&folder.join("track3.bin")))
        });
        assert_eq!(from_folder.unwrap().source, from_track.unwrap().source);
        let vectors = dreamcast_vectors();
        let expected = us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, None));
        assert_eq!(identify(&sheet).unwrap(), expected);
    }

    #[test]
    fn every_name_is_stable() {
        let consoles = [
            (Console::GameCube, "gamecube"),
            (Console::Nintendo64, "n64"),
            (Console::Dreamcast, "dreamcast"),
        ];
        for (console, name) in consoles {
            assert_eq!(console.as_str(), name);
        }
        let containers = [
            (Container::N64Rom, "n64Rom"),
            (Container::CueSheet, "cueSheet"),
            (Container::Gdi, "gdi"),
            (Container::GameCubeIso, "gameCubeIso"),
            (Container::GameCubeCiso, "gameCubeCiso"),
            (Container::Zip, "zip"),
            (Container::SevenZip, "sevenZip"),
            (Container::Chd, "chd"),
            (Container::Cdi, "cdi"),
            (Container::MdsMdf, "mdsMdf"),
            (Container::Nrg, "nrg"),
            (Container::CloneCd, "cloneCd"),
            (Container::PlainIso, "plainIso"),
            (Container::Unknown, "unknown"),
        ];
        for (container, name) in containers {
            assert_eq!(container.as_str(), name);
        }
        let refusals = [
            (DumpRefusal::OtherGame, "otherGame"),
            (DumpRefusal::OtherRevision, "otherRevision"),
            (DumpRefusal::HashMismatch, "hashMismatch"),
            (DumpRefusal::UnsupportedContainer, "unsupportedContainer"),
            (DumpRefusal::Cdi, "cdi"),
            (DumpRefusal::Archive, "archive"),
            (DumpRefusal::Unrecognized, "unrecognized"),
        ];
        for (refusal, name) in refusals {
            assert_eq!(refusal.as_str(), name);
        }
    }

    #[test]
    #[ignore = "needs the player's dump"]
    fn the_players_dumps_identify_as_supported() {
        // The ROM: the US cartridge, dumped in its own order. A failure says which field differs,
        // never what the dump holds.
        let rom = identify(&players_rom())
            .unwrap_or_else(|error| panic!("the ROM is refused: {}", error.rule()));
        assert!(rom.console == Some(Console::Nintendo64), "another console");
        assert!(rom.container == Container::N64Rom, "another container");
        assert!(rom.verdict == Verdict::Supported, "another cartridge");
        assert!(
            rom.title.as_deref() == Some("GAUNTLET LEGENDS"),
            "another title"
        );
        assert!(rom.serial.as_deref() == Some("NGXE"), "another game code");
        assert!(rom.region == Some(Region::NorthAmerica), "another region");
        let Source::N64 {
            size,
            byte_order,
            canonical_sha1,
            ..
        } = rom.source
        else {
            panic!("the ROM's source is no ROM's");
        };
        assert_eq!((size, byte_order), (16 << 20, ByteOrder::BigEndian));
        assert!(canonical_sha1 == US_SHA1, "another digest");

        // The disc: from its sheet, the US disc, every track's file there.
        let sheet = players_disc();
        let disc = identify(&sheet)
            .unwrap_or_else(|error| panic!("the disc is refused: {}", error.rule()));
        assert!(disc.console == Some(Console::Dreamcast), "another console");
        assert!(disc.container == Container::CueSheet, "another container");
        assert!(disc.verdict == Verdict::Supported, "another disc");
        assert!(
            disc.title.as_deref() == Some("GAUNTLET LEGENDS"),
            "another title"
        );
        assert!(disc.serial.as_deref() == Some("T9710N"), "another product");
        assert!(
            disc.revision.as_deref() == Some("V1.003"),
            "another version"
        );
        assert!(disc.region == Some(Region::NorthAmerica), "another region");
        let Source::Dreamcast { tracks, kind, .. } = &disc.source else {
            panic!("the disc's source is no disc's");
        };
        assert_eq!(*kind, SheetKind::Cue);
        assert_eq!(tracks.len(), 5);
        assert!(
            tracks.iter().all(|track| track.size.is_some()),
            "a track's file is missing"
        );
        // The same from the folder that holds it and from each track's file.
        let folder = sheet.parent().unwrap();
        let mut others = vec![(folder.to_path_buf(), "its folder".to_owned())];
        for (index, track) in tracks.iter().enumerate() {
            others.push((folder.join(&track.name), format!("track {}", index + 1)));
        }
        for (path, what) in others {
            let other = identify(&path)
                .unwrap_or_else(|error| panic!("{what} is refused: {}", error.rule()));
            assert!(other == disc, "the disc is told otherwise from {what}");
        }
    }
}
