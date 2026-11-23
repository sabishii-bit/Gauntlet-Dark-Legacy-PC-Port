//! What a dump is, told from its content and never from its name, for the reports that describe
//! it: the console it is for, the container it comes in, what it says of itself, and how it
//! stands against the dumps the launcher installs the game and the expansions from. An N64 ROM is
//! known by its first word, a GameCube disc image by its container's mark or its disc's, a
//! Dreamcast disc by its sheet, and every container the launcher does not read by a mark of its
//! own; each is read no further than telling it takes.

use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use gdl_files::{Cancel, Job, NoProgress};

use crate::dump_file::open_regular;
use crate::gamecube::{CISO_MARK, GAMECUBE_MARK, GAMECUBE_MARK_AT, Layout, read_header};
use crate::gdrom::{head_of, names_gauntlet_legends, sheet_naming, sheet_of, without_mark};
use crate::{
    AREA, ByteOrder, DiscHeader, DiscRule, IpBin, MAX_SHEET_LEN, MediaError, N64Rom, PRODUCT,
    RomHeader, SCRIPT_RULES, Sha1Digest, Sha256Digest, Sheet, SheetKind, SheetTrack, SourceName,
    TrackMode, US_SHA1, VERSION, ascii_field, find_sheet, gamecube_verdict, read_sheet,
};

/// How many bytes at a file's end identification reads: as many as the marks kept there take.
const TAIL_LEN: u64 = 16;

/// What a disc image of the RVZ format, compressed in groups of blocks, begins with.
const RVZ_MARK: &[u8] = b"RVZ\x01";

/// What a disc image of the WIA format, the one RVZ grew from, begins with.
const WIA_MARK: &[u8] = b"WIA\x01";

/// What a GCZ, a disc image compressed in blocks, begins with: its magic word, little-endian.
const GCZ_MARK: &[u8] = &[0x01, 0xC0, 0x0B, 0xB1];

/// What a WBFS, a Wii disc image kept as a partition of discs, begins with.
const WBFS_MARK: &[u8] = b"WBFS";

/// What a TGC, a GameCube disc image a game disc carries inside it, begins with.
const TGC_MARK: &[u8] = &[0xAE, 0x0F, 0x38, 0xA2];

/// What an NFS, a Wii U's image of a Wii disc, begins with.
const NFS_MARK: &[u8] = b"EGGS";

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
    /// A disc image of the RVZ format, which holds a GameCube or a Wii disc.
    Rvz,
    /// A disc image of the WIA format, which holds a GameCube or a Wii disc.
    Wia,
    /// A GCZ, which holds a GameCube or a Wii disc compressed in blocks.
    Gcz,
    /// A WBFS, which holds Wii discs.
    Wbfs,
    /// A TGC, a GameCube disc image carried inside another GameCube disc.
    Tgc,
    /// An NFS, a Wii U's image of a Wii disc.
    Nfs,
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
            Container::Rvz => "rvz",
            Container::Wia => "wia",
            Container::Gcz => "gcz",
            Container::Wbfs => "wbfs",
            Container::Tgc => "tgc",
            Container::Nfs => "nfs",
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
    /// It is for a console whose dumps are judged only when they are read to install from. No
    /// identification gives it: every console's dumps are judged as they are identified.
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
    /// How many bytes the file holds, or `None` when the system shows no regular file there.
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
    /// A GameCube disc image, of which its container and its disc's header were read.
    GameCube {
        /// The image's file name as the system gave it.
        name: SourceName,
        /// How many bytes the image holds.
        size: u64,
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
    /// Its serial: an N64 cartridge's game code, a GameCube disc's game ID, or a Dreamcast disc's
    /// product number.
    pub serial: Option<String>,
    /// Its revision: an N64 ROM's version, or a disc's.
    pub revision: Option<String>,
    /// The region of the world it was made for, when it names exactly one.
    pub region: Option<Region>,
    /// How it stands against the dumps the launcher installs the game and the expansions from.
    pub verdict: Verdict,
    /// What was read of it.
    pub source: Source,
}

/// What the dump at `path` is, told from its content, never from its name. A folder is the disc
/// whose one sheet [`find_sheet`] finds in it, whatever the folder's path ends in, a drive's root
/// or `..` included, since the disc is known by its sheet's name. A file is known by the first of
/// these its content meets: an N64 ROM's first word, in any byte order, when the ROM is read whole
/// and judged against the US cartridge; the marks of a GameCube disc image, whole or sparse, when
/// its container and the header of its disc are read as the installer reads them and the header
/// judged against the US disc, its file table never read; the marks a disc image of another
/// GameCube or Wii format, an archive, a CHD, an Alcohol 120% descriptor or a CloneCD control
/// file begins with, each refused; a sheet's text, the readers' refusal of it included; the marks
/// a DiscJuggler image or a Nero image ends with, each refused; a sheet beside it that names it as
/// a track's file; and an ISO 9660 image's volume descriptor in cooked sectors, refused. Anything
/// else is unrecognized. A Dreamcast disc is read as the Dreamcast extractor reads its head, and
/// judged by its boot area against the US disc, a boot area of another game telling only that.
/// The verdict describes the dump and never decides an extraction or an install, which hold the
/// dump to rules of their own.
///
/// A path that is no folder and names no file is refused before anything is read, and so is one
/// where what is there is neither a folder nor a regular file. Fails when the dump cannot be read,
/// or when the ROM, the GameCube image's container or the Dreamcast disc breaks a rule of its
/// format. Identification reads no more than a file's first [`MAX_SHEET_LEN`] bytes and its last
/// 16, a GameCube image's CISO header and its disc's header, the small files beside it that may be
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
    if ends.marked_for_gamecube() {
        return gamecube_identity(path, name);
    }
    if let Some((console, container, verdict)) = ends.marked_at_start() {
        return Ok(unread(console, container, verdict));
    }
    // The marks at a file's end are looked for only once its text is no sheet, since a sheet's
    // last line, a remark the extractor passes over, may end as one of them does.
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
    if let Some((console, container, verdict)) = ends.marked_at_end() {
        return Ok(unread(console, container, verdict));
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

    /// Whether the file begins as a GameCube disc image does: as a CISO, or with a disc's header
    /// that carries the GameCube's mark. No sheet begins so: no sheet's first line begins with the
    /// CISO's mark, and the GameCube's is no UTF-8.
    fn marked_for_gamecube(&self) -> bool {
        let head = self.head.as_slice();
        head.starts_with(&CISO_MARK)
            || head
                .get(GAMECUBE_MARK_AT..)
                .is_some_and(|rest| rest.starts_with(&GAMECUBE_MARK))
    }

    /// The console, container and verdict of a file that the marks its container's format puts in
    /// its first bytes tell, in the order the marks are looked for, or `None` when none is there.
    /// No sheet holds one there: each is no UTF-8, or a first line no sheet begins with. The
    /// formats of GameCube and Wii disc images other than the ISO and the CISO hold Wii discs as
    /// well as GameCube ones, but for the TGC, and their marks do not tell which.
    fn marked_at_start(&self) -> Option<(Option<Console>, Container, Verdict)> {
        let head = self.head.as_slice();
        let refused = Verdict::Refused;
        let unsupported = refused(DumpRefusal::UnsupportedContainer);
        let disc_images: [(&[u8], Option<Console>, Container); 6] = [
            (RVZ_MARK, None, Container::Rvz),
            (WIA_MARK, None, Container::Wia),
            (GCZ_MARK, None, Container::Gcz),
            (WBFS_MARK, None, Container::Wbfs),
            (TGC_MARK, Some(Console::GameCube), Container::Tgc),
            (NFS_MARK, None, Container::Nfs),
        ];
        let disc_image = disc_images
            .into_iter()
            .find(|(mark, _, _)| head.starts_with(mark));
        let marked = if let Some((_, console, container)) = disc_image {
            (console, container, unsupported)
        } else if head.starts_with(ZIP_MARK) {
            (None, Container::Zip, refused(DumpRefusal::Archive))
        } else if head.starts_with(SEVEN_ZIP_MARK) {
            (None, Container::SevenZip, refused(DumpRefusal::Archive))
        } else if head.starts_with(CHD_MARK) {
            (None, Container::Chd, unsupported)
        } else if head.starts_with(MDS_MARK) {
            (None, Container::MdsMdf, unsupported)
        } else if self.len <= MAX_SHEET_LEN && without_mark(head).starts_with(CLONE_CD_MARK) {
            (None, Container::CloneCd, unsupported)
        } else {
            return None;
        };
        Some(marked)
    }

    /// The console, container and verdict of a file that the marks its container's format puts
    /// at its end tell, in the order the marks are looked for, or `None` when none is there.
    fn marked_at_end(&self) -> Option<(Option<Console>, Container, Verdict)> {
        let marked = if self
            .word_before_end(CDI_VERSION_FROM_END)
            .is_some_and(|version| CDI_VERSIONS.contains(&u32::from_le_bytes(version)))
        {
            (
                Some(Console::Dreamcast),
                Container::Cdi,
                Verdict::Refused(DumpRefusal::Cdi),
            )
        } else if self.word_before_end(NRG_MARK_FROM_END) == Some(NRG_MARK)
            || self.word_before_end(NRG_FIRST_MARK_FROM_END) == Some(NRG_FIRST_MARK)
        {
            let refusal = Verdict::Refused(DumpRefusal::UnsupportedContainer);
            (None, Container::Nrg, refusal)
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

/// What the GameCube disc image at `path`, the file named `name`, is, its container and its disc's
/// header read as the installer reads them, the image opened only once it is seen to be a regular
/// file: the title, game ID, revision and region the header gives, judged against the US disc. A
/// CISO whose disc's header carries no GameCube disc's mark is content the launcher does not
/// know, and tells nothing more. Fails when the image cannot be read, or when its container breaks
/// a rule of its format or holds less than a disc's header.
fn gamecube_identity(path: &Path, name: SourceName) -> Result<Identity, MediaError> {
    let (mut image, len) = open_regular(path)
        .map_err(|error| MediaError::io(path, error))?
        .ok_or_else(|| MediaError::names_no_regular_file(path))?;
    let (layout, _, header) = read_header(&mut image, len, path)?;
    let container = match layout {
        Layout::Iso => Container::GameCubeIso,
        Layout::Ciso(_) => Container::GameCubeCiso,
    };
    let verdict = gamecube_verdict(&header);
    if !header.marked {
        return Ok(unread(None, container, verdict));
    }
    Ok(Identity {
        console: Some(Console::GameCube),
        container,
        title: header.title.clone(),
        serial: game_id_text(&header),
        revision: Some(header.revision.to_string()),
        region: header.region(),
        verdict,
        source: Source::GameCube { name, size: len },
    })
}

/// The game ID `header` gives, when each of its bytes is a letter, a digit or another printable
/// character of ASCII but the space; any other byte would show as no character of the ID.
fn game_id_text(header: &DiscHeader) -> Option<String> {
    let id = &header.game_id;
    id.iter()
        .all(u8::is_ascii_graphic)
        .then(|| String::from_utf8_lossy(id).into_owned())
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
        tracks: track_files(&sheet),
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
/// that holds it, or none when the system shows no regular file there: none is there, something
/// else is, or the system cannot say what is, as for a name it cannot hold. This only describes
/// the dump: the extractor never reads an audio track's file, and a data track's file that cannot
/// be read is refused as its head is read.
fn track_files(sheet: &Sheet) -> Vec<TrackFile> {
    let folder = sheet.path.parent().unwrap_or(Path::new(""));
    let file_len = |track: &SheetTrack| {
        let metadata = fs::metadata(folder.join(&track.file)).ok()?;
        metadata.is_file().then_some(metadata.len())
    };
    let files = sheet.tracks.iter().map(|track| TrackFile {
        name: track.file.clone(),
        mode: track.mode,
        size: file_len(track),
    });
    files.collect()
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

    use gdl_files::Cancel;
    use gdl_fixtures::{
        DiscSet, Edc, edited, raw_sector, reorder, snapshot, synthetic_ciso, synthetic_iso,
    };
    use serde_json::{Value, json};

    use super::{
        Console, Container, DiscIdentity, DumpRefusal, Identity, Region, Source, TrackFile,
        US_DISC, Verdict, disc_verdict, identify, n64_verdict,
    };
    use crate::vectors::{
        boot_of, dreamcast_vectors, frozen, n64_vectors, named, players_disc, players_rom, quiet,
        with_boot_bytes,
    };
    use crate::{
        ByteOrder, GdRom, IpBin, MAX_ROM_LEN, MediaError, RomHeader, SCRIPT_RULES, Sha1Digest,
        Sha256Digest, SheetKind, TrackMode, US_SHA1, parse_sheet,
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

    /// Where a GameCube disc's header keeps the mark every GameCube disc carries.
    const FORMAT_GAMECUBE_MARK_AT: usize = 0x1C;

    /// That mark.
    const FORMAT_GAMECUBE_MARK: [u8; 4] = [0xC2, 0x33, 0x9F, 0x3D];

    /// How many bytes of a disc's start its header takes.
    const FORMAT_DISC_HEADER_LEN: usize = 0x440;

    /// How many bytes a CISO's header takes, its mark, block size and map of blocks.
    const FORMAT_CISO_HEADER_LEN: usize = 0x8000;

    /// Where a CISO's header gives its block size.
    const FORMAT_CISO_BLOCK_SIZE_AT: usize = 4;

    /// Where a CISO's map of blocks starts.
    const FORMAT_CISO_MAP_AT: usize = 8;

    /// Where a CISO built of a disc keeps the disc's first block, after its header.
    const FORMAT_CISO_FIRST_BLOCK_AT: usize = 0x8000;

    /// Where a disc's header gives the region's letter of its game ID.
    const FORMAT_REGION_AT: usize = 3;

    /// Where it gives the disc's number.
    const FORMAT_DISC_NUMBER_AT: usize = 6;

    /// Where it gives the revision.
    const FORMAT_REVISION_AT: usize = 7;

    /// Where it keeps the title.
    const FORMAT_DISC_TITLE_AT: usize = 0x20;

    /// Where an NKit image keeps its mark.
    const FORMAT_NKIT_AT: usize = 0x200;

    /// Where the disc's header gives the place of its file table.
    const FORMAT_FST_OFFSET_AT: usize = 0x424;

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
            matches!(
                error,
                MediaError::Rom { .. } | MediaError::Disc { .. } | MediaError::GameCube { .. }
            ),
            "{error:?}"
        );
        (error.rule(), error.to_string())
    }

    /// The installer tests' disc, a GameCube image of the US disc, revision 1, held in
    /// `container`, from a file named `name` of `size` bytes.
    fn us_gamecube(name: &str, container: Container, size: usize) -> Identity {
        Identity {
            console: Some(Console::GameCube),
            container,
            title: None,
            serial: Some("GUNE5D".to_owned()),
            revision: Some("1".to_owned()),
            region: Some(Region::NorthAmerica),
            verdict: Verdict::Supported,
            source: Source::GameCube {
                name: named(name),
                size: size as u64,
            },
        }
    }

    /// `image` with `bytes` written over it from `at`.
    fn with_bytes(image: &[u8], at: usize, bytes: &[u8]) -> Vec<u8> {
        let mut changed = image.to_vec();
        changed[at..at + bytes.len()].copy_from_slice(bytes);
        changed
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
            // The first mark looked for decides: an archive that ends as a DiscJuggler image does,
            // a plain image that ends so too, and a CloneCD control file that ends as a Nero image
            // does, since a mark a file begins with is looked for before those at its end.
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
            (
                "ccd ending as an nrg",
                [b"[CloneCD]\r\nVersion=3\r\n".to_vec(), nrg(b"NERO", 4)].concat(),
                marked(None, Container::CloneCd, unsupported),
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
        // A GameCube disc image, whole and sparse, is told by its container and its disc's
        // header, whatever its name.
        let iso = synthetic_iso();
        let ciso = synthetic_ciso(&iso);
        let images = [
            ("gamecube iso", &iso, Container::GameCubeIso),
            ("gamecube ciso", &ciso, Container::GameCubeCiso),
        ];
        for (case, bytes, container) in images {
            for name in ["dump.cue", "dump.z64"] {
                let path = file_in(dir.path(), name, bytes);
                let expected = us_gamecube(name, container, bytes.len());
                assert_eq!(identified(&path), expected, "{case} as {name}");
                fs::remove_file(&path).unwrap();
            }
        }
        // The first mark looked for decides: a CISO whose map happens to hold a GameCube disc's
        // mark is read as a CISO, and refused by its map.
        let both = with_bytes(&ciso, FORMAT_GAMECUBE_MARK_AT, &FORMAT_GAMECUBE_MARK);
        let path = file_in(dir.path(), "dump.iso", &both);
        let map = ("cisoMap", "Invalid CISO block map".to_owned());
        assert_eq!(refusal(identify(&path)), map);
        // An N64 ROM's first word decides before any other mark.
        let mut rom = frozen(&n64_vectors(), "default");
        rom[0x1C..0x20].copy_from_slice(&[0xC2, 0x33, 0x9F, 0x3D]);
        rom.extend_from_slice(&0x8000_0004_u32.to_le_bytes());
        rom.extend_from_slice(&[0; 4]);
        let path = file_in(dir.path(), "dump.iso", &rom);
        assert_eq!(identified(&path).container, Container::N64Rom);
    }

    #[test]
    fn gamecube_images_are_judged_by_their_header() {
        let dir = tempfile::tempdir().unwrap();
        let iso = synthetic_iso();
        // The US disc, of either revision it was pressed as, whole and sparse.
        let us = us_gamecube("game.iso", Container::GameCubeIso, iso.len());
        let path = file_in(dir.path(), "game.iso", &iso);
        assert_eq!(identified(&path), us);
        let first = with_bytes(&iso, FORMAT_REVISION_AT, &[0]);
        fs::write(&path, &first).unwrap();
        let revision_0 = Identity {
            revision: Some("0".to_owned()),
            ..us.clone()
        };
        assert_eq!(identified(&path), revision_0);
        let ciso = synthetic_ciso(&iso);
        let path = file_in(dir.path(), "game.ciso", &ciso);
        let sparse = us_gamecube("game.ciso", Container::GameCubeCiso, ciso.len());
        assert_eq!(identified(&path), sparse);

        // Another region, disc or revision of the game, another game, and NKit images, each with
        // what its header gives: what the disc is decides before the form its image takes.
        let other_revision = Verdict::Refused(DumpRefusal::OtherRevision);
        let cases = [
            (
                "europe",
                with_bytes(&iso, FORMAT_REGION_AT, b"P"),
                "GUNP5D",
                "1",
                Some(Region::Europe),
                other_revision,
            ),
            (
                "japan",
                with_bytes(&iso, FORMAT_REGION_AT, b"J"),
                "GUNJ5D",
                "1",
                Some(Region::Japan),
                other_revision,
            ),
            (
                "no region told",
                with_bytes(&iso, FORMAT_REGION_AT, b"X"),
                "GUNX5D",
                "1",
                None,
                other_revision,
            ),
            (
                "second disc",
                with_bytes(&iso, FORMAT_DISC_NUMBER_AT, &[1]),
                "GUNE5D",
                "1",
                Some(Region::NorthAmerica),
                other_revision,
            ),
            (
                "revision 2",
                with_bytes(&iso, FORMAT_REVISION_AT, &[2]),
                "GUNE5D",
                "2",
                Some(Region::NorthAmerica),
                other_revision,
            ),
            (
                "another game",
                with_bytes(&iso, 0, b"GALE01"),
                "GALE01",
                "1",
                Some(Region::NorthAmerica),
                Verdict::Refused(DumpRefusal::OtherGame),
            ),
            (
                "nkit",
                with_bytes(&iso, FORMAT_NKIT_AT, b"NKIT"),
                "GUNE5D",
                "1",
                Some(Region::NorthAmerica),
                Verdict::Refused(DumpRefusal::UnsupportedContainer),
            ),
            (
                "european nkit",
                with_bytes(
                    &with_bytes(&iso, FORMAT_NKIT_AT, b"NKIT"),
                    FORMAT_REGION_AT,
                    b"P",
                ),
                "GUNP5D",
                "1",
                Some(Region::Europe),
                other_revision,
            ),
            (
                "another game's nkit",
                with_bytes(&with_bytes(&iso, FORMAT_NKIT_AT, b"NKIT"), 0, b"GALE01"),
                "GALE01",
                "1",
                Some(Region::NorthAmerica),
                Verdict::Refused(DumpRefusal::OtherGame),
            ),
        ];
        for (case, bytes, serial, revision, region, verdict) in cases {
            let path = file_in(dir.path(), "dump.iso", &bytes);
            let expected = Identity {
                serial: Some(serial.to_owned()),
                revision: Some(revision.to_owned()),
                region,
                verdict,
                ..us_gamecube("dump.iso", Container::GameCubeIso, bytes.len())
            };
            assert_eq!(identified(&path), expected, "{case}");
            // The same disc in a CISO is told the same way.
            let path = file_in(dir.path(), "dump.ciso", &synthetic_ciso(&bytes));
            let identity = identified(&path);
            assert_eq!(identity.container, Container::GameCubeCiso, "{case}");
            assert_eq!(identity.verdict, verdict, "{case}");
            assert_eq!(identity.serial, expected.serial, "{case}");
        }

        // The title the header gives, and a game ID with a byte no ID holds, which is no serial.
        let titled = with_bytes(&iso, FORMAT_DISC_TITLE_AT, b"Gauntlet Dark Legacy\0");
        let path = file_in(dir.path(), "dump.iso", &titled);
        let identity = identified(&path);
        assert_eq!(identity.title.as_deref(), Some("Gauntlet Dark Legacy"));
        assert_eq!(identity.verdict, Verdict::Supported);
        let unprintable = with_bytes(&iso, 5, b"\0");
        let path = file_in(dir.path(), "dump.iso", &unprintable);
        let identity = identified(&path);
        assert_eq!(identity.serial, None);
        assert_eq!(identity.verdict, Verdict::Refused(DumpRefusal::OtherGame));

        // A CISO whose disc's header carries no GameCube disc's mark is nothing the launcher
        // knows, and tells nothing more.
        let at = FORMAT_CISO_FIRST_BLOCK_AT + FORMAT_GAMECUBE_MARK_AT;
        let markless = with_bytes(&ciso, at, &[0; 4]);
        let path = file_in(dir.path(), "dump.ciso", &markless);
        let expected = marked(
            None,
            Container::GameCubeCiso,
            Verdict::Refused(DumpRefusal::Unrecognized),
        );
        assert_eq!(identified(&path), expected);

        // A disc's header alone, with no file table, is the US disc: identification never reads
        // the file table, so a disc whose table is broken is told by its header all the same.
        let mut header = vec![0; FORMAT_DISC_HEADER_LEN];
        header[..6].copy_from_slice(b"GUNE5D");
        header[FORMAT_GAMECUBE_MARK_AT..FORMAT_GAMECUBE_MARK_AT + 4]
            .copy_from_slice(&FORMAT_GAMECUBE_MARK);
        let path = file_in(dir.path(), "game.iso", &header);
        let expected = Identity {
            revision: Some("0".to_owned()),
            ..us_gamecube("game.iso", Container::GameCubeIso, header.len())
        };
        assert_eq!(identified(&path), expected);
        let broken_table = edited(
            &iso,
            &json!([{"op": "u32be", "at": FORMAT_FST_OFFSET_AT, "value": 0xFFFF_FF00_u32}]),
        );
        let path = file_in(dir.path(), "game.iso", &broken_table);
        assert_eq!(identified(&path), us);
    }

    #[test]
    fn a_gamecube_image_whose_container_breaks_its_rules_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let iso = synthetic_iso();
        let ciso = synthetic_ciso(&iso);
        // A CISO's header with blocks of `size` bytes and the rest of its map left out.
        let blocks_of = |size: u32| {
            let mut header = vec![0; FORMAT_CISO_HEADER_LEN];
            header[..4].copy_from_slice(b"CISO");
            let at = FORMAT_CISO_BLOCK_SIZE_AT;
            header[at..at + 4].copy_from_slice(&size.to_le_bytes());
            header
        };
        let mut all_kept = blocks_of(0x8000);
        all_kept[FORMAT_CISO_MAP_AT..].fill(1);
        let mut marked_iso = vec![0; 0x20];
        marked_iso[FORMAT_GAMECUBE_MARK_AT..].copy_from_slice(&FORMAT_GAMECUBE_MARK);
        let truncated_header = ("cisoHeader", "Truncated CISO header");
        let block_size = (
            "cisoBlockSize",
            "Unsupported GameCube CISO block size (not PSP CSO)",
        );
        let map = ("cisoMap", "Invalid CISO block map");
        let truncated_data = ("cisoData", "Truncated CISO data");
        let cases = [
            ("the mark alone", b"CISO".to_vec(), truncated_header),
            (
                "a header cut short",
                ciso[..FORMAT_CISO_HEADER_LEN - 1].to_vec(),
                truncated_header,
            ),
            ("blocks of 0 bytes", blocks_of(0), block_size),
            ("blocks of 511 bytes", blocks_of(511), block_size),
            ("blocks past 16 MiB", blocks_of((16 << 20) + 1), block_size),
            (
                "a map flag of 2",
                with_bytes(&ciso, FORMAT_CISO_MAP_AT + 1, &[2]),
                map,
            ),
            (
                "a block cut short",
                ciso[..ciso.len() - 1].to_vec(),
                truncated_data,
            ),
            ("every block kept, none there", all_kept, truncated_data),
            (
                "a disc shorter than its header",
                marked_iso,
                ("outsideImage", "Disc file extends beyond the image"),
            ),
        ];
        for (case, bytes, (rule, words)) in cases {
            let path = file_in(dir.path(), "dump.iso", &bytes);
            assert_eq!(refusal(identify(&path)), (rule, words.to_owned()), "{case}");
        }
        // The smallest and largest blocks a GameCube CISO keeps are read.
        for size in [512, 16 << 20] {
            let path = file_in(dir.path(), "dump.ciso", &blocks_of(size));
            let expected = marked(
                None,
                Container::GameCubeCiso,
                Verdict::Refused(DumpRefusal::Unrecognized),
            );
            assert_eq!(identified(&path), expected, "{size}");
        }
    }

    #[test]
    fn compressed_disc_containers_are_refused_by_their_marks() {
        let dir = tempfile::tempdir().unwrap();
        let unsupported = Verdict::Refused(DumpRefusal::UnsupportedContainer);
        // The formats that hold Wii discs as well as GameCube ones tell no console; a TGC holds a
        // GameCube disc alone.
        let marks: [(&[u8], Option<Console>, Container); 6] = [
            (b"RVZ\x01", None, Container::Rvz),
            (b"WIA\x01", None, Container::Wia),
            (&[0x01, 0xC0, 0x0B, 0xB1], None, Container::Gcz),
            (b"WBFS", None, Container::Wbfs),
            (
                &[0xAE, 0x0F, 0x38, 0xA2],
                Some(Console::GameCube),
                Container::Tgc,
            ),
            (b"EGGS", None, Container::Nfs),
        ];
        for (mark, console, container) in marks {
            let mut bytes = mark.to_vec();
            bytes.resize(0x100, 0);
            for name in ["dump.iso", "dump.cue"] {
                let path = file_in(dir.path(), name, &bytes);
                let expected = marked(console, container, unsupported);
                assert_eq!(
                    identified(&path),
                    expected,
                    "{} as {name}",
                    container.as_str()
                );
                fs::remove_file(&path).unwrap();
            }
            // A mark anywhere but at the start tells nothing.
            let late = [&[0xFF][..], mark].concat();
            let path = file_in(dir.path(), "dump.iso", &late);
            assert_eq!(identified(&path), unrecognized(), "{}", container.as_str());
        }
        // A GameCube disc's mark is looked for first: an image whose header carries it is told
        // by its header whatever it begins with.
        let mut rvz_named = synthetic_iso();
        rvz_named[..4].copy_from_slice(b"RVZ\x01");
        let path = file_in(dir.path(), "dump.iso", &rvz_named);
        let identity = identified(&path);
        assert_eq!(identity.container, Container::GameCubeIso);
        assert_eq!(identity.verdict, Verdict::Refused(DumpRefusal::OtherGame));
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

        // The audio track's file, which the set leaves out, under a name no file system holds: 130
        // characters past the Basic Multilingual Plane, 260 UTF-16 units and 520 bytes of UTF-8,
        // more than either kind of system takes in one name. The system cannot say what is there,
        // and the extractor never looks, since it reads no audio track's file.
        let dir = tempfile::tempdir().unwrap();
        let sheet = DiscSet::full(Edc::Zero).write(dir.path()).unwrap();
        let unheld = format!("{}.bin", "\u{1d11e}".repeat(130));
        let text = fs::read_to_string(&sheet).unwrap();
        let renamed = text.replace("\"track4.bin\"", &format!("\"{unheld}\""));
        assert_ne!(renamed, text);
        fs::write(&sheet, renamed).unwrap();
        let mut tracks = full_tracks(&vectors, None);
        tracks[1].name = unheld;
        let expected = us_disc(&sheet, SheetKind::Cue, tracks);
        let paths = [
            sheet.clone(),
            dir.path().to_path_buf(),
            dir.path().join("track3.bin"),
        ];
        for path in paths {
            assert_eq!(identified(&path), expected, "{}", path.display());
        }
    }

    #[test]
    fn a_sheet_is_read_before_the_marks_at_a_files_end() {
        let vectors = dreamcast_vectors();
        // The full set's cue sheet with a last line the readers pass over, a remark, that ends the
        // file as one of a Nero image's two marks does: four bytes before the place of its chunks,
        // 32 bits wide in the format's first version and 64 in its second. The disc is told by its
        // sheet, as the extractor reads it, whether it is given as the sheet, as its folder or as
        // a data track's file.
        let remarks: [(&str, &[u8], usize); 2] = [
            ("REM NERO 7\r\n", b"NERO", 8),
            ("REM NER5 12345\r\n", b"NER5", 12),
        ];
        for (remark, mark, from_end) in remarks {
            let dir = tempfile::tempdir().unwrap();
            let sheet = DiscSet::full(Edc::Zero).write(dir.path()).unwrap();
            let mut text = fs::read(&sheet).unwrap();
            text.extend_from_slice(remark.as_bytes());
            assert_eq!(&text[text.len() - from_end..][..mark.len()], mark);
            fs::write(&sheet, &text).unwrap();
            let expected = us_disc(&sheet, SheetKind::Cue, full_tracks(&vectors, None));
            let paths = [
                sheet.clone(),
                dir.path().to_path_buf(),
                dir.path().join("track3.bin"),
            ];
            for path in paths {
                assert_eq!(identified(&path), expected, "{remark:?} {}", path.display());
            }
        }
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
        let full = boot_of(&DiscSet::full(Edc::Zero)).unwrap();
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
        let small = boot_of(&DiscSet::small(Edc::Zero)).unwrap();
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
        let first = set.file_mut("track3.bin");
        first.chunks.push((boot_end as u64, vec![0xFF; rest]));
        let second = set.file_mut("track5.bin");
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
        let opened = GdRom::open(&sheet, &SCRIPT_RULES, &quiet(&Cancel::default()));
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
            (Container::Rvz, "rvz"),
            (Container::Wia, "wia"),
            (Container::Gcz, "gcz"),
            (Container::Wbfs, "wbfs"),
            (Container::Tgc, "tgc"),
            (Container::Nfs, "nfs"),
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
