//! A disc's icon as the installer decodes one: the first image of a GameCube TPL texture, in one
//! of the three encodings of direct colour that the game's own texture reader takes, decoded to
//! pixels from the top row down, and any other TPL refused by the first rule of its format it
//! breaks, in the installer's words.

use std::fmt;

use crate::{MediaError, TplRule};

/// The most bytes a disc's icon may take, as the installer reads one: a longer icon is refused, so
/// a caller that reads an icon to decode it need read no more than a byte past this.
pub const MAX_TPL_LEN: u64 = 1 << 20;

/// The word every TPL begins with.
const TPL_MAGIC: u32 = 0x0020_AF30;

/// Where a TPL's directory gives how many images its table lists, a big-endian word after the
/// magic.
const IMAGE_COUNT_AT: usize = 4;

/// Where the directory gives where that table lies, a big-endian word.
const TABLE_AT: usize = 8;

/// How many bytes the directory takes: the magic, the count of images and the table's place. No
/// TPL is shorter, and no table lies inside it.
const TPL_DIRECTORY_LEN: u64 = 12;

/// The most images a TPL's table may list.
const MAX_TPL_IMAGES: u32 = 256;

/// How many bytes a row of the table takes: where an image's header lies, then where its
/// palette's does, each a big-endian word.
const TPL_TABLE_ROW_LEN: u64 = 8;

/// Where a row gives where its image's palette lies: nought for an image without one, the only
/// kind the icon may be.
const PALETTE_AT: usize = 4;

/// How many bytes an image's header takes.
const TPL_IMAGE_HEADER_LEN: u64 = 36;

/// Where an image's header gives the image's height, a big-endian half-word.
const HEIGHT_AT: usize = 0;

/// Where it gives the image's width, a big-endian half-word.
const WIDTH_AT: usize = 2;

/// Where it gives the encoding of the image's pixels, a big-endian word.
const ENCODING_AT: usize = 4;

/// Where it gives where the image's pixels lie in the TPL, a big-endian word.
const PIXELS_AT: usize = 8;

/// The longest side of an image the installer takes for an icon, in pixels, so that no decoded
/// frame takes more than four bytes for each of 256 × 256 pixels.
const MAX_TPL_SIDE: u16 = 256;

/// How many pixels a side of a tile holds: an image keeps its pixels in tiles of four by four,
/// the tiles row by row and the pixels of each tile row by row.
const TILE_SIDE: usize = 4;

/// How many bytes a pixel of a 16-bit encoding takes, or each half of an RGBA8 pixel.
const TEXEL_LEN: usize = 2;

/// How many bytes a tile of a 16-bit encoding takes.
const TILE_LEN: usize = 32;

/// How many bytes a tile of RGBA8 takes: the alpha and red of its pixels, then their green and
/// blue.
const RGBA8_TILE_LEN: usize = 64;

/// Where an RGBA8 tile's green and blue begin, after the alpha and red of its sixteen pixels.
const GREEN_BLUE_AT: usize = 32;

/// How many bytes a decoded pixel takes: its red, green, blue and alpha.
const RGBA_LEN: usize = 4;

/// The code of the encoding of 16-bit pixels of five bits of red, six of green and five of blue.
const RGB565: u32 = 4;

/// The code of the encoding of 16-bit pixels that are either opaque, of five bits a colour, or
/// translucent, of three bits of alpha and four a colour.
const RGB5A3: u32 = 5;

/// The code of the encoding of 32-bit pixels, eight bits each of alpha, red, green and blue.
const RGBA8: u32 = 6;

/// Where an RGB565 pixel keeps its red, green and blue: each one's lowest bit and its width.
const RGB565_FIELDS: [(u32, u32); 3] = [(11, 5), (5, 6), (0, 5)];

/// The bit an RGB5A3 pixel sets when it is opaque.
const RGB5A3_OPAQUE: u16 = 0x8000;

/// Where an opaque RGB5A3 pixel keeps its red, green and blue.
const OPAQUE_FIELDS: [(u32, u32); 3] = [(10, 5), (5, 5), (0, 5)];

/// Where a translucent RGB5A3 pixel keeps its red, green and blue.
const TRANSLUCENT_FIELDS: [(u32, u32); 3] = [(8, 4), (4, 4), (0, 4)];

/// Where a translucent RGB5A3 pixel keeps its alpha.
const TRANSLUCENT_ALPHA: (u32, u32) = (12, 3);

/// The alpha of a pixel its encoding keeps opaque.
const OPAQUE_ALPHA: u8 = u8::MAX;

/// The first image of a disc's icon, decoded: its width and height in pixels, and its pixels from
/// the top row down, each row from the left, as red, green, blue and alpha, a byte each.
#[derive(Clone, PartialEq, Eq)]
pub struct Frame {
    /// How many pixels wide the image is.
    pub width: u16,
    /// How many pixels high it is.
    pub height: u16,
    /// Its pixels, four bytes each.
    pub rgba: Vec<u8>,
}

impl fmt::Debug for Frame {
    /// The frame's sides and how many bytes its pixels take, without a pixel of the icon.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        to.debug_struct("Frame")
            .field("width", &self.width)
            .field("height", &self.height)
            .field("rgba_len", &self.rgba.len())
            .finish_non_exhaustive()
    }
}

/// The first image of the TPL `tpl`, decoded as the installer decodes a disc's icon. The TPL is
/// held to its format in the installer's order: its length, its directory, the first image's
/// header, the image's sides, its encoding and palette, then its pixels; the first rule it breaks
/// is the refusal, in the installer's words. Only the first image the table lists is read, and
/// its sides are at most 256 pixels, so no frame takes more than four bytes for each of 256 × 256
/// pixels.
pub fn first_frame(tpl: &[u8]) -> Result<Frame, MediaError> {
    let len = tpl.len() as u64;
    if !(TPL_DIRECTORY_LEN..=MAX_TPL_LEN).contains(&len) {
        return Err(refused(TplRule::Size));
    }
    let count = word_at(tpl, IMAGE_COUNT_AT);
    let table_at = word_at(tpl, TABLE_AT);
    let listed = word_at(tpl, 0) == TPL_MAGIC
        && (1..=MAX_TPL_IMAGES).contains(&count)
        && u64::from(table_at) >= TPL_DIRECTORY_LEN;
    let table = span(tpl, table_at.into(), u64::from(count) * TPL_TABLE_ROW_LEN)
        .filter(|_| listed)
        .ok_or_else(|| refused(TplRule::Directory))?;
    let header = span(tpl, word_at(table, 0).into(), TPL_IMAGE_HEADER_LEN)
        .ok_or_else(|| refused(TplRule::Header))?;
    let (width, height) = (half_at(header, WIDTH_AT), half_at(header, HEIGHT_AT));
    if !(1..=MAX_TPL_SIDE).contains(&width) || !(1..=MAX_TPL_SIDE).contains(&height) {
        return Err(refused(TplRule::Dimensions));
    }
    let encoding = Encoding::of(word_at(header, ENCODING_AT))
        .filter(|_| word_at(table, PALETTE_AT) == 0)
        .ok_or_else(|| refused(TplRule::Format))?;
    let (across, down) = (usize::from(width), usize::from(height));
    let tiles_across = across.div_ceil(TILE_SIDE);
    let tile_len = encoding.tile_len();
    let pixels_len = tiles_across * down.div_ceil(TILE_SIDE) * tile_len;
    let pixels = span(tpl, word_at(header, PIXELS_AT).into(), pixels_len as u64)
        .ok_or_else(|| refused(TplRule::Pixels))?;
    let mut rgba = Vec::with_capacity(across * down * RGBA_LEN);
    for y in 0..down {
        for x in 0..across {
            let tile = (y / TILE_SIDE * tiles_across + x / TILE_SIDE) * tile_len;
            let at = ((y % TILE_SIDE) * TILE_SIDE + x % TILE_SIDE) * TEXEL_LEN;
            rgba.extend(encoding.pixel(&pixels[tile..tile + tile_len], at));
        }
    }
    Ok(Frame {
        width,
        height,
        rgba,
    })
}

/// An encoding of an image's pixels that the launcher decodes.
#[derive(Clone, Copy)]
enum Encoding {
    /// 16-bit pixels of five bits of red, six of green and five of blue.
    Rgb565,
    /// 16-bit pixels either opaque, of five bits a colour, or translucent, of three bits of alpha
    /// and four a colour.
    Rgb5a3,
    /// 32-bit pixels, their alpha and red in a tile's first half and their green and blue in its
    /// second.
    Rgba8,
}

impl Encoding {
    /// The encoding an image's header names by `code`, when the launcher decodes it.
    fn of(code: u32) -> Option<Encoding> {
        match code {
            RGB565 => Some(Encoding::Rgb565),
            RGB5A3 => Some(Encoding::Rgb5a3),
            RGBA8 => Some(Encoding::Rgba8),
            _ => None,
        }
    }

    /// How many bytes a tile of the encoding takes.
    fn tile_len(self) -> usize {
        match self {
            Encoding::Rgb565 | Encoding::Rgb5a3 => TILE_LEN,
            Encoding::Rgba8 => RGBA8_TILE_LEN,
        }
    }

    /// The pixel of `tile`, a tile of the encoding, whose bytes begin `at` bytes into it: as red,
    /// green, blue and alpha.
    fn pixel(self, tile: &[u8], at: usize) -> [u8; RGBA_LEN] {
        match self {
            Encoding::Rgb565 => coloured(half_at(tile, at), RGB565_FIELDS, OPAQUE_ALPHA),
            Encoding::Rgb5a3 => {
                let value = half_at(tile, at);
                if value & RGB5A3_OPAQUE != 0 {
                    coloured(value, OPAQUE_FIELDS, OPAQUE_ALPHA)
                } else {
                    let alpha = widened(value, TRANSLUCENT_ALPHA);
                    coloured(value, TRANSLUCENT_FIELDS, alpha)
                }
            }
            Encoding::Rgba8 => {
                let alpha_red = half_at(tile, at).to_be_bytes();
                let green_blue = half_at(tile, GREEN_BLUE_AT + at).to_be_bytes();
                [alpha_red[1], green_blue[0], green_blue[1], alpha_red[0]]
            }
        }
    }
}

/// The pixel `value` keeps its red, green and blue in at `fields`, each widened to a byte, with
/// `alpha`.
fn coloured(value: u16, fields: [(u32, u32); 3], alpha: u8) -> [u8; RGBA_LEN] {
    let [red, green, blue] = fields.map(|field| widened(value, field));
    [red, green, blue, alpha]
}

/// The channel `value` keeps at `field`, its lowest bit and its width, widened to a byte as the
/// game widens a texture's channels: its bits repeated below themselves until all eight are
/// filled, so that nought stays 0 and every bit set becomes 255.
fn widened(value: u16, (low, bits): (u32, u32)) -> u8 {
    let channel = (u32::from(value) >> low) & ((1 << bits) - 1);
    let mut wide = 0;
    let mut top = u8::BITS;
    while top > 0 {
        wide |= if top >= bits {
            channel << (top - bits)
        } else {
            channel >> (bits - top)
        };
        top = top.saturating_sub(bits);
    }
    u8::try_from(wide).unwrap_or(u8::MAX)
}

/// The `len` bytes of `tpl` from `at`, or `None` where `tpl` ends before they do.
fn span(tpl: &[u8], at: u64, len: u64) -> Option<&[u8]> {
    let start = usize::try_from(at).ok()?;
    let end = usize::try_from(at.checked_add(len)?).ok()?;
    tpl.get(start..end)
}

/// The big-endian word at `at` of `bytes`: nought where `bytes` ends before it, which the checks
/// before each read rule out.
fn word_at(bytes: &[u8], at: usize) -> u32 {
    bytes
        .get(at..)
        .and_then(<[u8]>::first_chunk)
        .map_or(0, |word| u32::from_be_bytes(*word))
}

/// The big-endian half-word at `at` of `bytes`: nought where `bytes` ends before it, which the
/// checks before each read rule out.
fn half_at(bytes: &[u8], at: usize) -> u16 {
    bytes
        .get(at..)
        .and_then(<[u8]>::first_chunk)
        .map_or(0, |half| u16::from_be_bytes(*half))
}

/// The refusal of a TPL by `rule`, in the installer's fixed words for it.
fn refused(rule: TplRule) -> MediaError {
    let detail = match rule {
        TplRule::Size => "Invalid disc icon TPL size",
        TplRule::Directory => "Invalid disc icon TPL directory",
        TplRule::Header => "Truncated disc icon TPL header",
        TplRule::Dimensions => "Invalid disc icon dimensions",
        TplRule::Format => "Unsupported disc icon TPL format",
        TplRule::Pixels => "Truncated disc icon TPL pixels",
    };
    MediaError::tpl(rule, detail)
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::fs::File;
    use std::io::Read;

    use gdl_fixtures::{edited, first_frame_only_tpl, tpl, unhex};
    use serde_json::Value;

    use super::{Frame, MAX_TPL_LEN, first_frame};
    use crate::vectors::{number, players_disc_icon, text};
    use crate::{MediaError, Sha256Digest};

    /// The installer's icon cases, which the decoder is held to.
    const ICON_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/installer/icon-vectors.json"
    ));

    /// What the vectors hold, each once: a key they gain is refused until a test reads it. The
    /// icon's resources are the install crate's to read.
    const VECTOR_KEYS: [&str; 7] = [
        "cases",
        "format",
        "frames",
        "pixels",
        "resources",
        "tpls",
        "version",
    ];

    /// The TPLs the vectors decode, by their names.
    const DECODED: [&str; 4] = ["rgb5a3", "rgb565", "rgba8", "firstFrameOnly"];

    /// The rules the vectors' cases refuse a TPL by: every rule of the format.
    const VECTOR_RULES: [&str; 6] = [
        "size",
        "directory",
        "header",
        "dimensions",
        "format",
        "pixels",
    ];

    /// The code of the encoding RGB565. The format's facts are given here apart from the
    /// decoder's own constants, so that a decoder that reads them otherwise is caught.
    const FORMAT_RGB565: u32 = 4;

    /// The code of the encoding RGB5A3.
    const FORMAT_RGB5A3: u32 = 5;

    /// The code of the encoding RGBA8.
    const FORMAT_RGBA8: u32 = 6;

    /// How many bytes the icon tests' TPLs take before their pixels: the directory, the table and
    /// the image's header, then padding.
    const FORMAT_HEADER_LEN: usize = 64;

    /// The most bytes a disc's icon may take.
    const FORMAT_MAX_TPL_LEN: usize = 1 << 20;

    /// The most images a TPL's table may list.
    const FORMAT_MAX_IMAGES: u32 = 256;

    /// Where a TPL's directory gives how many images its table lists.
    const FORMAT_COUNT_AT: usize = 4;

    /// Where the directory gives where that table lies.
    const FORMAT_TABLE_AT: usize = 8;

    /// How many bytes the directory takes: the magic, the count and the table's place.
    const FORMAT_DIRECTORY_LEN: usize = 12;

    /// Where the icon tests' TPLs keep their image's height, a big-endian half-word at the start
    /// of its header.
    const FORMAT_HEIGHT_AT: usize = 20;

    /// Where they keep its width, the half-word after.
    const FORMAT_WIDTH_AT: usize = 22;

    /// The longest side of an icon's image.
    const FORMAT_MAX_SIDE: u16 = 256;

    /// How many bytes a decoded pixel takes.
    const FORMAT_RGBA_LEN: usize = 4;

    /// The most bytes a decoded frame takes: four for each of 256 × 256 pixels.
    const FORMAT_MOST_FRAME_LEN: usize = FORMAT_RGBA_LEN * 256 * 256;

    /// The side of the player's disc's icon, in pixels.
    const PLAYERS_ICON_SIDE: u16 = 32;

    /// The vectors, read, of their format and version and holding nothing but what the tests know.
    fn vectors() -> Value {
        let vectors: Value = serde_json::from_str(ICON_VECTORS).expect("the icon vectors are JSON");
        assert_eq!(vectors["format"], "gdl-icon-vectors");
        assert_eq!(vectors["version"], 1);
        let object = vectors.as_object().expect("the vectors are an object");
        // As a set, so that the keys' order, which the JSON map may or may not keep, never counts.
        let keys: BTreeSet<&str> = object.keys().map(String::as_str).collect();
        assert_eq!(
            keys,
            BTreeSet::from(VECTOR_KEYS),
            "the vectors hold other keys"
        );
        vectors
    }

    /// The TPL the vectors name `name`, as the icon tests build it.
    fn built(name: &str) -> Vec<u8> {
        match name {
            "rgb5a3" => tpl(FORMAT_RGB5A3, 8, 8),
            "rgb565" => tpl(FORMAT_RGB565, 8, 8),
            "rgba8" => tpl(FORMAT_RGBA8, 8, 8),
            "firstFrameOnly" => first_frame_only_tpl(),
            "empty" => Vec::new(),
            other => panic!("the vectors name no TPL {other:?}"),
        }
    }

    /// The frame of the TPL `bytes`, which must decode.
    fn decoded(bytes: &[u8]) -> Frame {
        first_frame(bytes).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The rule and the words of `outcome`'s refusal, which must be one by a rule of the format.
    fn refusal(outcome: Result<Frame, MediaError>) -> (&'static str, String) {
        let error = outcome.expect_err("a refusal");
        assert!(matches!(error, MediaError::Tpl { .. }), "{error:?}");
        (error.rule(), error.to_string())
    }

    /// `bytes` with the byte at `at` set to `value`.
    fn with_byte(bytes: &[u8], at: usize, value: u8) -> Vec<u8> {
        let mut changed = bytes.to_vec();
        changed[at] = value;
        changed
    }

    /// `bytes` with the big-endian word at `at` set to `value`.
    fn with_word(bytes: &[u8], at: usize, value: u32) -> Vec<u8> {
        let mut changed = bytes.to_vec();
        changed[at..at + 4].copy_from_slice(&value.to_be_bytes());
        changed
    }

    /// `bytes` with the big-endian half-word at `at` set to `value`.
    fn with_half(bytes: &[u8], at: usize, value: u16) -> Vec<u8> {
        let mut changed = bytes.to_vec();
        changed[at..at + 2].copy_from_slice(&value.to_be_bytes());
        changed
    }

    /// The pixel at (`x`, `y`) of an image `width` pixels wide in `encoding`, whose pixels begin
    /// at `offset` of `data`, as red, green, blue and alpha: the installer's arithmetic, written
    /// out as its decoder writes it.
    fn installers_pixel(
        data: &[u8],
        encoding: u32,
        width: usize,
        offset: usize,
        (x, y): (usize, usize),
    ) -> [u8; 4] {
        let tile_bytes = if encoding == FORMAT_RGBA8 { 64 } else { 32 };
        let tile = offset + ((y / 4) * width.div_ceil(4) + x / 4) * tile_bytes;
        let pixel = ((y % 4) * 4 + x % 4) * 2;
        if encoding == FORMAT_RGBA8 {
            let (alpha, red) = (data[tile + pixel], data[tile + pixel + 1]);
            let (green, blue) = (data[tile + 32 + pixel], data[tile + 33 + pixel]);
            return [red, green, blue, alpha];
        }
        let value = u16::from_be_bytes([data[tile + pixel], data[tile + pixel + 1]]);
        let channels = if encoding == FORMAT_RGB565 {
            let (red, green, blue) = (value >> 11, (value >> 5) & 63, value & 31);
            [
                (red << 3) | (red >> 2),
                (green << 2) | (green >> 4),
                (blue << 3) | (blue >> 2),
                255,
            ]
        } else if value & 0x8000 != 0 {
            let colours = [(value >> 10) & 31, (value >> 5) & 31, value & 31];
            let [red, green, blue] = colours.map(|channel| (channel << 3) | (channel >> 2));
            [red, green, blue, 255]
        } else {
            let [red, green, blue] = [8, 4, 0].map(|shift| ((value >> shift) & 15) * 17);
            let alpha = value >> 12;
            [red, green, blue, (alpha << 5) | (alpha << 2) | (alpha >> 1)]
        };
        channels.map(|channel| u8::try_from(channel).unwrap())
    }

    #[test]
    fn pixels_follow_the_shared_vectors() {
        let vectors = vectors();
        for name in DECODED {
            // The icon tests' TPL, built as the vectors keep it, decodes to the frame the
            // installer decoded, every pixel of it.
            let bytes = built(name);
            assert_eq!(bytes, unhex(&vectors["tpls"][name]["hex"]), "{name}");
            let listed = &vectors["frames"][name];
            let frame = decoded(&bytes);
            assert_eq!(usize::from(frame.width), number(&listed["width"]), "{name}");
            assert_eq!(
                usize::from(frame.height),
                number(&listed["height"]),
                "{name}"
            );
            assert_eq!(frame.rgba, unhex(&listed["rgbaHex"]), "{name}");
            assert_eq!(
                Sha256Digest::of(&frame.rgba).hex(),
                text(&listed["sha256"]),
                "{name}"
            );
            let pixels = usize::from(frame.width) * usize::from(frame.height);
            assert_eq!(frame.rgba.len(), pixels * FORMAT_RGBA_LEN, "{name}");
        }
        for pixel in vectors["pixels"].as_array().unwrap() {
            let frame = decoded(&built(text(&pixel["tpl"])));
            let at = (number(&pixel["y"]) * usize::from(frame.width) + number(&pixel["x"]))
                * FORMAT_RGBA_LEN;
            let expected: Vec<u8> = pixel["rgba"]
                .as_array()
                .unwrap()
                .iter()
                .map(|channel| u8::try_from(number(channel)).unwrap())
                .collect();
            assert_eq!(frame.rgba[at..at + FORMAT_RGBA_LEN], expected, "{pixel}");
        }
        // Only the first frame of a TPL whose table lists two is decoded, its partial tiles cut
        // to its five by three pixels.
        let only = decoded(&first_frame_only_tpl());
        assert_eq!((only.width, only.height, only.rgba.len()), (5, 3, 60));
    }

    #[test]
    fn every_pixel_decodes_as_the_installer_decodes_it() {
        // Images whose 16-bit pixels take every value once, and RGBA8 images of varied bytes, of
        // whole and partial tiles: each pixel is the installer's.
        for encoding in [FORMAT_RGB565, FORMAT_RGB5A3, FORMAT_RGBA8] {
            for (width, height) in [(FORMAT_MAX_SIDE, FORMAT_MAX_SIDE), (13, 7), (1, 9)] {
                let mut data = tpl(encoding, width, height);
                for (index, byte) in data[FORMAT_HEADER_LEN..].iter_mut().enumerate() {
                    *byte = if encoding == FORMAT_RGBA8 {
                        u8::try_from(index % 251).unwrap()
                    } else {
                        u16::try_from(index / 2).unwrap().to_be_bytes()[index % 2]
                    };
                }
                let frame = decoded(&data);
                assert_eq!((frame.width, frame.height), (width, height));
                let (across, down) = (usize::from(width), usize::from(height));
                for y in 0..down {
                    for x in 0..across {
                        let at = (y * across + x) * FORMAT_RGBA_LEN;
                        let pixel = &frame.rgba[at..at + FORMAT_RGBA_LEN];
                        let expected =
                            installers_pixel(&data, encoding, across, FORMAT_HEADER_LEN, (x, y));
                        assert_eq!(pixel, expected, "{encoding}: {x}, {y}");
                    }
                }
            }
        }
    }

    #[test]
    fn cases_follow_the_shared_vectors() {
        let vectors = vectors();
        let mut rules = BTreeSet::new();
        for case in vectors["cases"].as_array().unwrap() {
            let name = text(&case["name"]);
            let bytes = edited(&built(text(&case["base"])), &case["edits"]);
            // Refused by the installer and the launcher alike, by the vectors' rule and in the
            // installer's fixed words.
            assert_eq!(case["python"], "refused", "{name}");
            assert_eq!(case["message"], case["detail"], "{name}");
            let expected = (text(&case["rule"]), text(&case["detail"]).to_owned());
            assert_eq!(refusal(first_frame(&bytes)), expected, "{name}");
            rules.insert(expected.0);
        }
        assert_eq!(rules, BTreeSet::from(VECTOR_RULES));
    }

    #[test]
    fn every_cut_and_every_changed_header_byte_is_refused_or_decoded() {
        // Each TPL decodes to a frame of its sides, never more than the largest frame takes, or
        // is refused by a rule of its format, never with a panic.
        let check = |bytes: &[u8]| -> bool {
            match first_frame(bytes) {
                Ok(frame) => {
                    let pixels = usize::from(frame.width) * usize::from(frame.height);
                    assert_eq!(frame.rgba.len(), pixels * FORMAT_RGBA_LEN, "{frame:?}");
                    assert!(frame.rgba.len() <= FORMAT_MOST_FRAME_LEN, "{frame:?}");
                    true
                }
                Err(MediaError::Tpl { .. }) => false,
                Err(other) => panic!("{} bytes: {other:?}", bytes.len()),
            }
        };
        for name in DECODED {
            // A cut TPL decodes only whole: each ends where its last tile does.
            let whole = built(name);
            for cut in 0..=whole.len() {
                assert_eq!(check(&whole[..cut]), cut == whole.len(), "{name}: {cut}");
            }
            for at in 0..FORMAT_HEADER_LEN {
                for value in [0x00, 0xFF, whole[at].wrapping_add(1)] {
                    check(&with_byte(&whole, at, value));
                }
            }
        }
    }

    #[test]
    fn the_bounds_are_taken_up_to_their_edges() {
        // A TPL of the most bytes an icon may take decodes, its padding unread; a byte more is
        // refused.
        let small = tpl(FORMAT_RGB5A3, 8, 8);
        let mut padded = small.clone();
        padded.resize(FORMAT_MAX_TPL_LEN, 0);
        assert_eq!(MAX_TPL_LEN, FORMAT_MAX_TPL_LEN as u64);
        assert_eq!(decoded(&padded), decoded(&small));
        padded.push(0);
        let size = ("size", "Invalid disc icon TPL size".to_owned());
        assert_eq!(refusal(first_frame(&padded)), size);
        // A TPL shorter than its directory is refused by its size; one of the directory alone
        // gets past its size, to a table it does not hold.
        let directory = ("directory", "Invalid disc icon TPL directory".to_owned());
        let short = &small[..FORMAT_DIRECTORY_LEN - 1];
        assert_eq!(refusal(first_frame(short)), size);
        let bare = &small[..FORMAT_DIRECTORY_LEN];
        assert_eq!(refusal(first_frame(bare)), directory);
        // No table lies inside the directory, wherever in it.
        for table_at in [0, 4, 8, FORMAT_DIRECTORY_LEN - 1] {
            let inside = with_word(&small, FORMAT_TABLE_AT, u32::try_from(table_at).unwrap());
            assert_eq!(refusal(first_frame(&inside)), directory, "{table_at}");
        }
        // An image of no pixels either way is refused, whichever side is nought.
        let dimensions = ("dimensions", "Invalid disc icon dimensions".to_owned());
        for at in [FORMAT_HEIGHT_AT, FORMAT_WIDTH_AT] {
            let flat = with_half(&small, at, 0);
            assert_eq!(refusal(first_frame(&flat)), dimensions, "{at}");
        }
        // An image of the longest sides decodes in every encoding, to the largest frame; a side
        // longer is refused, either side.
        for encoding in [FORMAT_RGB565, FORMAT_RGB5A3, FORMAT_RGBA8] {
            let largest = decoded(&tpl(encoding, FORMAT_MAX_SIDE, FORMAT_MAX_SIDE));
            assert_eq!((largest.width, largest.height), (256, 256));
            assert_eq!(largest.rgba.len(), FORMAT_MOST_FRAME_LEN);
            for (width, height) in [(FORMAT_MAX_SIDE + 1, 1), (1, FORMAT_MAX_SIDE + 1)] {
                let longer = tpl(encoding, width, height);
                let outcome = first_frame(&longer);
                assert_eq!(refusal(outcome), dimensions, "{encoding}: {width}x{height}");
            }
            let smallest = decoded(&tpl(encoding, 1, 1));
            assert_eq!(smallest.rgba.len(), FORMAT_RGBA_LEN);
        }
        // A table may list as many images as a TPL may hold, of which the first alone is read;
        // one more is refused.
        let roomy = tpl(FORMAT_RGB5A3, 64, 64);
        let most = with_word(&roomy, FORMAT_COUNT_AT, FORMAT_MAX_IMAGES);
        assert_eq!(decoded(&most), decoded(&roomy));
        let too_many = with_word(&roomy, FORMAT_COUNT_AT, FORMAT_MAX_IMAGES + 1);
        assert_eq!(refusal(first_frame(&too_many)), directory);
    }

    #[test]
    fn a_frames_debug_form_holds_no_pixel() {
        let frame = Frame {
            width: 1,
            height: 1,
            rgba: vec![0xAB, 0xCD, 0xEF, 0x12],
        };
        let shown = format!("{frame:?}");
        assert_eq!(shown, "Frame { width: 1, height: 1, rgba_len: 4, .. }");
    }

    #[test]
    #[ignore = "needs the player's extracted disc"]
    fn the_players_disc_icon_decodes() {
        // The icon of the player's disc, read as an install reads it, at most a byte past the
        // most an icon may take, decodes to a frame of 32 by 32 pixels. A failure says which
        // rule the icon breaks, never what it holds.
        let mut icon = Vec::new();
        File::open(players_disc_icon())
            .and_then(|file| file.take(MAX_TPL_LEN + 1).read_to_end(&mut icon))
            .unwrap_or_else(|error| panic!("the icon cannot be read: {}", error.kind()));
        let frame = first_frame(&icon)
            .unwrap_or_else(|error| panic!("the icon is refused: {}", error.rule()));
        let side = PLAYERS_ICON_SIDE;
        assert!((frame.width, frame.height) == (side, side), "other sides");
        let pixels = usize::from(side) * usize::from(side);
        assert!(
            frame.rgba.len() == pixels * FORMAT_RGBA_LEN,
            "another length"
        );
    }
}
