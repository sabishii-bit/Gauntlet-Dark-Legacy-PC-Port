//! A GameCube disc image as the installer reads one: whole, as an ISO, or sparse, as a CISO, with
//! the header that tells the disc, held to the US disc of the game by one precedence of its rules,
//! which identifying an image and reading it to install from share.

use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

use crate::{DumpRefusal, GameCubeRule, MediaError, Region, Verdict};

mod ciso;

pub(crate) use ciso::CISO_MARK;
use ciso::Ciso;

/// The most bytes a GameCube disc holds: a mini-DVD's. No image is read past it, and the files
/// of its tree together hold no more.
pub const DISC_SIZE: u64 = 1_459_978_240;

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
    /// Fills `buffer` with the bytes of the disc from `offset` on, read from `source`, the image
    /// at `path`, whose disc is `size` bytes long. A read that reaches past the disc is refused
    /// before anything is read, and one the image ends before is refused as cut short.
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
                if fill(source, buffer).map_err(failed)? != buffer.len() {
                    return Err(MediaError::gamecube(
                        GameCubeRule::TruncatedIso,
                        TRUNCATED_ISO,
                    ));
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
    let mut header = [0; DISC_HEADER_LEN];
    layout.read_at(source, path, size, 0, &mut header)?;
    Ok((layout, size, DiscHeader::of(&header)))
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
#[cfg(test)]
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
    use std::io::{self, Cursor, Read, Seek, SeekFrom};
    use std::path::Path;

    use gdl_fixtures::{edited, icon_iso, synthetic_ciso, synthetic_iso, unhex};
    use serde_json::Value;

    use super::{
        DiscHeader, Layout, MAX_REVISION, US_GAME_ID, gamecube_verdict, header_refusal,
        header_rule, read_header,
    };
    use crate::{DumpRefusal, MediaError, Region, Verdict};

    /// The installer's disc cases, which the reader is held to.
    const GAMECUBE_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/installer/gamecube-vectors.json"
    ));

    /// What the vectors hold, each once: a key they gain is refused until a test reads it.
    const VECTOR_KEYS: [&str; 6] = ["cases", "entries", "format", "images", "reads", "version"];

    /// The path the tests' images are named by in the refusals of their reads.
    const IMAGE: &str = "disc.iso";

    /// The cases the container's rules or the disc's header refuse, each by the rule and the words
    /// the vectors give it: every other case passes both, and is refused further in, if at all.
    const HEADER_CASES: [&str; 15] = [
        "empty",
        "cisoMagic",
        "cisoZeros",
        "notAGame",
        "pspCso",
        "id",
        "europe",
        "europeNkit",
        "noMark",
        "cisoNoMark",
        "discNumber",
        "revision2",
        "nkit",
        "cisoMap",
        "cisoTruncated",
    ];

    /// How many bytes of a disc's start its header takes. The format's facts are given here apart
    /// from the reader's own constants, so that a reader that looks elsewhere is caught.
    const FORMAT_HEADER_LEN: usize = 0x440;

    /// How many bytes a CISO's header takes.
    const FORMAT_CISO_HEADER_LEN: usize = 0x8000;

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

    /// The rule and the words of the refusal of the image `bytes` by its container or its disc's
    /// header, or `None` when it passes both.
    fn header_judged(bytes: &[u8]) -> Option<(&'static str, String)> {
        let refusal = match header_of(bytes) {
            Ok((_, _, header)) => header_refusal(&header)?,
            Err(error) => error,
        };
        assert!(
            matches!(refusal, MediaError::GameCube { .. }),
            "{refusal:?}"
        );
        Some((refusal.rule(), refusal.to_string()))
    }

    /// `image` with `bytes` written over it from `at`.
    fn with_bytes(image: &[u8], at: usize, bytes: &[u8]) -> Vec<u8> {
        let mut changed = image.to_vec();
        changed[at..at + bytes.len()].copy_from_slice(bytes);
        changed
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

    /// A reader of `bytes` in memory that keeps how far any read it is asked for reaches and the
    /// longest it is asked for.
    struct Counting<'a> {
        bytes: Cursor<&'a [u8]>,
        furthest: u64,
        longest: usize,
    }

    impl<'a> Counting<'a> {
        fn new(bytes: &'a [u8]) -> Self {
            Counting {
                bytes: Cursor::new(bytes),
                furthest: 0,
                longest: 0,
            }
        }
    }

    impl Read for Counting<'_> {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            let end = self.bytes.position() + buffer.len() as u64;
            self.furthest = self.furthest.max(end);
            self.longest = self.longest.max(buffer.len());
            self.bytes.read(buffer)
        }
    }

    impl Seek for Counting<'_> {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.bytes.seek(to)
        }
    }

    #[test]
    fn header_cases_follow_the_shared_vectors() {
        let vectors = vectors();
        let cases = vectors["cases"].as_array().unwrap();
        let names: Vec<&str> = cases.iter().map(|case| text(&case["name"])).collect();
        for name in HEADER_CASES {
            assert!(names.contains(&name), "the vectors hold no case {name}");
        }
        for case in cases {
            let name = text(&case["name"]);
            let judged = header_judged(&case_image(case));
            if HEADER_CASES.contains(&name) {
                // Refused by its container or its disc's header, by the vectors' rule and in the
                // vectors' words, the installer's own where it refused the image too.
                let expected = (text(&case["rule"]), text(&case["detail"]).to_owned());
                assert_eq!(judged, Some(expected), "{name}");
                if case["python"] == "refused" {
                    assert_eq!(case["message"], case["detail"], "{name}");
                }
            } else {
                // The container and the header are the US disc's: what refuses it lies past them.
                assert_eq!(judged, None, "{name}");
            }
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
        // disc; a case refused only past the header is the US disc's as far as it tells.
        let vectors = vectors();
        for case in vectors["cases"].as_array().unwrap() {
            let name = text(&case["name"]);
            let Ok((_, _, header)) = header_of(&case_image(case)) else {
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
            let refused = header_refusal(&header).map(|refusal| refusal.rule());
            if expected != Verdict::Supported {
                assert_eq!(refused, rule, "{name}");
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
            header_judged(&iso[..FORMAT_HEADER_LEN - 1]),
            Some((
                "outsideImage",
                "Disc file extends beyond the image".to_owned()
            ))
        );
    }

    #[test]
    fn every_cut_and_every_changed_header_byte_is_read_or_refused() {
        // Each image is read or refused by a rule of its format, never with a panic, never reading
        // past its length and never asking for more than a CISO's header in one read.
        let check = |bytes: &[u8]| {
            let mut source = Counting::new(bytes);
            match read_header(&mut source, bytes.len() as u64, Path::new(IMAGE)) {
                Ok((_, _, header)) => {
                    let _ = gamecube_verdict(&header);
                    let _ = header_refusal(&header);
                }
                Err(MediaError::GameCube { .. }) => {}
                Err(other) => panic!("{} bytes: {other:?}", bytes.len()),
            }
            assert!(
                source.furthest <= bytes.len() as u64,
                "{} bytes",
                bytes.len()
            );
            assert!(
                source.longest <= FORMAT_CISO_HEADER_LEN,
                "{} bytes",
                bytes.len()
            );
        };
        let iso = synthetic_iso();
        let ciso = synthetic_ciso(&iso);
        for image in [&iso, &ciso] {
            for cut in 0..=image.len() {
                check(&image[..cut]);
            }
        }
        for at in 0..FORMAT_HEADER_LEN {
            for value in [0x00, 0xFF, iso[at].wrapping_add(1)] {
                check(&with_bytes(&iso, at, &[value]));
            }
        }
    }
}
