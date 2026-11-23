//! The disc's icon as Windows shows a program's: the first frame of the disc's TPL made into the
//! icon resources that the game's Windows executable carries. The group names six square images,
//! from 16 to 256 pixels a side; each is a 32-bit bitmap from its bottom row up, with its alpha and
//! the mask that older parts of Windows draw by, sampled from the frame as the installer samples
//! it, so that the resources are the installer's byte for byte.

use std::fmt;

use gdl_media::{Frame, MAX_TPL_SIDE, MediaError};

/// The sides of the icon's square images, in pixels, smallest first.
pub const ICON_SIZES: [u16; 6] = [16, 32, 48, 64, 128, 256];

/// The disc's icon, by its path in the disc's tree, which an installation keeps as it is.
pub const DISC_ICON: &str = "carddemo/icon.tpl";

/// The resource id of the icon's first image; each image after it takes the next.
pub(crate) const FIRST_ICON_ID: u16 = 1;

/// How many bytes an image's bitmap header takes, as its first field says.
pub(crate) const DIB_HEADER_LEN: u32 = 40;

/// How many bits a pixel of an image takes: its blue, green, red and alpha, a byte each.
pub(crate) const BITS_PER_PIXEL: u16 = 32;

/// How many bytes the group's header takes: a reserved half-word, the kind of image the group
/// names, and how many it names.
pub(crate) const GROUP_HEADER_LEN: usize = 6;

/// How many bytes each of the group's entries takes, one an image.
pub(crate) const GROUP_ENTRY_LEN: usize = 14;

/// What the group's header keeps in its reserved half-word.
const GROUP_RESERVED: u16 = 0;

/// The kind of image the group names, in its header: icons, as against cursors.
const ICON_KIND: u16 = 1;

/// How many images the group names, one a size.
const IMAGE_COUNT: u16 = ICON_SIZES.len() as u16;

/// How many colours an entry says its image's palette holds: none, since its pixels are whole
/// colours.
const NO_PALETTE: u8 = 0;

/// What an entry keeps in its reserved byte.
const ENTRY_RESERVED: u8 = 0;

/// How many colour planes an image has: the one a bitmap of whole pixels has.
const PLANES: u16 = 1;

/// How many bitmaps an image's header counts in its height: the pixels, then the mask under them,
/// each as high as the image's side.
const STACKED_BITMAPS: i32 = 2;

/// The compression an image's header names: none.
const UNCOMPRESSED: u32 = 0;

/// How many bytes end an image's header after the length of its pixels: its resolution either way
/// and the two counts of its palette's colours, each left at nought.
const UNSET_HEADER_LEN: usize = 16;

/// How many bytes a pixel takes, in a frame and in an image alike.
const PIXEL_LEN: usize = 4;

/// How many of a mask's bits make a word, to which each row of the mask is padded.
const MASK_WORD_BITS: usize = 32;

/// How many bytes a word of a mask takes.
const MASK_WORD_LEN: usize = 4;

/// How many pixels a byte of a mask covers.
const MASK_BYTE_PIXELS: usize = 8;

/// The bit of a mask's byte that covers the leftmost of its pixels; each pixel to its right takes
/// the next bit down.
const MASK_FIRST_BIT: u8 = 0x80;

/// The icon resources made from the disc's icon: the group, which names each image by its sides,
/// its length and its resource id, and the images it names, in the order of [`ICON_SIZES`], whose
/// ids run from 1.
#[derive(Clone, PartialEq, Eq)]
pub struct IconResources {
    /// The group, the resource of the icon itself.
    pub group: Vec<u8>,
    /// The images, one a size.
    pub images: Vec<Vec<u8>>,
}

impl fmt::Debug for IconResources {
    /// The group's length and each image's, without a pixel of the icon.
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        let image_lens: Vec<usize> = self.images.iter().map(Vec::len).collect();
        to.debug_struct("IconResources")
            .field("group_len", &self.group.len())
            .field("image_lens", &image_lens)
            .finish_non_exhaustive()
    }
}

/// Why the disc's icon could not be made into the icon of the game's executable.
#[derive(Debug, thiserror::Error)]
pub enum IconError {
    /// The disc's icon is no TPL the launcher decodes: it breaks a rule of its format, or keeps
    /// its first image in an encoding the launcher does not decode.
    #[error(transparent)]
    Tpl(MediaError),
    /// The frame given has a side of no pixels or of more than 256, or pixels that do not fill
    /// its sides exactly, so no decoder gave it.
    #[error("Invalid decoded icon")]
    Decoded,
}

impl IconError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the rule of its
    /// format that the TPL breaks, or `decoded`.
    pub fn rule(&self) -> &'static str {
        match self {
            IconError::Tpl(error) => error.rule(),
            IconError::Decoded => "decoded",
        }
    }
}

/// The icon resources of `frame`, the first frame of the disc's icon, as the installer makes them:
/// a group of six images, one of each of [`ICON_SIZES`], each sampled from the frame. A frame no
/// decoder gives, with a side outside 1 to 256 pixels or pixels that do not fill its sides, is
/// refused.
pub fn icon_resources(frame: &Frame) -> Result<IconResources, IconError> {
    let sides = 1..=MAX_TPL_SIDE;
    let pixels = usize::from(frame.width) * usize::from(frame.height);
    if !sides.contains(&frame.width)
        || !sides.contains(&frame.height)
        || frame.rgba.len() != pixels * PIXEL_LEN
    {
        return Err(IconError::Decoded);
    }
    let mut group = Vec::with_capacity(GROUP_HEADER_LEN + ICON_SIZES.len() * GROUP_ENTRY_LEN);
    for field in [GROUP_RESERVED, ICON_KIND, IMAGE_COUNT] {
        group.extend(field.to_le_bytes());
    }
    let mut images = Vec::with_capacity(ICON_SIZES.len());
    for (id, side) in (FIRST_ICON_ID..).zip(ICON_SIZES) {
        let image = image(frame, side);
        push_entry(&mut group, side, image.len(), id);
        images.push(image);
    }
    Ok(IconResources { group, images })
}

/// The image of `frame` that is `side` pixels a side: its bitmap header, its pixels from the
/// bottom row up, each row from the left, as blue, green, red and alpha, then its mask, a bit a
/// pixel in the same order, set where the pixel is clear, each row padded to a whole word. Each
/// pixel is the frame's under it, the frame's rows and columns taken in proportion and rounded
/// down, so that a frame smaller than the image is stretched and a larger one thinned.
fn image(frame: &Frame, side: u16) -> Vec<u8> {
    let (width, height) = (usize::from(frame.width), usize::from(frame.height));
    let (texels, _) = frame.rgba.as_chunks::<PIXEL_LEN>();
    let across = usize::from(side);
    let pixels_len = across * across * PIXEL_LEN;
    let stride = across.div_ceil(MASK_WORD_BITS) * MASK_WORD_LEN;
    let mut image = header(side, pixels_len);
    image.reserve(pixels_len + stride * across);
    let mut mask = vec![0; stride * across];
    for row in 0..across {
        let y = (across - row - 1) * height / across;
        for x in 0..across {
            let [red, green, blue, alpha] = texels[y * width + x * width / across];
            image.extend([blue, green, red, alpha]);
            if alpha == 0 {
                mask[row * stride + x / MASK_BYTE_PIXELS] |=
                    MASK_FIRST_BIT >> (x % MASK_BYTE_PIXELS);
            }
        }
    }
    image.extend(mask);
    image
}

/// The bitmap header of an image `side` pixels a side whose pixels take `pixels_len` bytes. Its
/// height counts the mask with the pixels, so it is twice the side.
fn header(side: u16, pixels_len: usize) -> Vec<u8> {
    let side = i32::from(side);
    let mut header = Vec::new();
    header.extend(DIB_HEADER_LEN.to_le_bytes());
    header.extend(side.to_le_bytes());
    header.extend((STACKED_BITMAPS * side).to_le_bytes());
    header.extend(PLANES.to_le_bytes());
    header.extend(BITS_PER_PIXEL.to_le_bytes());
    header.extend(UNCOMPRESSED.to_le_bytes());
    header.extend(length_field(pixels_len));
    header.extend([0; UNSET_HEADER_LEN]);
    header
}

/// `group` with the entry of an image `side` pixels a side and `len` bytes long, whose resource id
/// is `id`, added: its sides, a byte each, with no palette, then its planes, its depth, its length
/// and its id. A side is written as its low byte, so 256 is written as nought, which is how the
/// format names the largest side.
fn push_entry(group: &mut Vec<u8>, side: u16, len: usize, id: u16) {
    let [low, _] = side.to_le_bytes();
    group.extend([low, low, NO_PALETTE, ENTRY_RESERVED]);
    group.extend(PLANES.to_le_bytes());
    group.extend(BITS_PER_PIXEL.to_le_bytes());
    group.extend(length_field(len));
    group.extend(id.to_le_bytes());
}

/// `len`, the length of an image or of its pixels, as the 32-bit field the format keeps it in. No
/// image of a side of at most 256 pixels takes more than 270,376 bytes, so every length fits.
fn length_field(len: usize) -> [u8; 4] {
    u32::try_from(len).unwrap_or(u32::MAX).to_le_bytes()
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::error::Error;

    use gdl_fixtures::{edited, first_frame_only_tpl, tpl, unhex};
    use gdl_media::{Frame, MediaError, Sha256Digest, first_frame};
    use serde_json::Value;

    use super::{ICON_SIZES, IconError, IconResources, icon_resources};

    /// The installer's icon cases and the resources it made, which the encoder is held to.
    const ICON_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/installer/icon-vectors.json"
    ));

    /// The TPLs whose resources the vectors hold, by their names.
    const RESOURCE_TPLS: [&str; 4] = ["rgb5a3", "rgb565", "rgba8", "firstFrameOnly"];

    /// What the vectors hold of a TPL's resources, each once: a key they gain is refused until a
    /// test reads it.
    const RESOURCE_KEYS: [&str; 3] = ["allSha256", "groupHex", "images"];

    /// What the vectors hold of each image but the smallest.
    const IMAGE_KEYS: [&str; 3] = ["length", "sha256", "size"];

    /// What they hold of the smallest image, which they keep whole.
    const WHOLE_IMAGE_KEYS: [&str; 4] = ["hex", "length", "sha256", "size"];

    /// The sides of an icon's images. The format's facts are given here apart from the encoder's
    /// own constants, so that an encoder that writes them otherwise is caught.
    const FORMAT_SIZES: [usize; 6] = [16, 32, 48, 64, 128, 256];

    /// How many bytes the group's header takes.
    const FORMAT_GROUP_HEADER_LEN: usize = 6;

    /// How many bytes each of the group's entries takes.
    const FORMAT_ENTRY_LEN: usize = 14;

    /// How many bytes an image's bitmap header takes.
    const FORMAT_HEADER_LEN: usize = 40;

    /// How many bytes a pixel takes, in a frame and in an image alike.
    const FORMAT_PIXEL_LEN: usize = 4;

    /// The longest side of a decoded frame.
    const FORMAT_MAX_SIDE: u16 = 256;

    /// The code of the encoding RGB565.
    const FORMAT_RGB565: u32 = 4;

    /// The code of the encoding RGB5A3.
    const FORMAT_RGB5A3: u32 = 5;

    /// The code of the encoding RGBA8.
    const FORMAT_RGBA8: u32 = 6;

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The vectors, read, of their format and version.
    fn vectors() -> Value {
        let vectors: Value = serde_json::from_str(ICON_VECTORS).expect("the icon vectors are JSON");
        assert_eq!(vectors["format"], "gdl-icon-vectors");
        assert_eq!(vectors["version"], 1);
        vectors
    }

    /// The keys of `value`, an object of the vectors, as a set, so that their order never counts.
    fn keys_of(value: &Value) -> BTreeSet<&str> {
        let object = value.as_object().expect("an object");
        object.keys().map(String::as_str).collect()
    }

    /// A text of the vectors.
    fn text(value: &Value) -> &str {
        value.as_str().expect("a string")
    }

    /// A number of the vectors.
    fn number(value: &Value) -> usize {
        usize::try_from(value.as_u64().expect("a number")).unwrap()
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

    /// The resources of the TPL `bytes`, which must decode to a frame that gives them.
    fn resources_of(bytes: &[u8]) -> IconResources {
        let frame = first_frame(bytes).unwrap_or_else(|error| panic!("{error}"));
        icon_resources(&frame).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The SHA-256 digest of `bytes`, in lower-case hexadecimal.
    fn digest(bytes: &[u8]) -> String {
        Sha256Digest::of(bytes).hex()
    }

    /// The little-endian half-word at `at` of `bytes`.
    fn half(bytes: &[u8], at: usize) -> u16 {
        u16::from_le_bytes(bytes[at..at + 2].try_into().unwrap())
    }

    /// The little-endian word at `at` of `bytes`.
    fn word(bytes: &[u8], at: usize) -> u32 {
        u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
    }

    /// The little-endian signed word at `at` of `bytes`.
    fn signed(bytes: &[u8], at: usize) -> i32 {
        i32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
    }

    /// A frame `width` by `height` pixels of varied colours, every third pixel clear and the rest
    /// of varied alpha, none of it nought.
    fn varied_frame(width: u16, height: u16) -> Frame {
        let pixels = usize::from(width) * usize::from(height);
        let mut rgba = Vec::with_capacity(pixels * FORMAT_PIXEL_LEN);
        for pixel in 0..pixels {
            let shade = |step: usize| u8::try_from((pixel * step + step) % 251).unwrap();
            let alpha = if pixel % 3 == 0 { 0 } else { shade(13).max(1) };
            rgba.extend([shade(7), shade(11), shade(5), alpha]);
        }
        Frame {
            width,
            height,
            rgba,
        }
    }

    /// The image of `frame` `side` pixels a side, the installer's arithmetic written out as its
    /// encoder writes it.
    fn installers_image(frame: &Frame, side: usize) -> Vec<u8> {
        let (width, height) = (usize::from(frame.width), usize::from(frame.height));
        let mut pixels = Vec::new();
        let stride = side.div_ceil(32) * 4;
        let mut mask = vec![0_u8; stride * side];
        for row in 0..side {
            let y = (side - row - 1) * height / side;
            for x in 0..side {
                let at = (y * width + x * width / side) * 4;
                let [red, green, blue, alpha] = frame.rgba[at..at + 4].try_into().unwrap();
                pixels.extend([blue, green, red, alpha]);
                if alpha == 0 {
                    mask[row * stride + x / 8] |= 0x80 >> (x % 8);
                }
            }
        }
        let side_field = i32::try_from(side).unwrap();
        let mut image = Vec::new();
        image.extend(40_u32.to_le_bytes());
        image.extend(side_field.to_le_bytes());
        image.extend((side_field * 2).to_le_bytes());
        image.extend(1_u16.to_le_bytes());
        image.extend(32_u16.to_le_bytes());
        image.extend(0_u32.to_le_bytes());
        image.extend(u32::try_from(pixels.len()).unwrap().to_le_bytes());
        image.extend([0_u8; 16]);
        image.extend(pixels);
        image.extend(mask);
        image
    }

    #[test]
    fn resources_equal_the_golden_vector() {
        let vectors = vectors();
        let goldens = &vectors["resources"];
        assert_eq!(keys_of(goldens), BTreeSet::from(RESOURCE_TPLS));
        let smallest = FORMAT_SIZES[0];
        let mut groups = BTreeSet::new();
        for name in RESOURCE_TPLS {
            // Each of the icon tests' TPLs, decoded and made into resources, gives the group and
            // the six images the installer made of it, byte for byte.
            let golden = &goldens[name];
            assert_eq!(keys_of(golden), BTreeSet::from(RESOURCE_KEYS), "{name}");
            let bytes = built(name);
            assert!(
                bytes == unhex(&vectors["tpls"][name]["hex"]),
                "{name}: the TPL"
            );
            let made = resources_of(&bytes);
            assert!(
                made.group == unhex(&golden["groupHex"]),
                "{name}: the group"
            );
            let listed = golden["images"].as_array().expect("a list of images");
            assert_eq!(made.images.len(), listed.len(), "{name}");
            assert_eq!(listed.len(), FORMAT_SIZES.len(), "{name}");
            for ((image, golden_image), size) in made.images.iter().zip(listed).zip(FORMAT_SIZES) {
                assert_eq!(number(&golden_image["size"]), size, "{name}");
                assert_eq!(
                    image.len(),
                    number(&golden_image["length"]),
                    "{name}: {size}"
                );
                assert_eq!(
                    digest(image),
                    text(&golden_image["sha256"]),
                    "{name}: {size}"
                );
                if size == smallest {
                    let keys = BTreeSet::from(WHOLE_IMAGE_KEYS);
                    assert_eq!(keys_of(golden_image), keys, "{name}: {size}");
                    let whole = unhex(&golden_image["hex"]);
                    assert!(*image == whole, "{name}: the {size}-pixel image");
                } else {
                    let keys = BTreeSet::from(IMAGE_KEYS);
                    assert_eq!(keys_of(golden_image), keys, "{name}: {size}");
                }
            }
            let mut all = made.group.clone();
            for image in &made.images {
                all.extend(image);
            }
            assert_eq!(digest(&all), text(&golden["allSha256"]), "{name}");
            groups.insert(made.group);
        }
        // The group names only the images' sides, lengths and ids, so it is one group for every
        // icon.
        assert_eq!(groups.len(), 1);
    }

    #[test]
    fn the_group_and_images_keep_their_layout() {
        // The icon tests' RGB5A3 TPL: its top-left tile opaque red, its bottom-left one
        // translucent (17, 34, 51) at alpha 109 and its bottom-right one clear.
        let made = resources_of(&tpl(FORMAT_RGB5A3, 8, 8));
        let group = &made.group;
        let count = FORMAT_SIZES.len();
        assert_eq!(
            group.len(),
            FORMAT_GROUP_HEADER_LEN + count * FORMAT_ENTRY_LEN
        );
        let header = (half(group, 0), half(group, 2), usize::from(half(group, 4)));
        assert_eq!(header, (0, 1, count));
        assert_eq!(made.images.len(), count);
        assert_eq!(ICON_SIZES.map(usize::from), FORMAT_SIZES);
        for (index, (&size, image)) in FORMAT_SIZES.iter().zip(&made.images).enumerate() {
            // The entry: the sides a byte each, 256 as nought, no palette, one plane of 32 bits,
            // the image's length and its id, from 1.
            let at = FORMAT_GROUP_HEADER_LEN + index * FORMAT_ENTRY_LEN;
            let entry = &group[at..at + FORMAT_ENTRY_LEN];
            let low = u8::try_from(size % 256).unwrap();
            assert_eq!(entry[..4], [low, low, 0, 0], "{size}");
            let fields = (
                half(entry, 4),
                half(entry, 6),
                word(entry, 8),
                half(entry, 12),
            );
            let length = u32::try_from(image.len()).unwrap();
            let id = u16::try_from(index + 1).unwrap();
            assert_eq!(fields, (1, 32, length, id), "{size}");
            // The bitmap header: its own length, the side, twice the side for the pixels and the
            // mask, one plane of 32 bits, no compression and the pixels' length, the rest nought.
            let side = i32::try_from(size).unwrap();
            let fields = (
                word(image, 0),
                signed(image, 4),
                signed(image, 8),
                half(image, 12),
                half(image, 14),
            );
            assert_eq!(fields, (40, side, 2 * side, 1, 32), "{size}");
            let pixels_len = size * size * FORMAT_PIXEL_LEN;
            let pixels_field = usize::try_from(word(image, 20)).unwrap();
            assert_eq!((word(image, 16), pixels_field), (0, pixels_len), "{size}");
            assert_eq!(image[24..FORMAT_HEADER_LEN], [0; 16], "{size}");
            let stride = size.div_ceil(32) * 4;
            assert_eq!(image.len(), FORMAT_HEADER_LEN + pixels_len + stride * size);
            // The bottom row comes first, as blue, green, red and alpha: its first pixel is the
            // frame's bottom-left one.
            let first = FORMAT_HEADER_LEN;
            assert_eq!(image[first..first + 4], [51, 34, 17, 109], "{size}");
            // The top row comes last: its first pixel is the frame's top-left one.
            let top = FORMAT_HEADER_LEN + (size - 1) * size * FORMAT_PIXEL_LEN;
            assert_eq!(image[top..top + 4], [0, 0, 255, 255], "{size}");
            // The mask's first row covers the bottom row, whose right half alone is clear.
            let mask = FORMAT_HEADER_LEN + pixels_len;
            let (left, right) = (&image[mask..mask + size / 16], &image[mask + size / 16..]);
            assert!(left.iter().all(|&byte| byte == 0), "{size}");
            assert!(
                right[..size / 16].iter().all(|&byte| byte == 0xFF),
                "{size}"
            );
        }
    }

    #[test]
    fn every_image_samples_the_frame_as_the_installer_does() {
        // Frames smaller and larger than the images, square and not, with clear pixels among
        // the rest: every image is the installer's, its pixels, its mask and its header alike.
        let most = FORMAT_MAX_SIDE;
        let sides = [
            (1, 1),
            (5, 3),
            (13, 7),
            (8, 8),
            (100, 37),
            (1, most),
            (most, 1),
            (most, most),
        ];
        for (width, height) in sides {
            let frame = varied_frame(width, height);
            let made = icon_resources(&frame).unwrap_or_else(|error| panic!("{error}"));
            for (image, size) in made.images.iter().zip(FORMAT_SIZES) {
                let expected = installers_image(&frame, size);
                assert!(
                    *image == expected,
                    "{width}x{height}: the {size}-pixel image"
                );
            }
        }
    }

    #[test]
    fn a_decoded_frame_of_another_length_is_refused() {
        // Pixels that do not fill the frame's sides exactly, one byte or one pixel either way,
        // none at all or one pixel's worth, are refused before anything is made.
        let whole = varied_frame(3, 2);
        assert!(icon_resources(&whole).is_ok());
        let len = whole.rgba.len();
        for other in [
            0,
            FORMAT_PIXEL_LEN,
            len - FORMAT_PIXEL_LEN,
            len - 1,
            len + 1,
        ] {
            let mut frame = whole.clone();
            frame.rgba.resize(other, 0);
            let outcome = icon_resources(&frame);
            assert!(
                matches!(outcome, Err(IconError::Decoded)),
                "{other}: {outcome:?}"
            );
        }
    }

    #[test]
    fn the_sides_are_taken_up_to_their_edges() {
        // Frames of one pixel a side up to the longest side a decoder gives are taken.
        let most = FORMAT_MAX_SIDE;
        for (width, height) in [(1, 1), (most, 1), (1, most), (most, most)] {
            assert!(
                icon_resources(&varied_frame(width, height)).is_ok(),
                "{width}x{height}"
            );
        }
        // A side of no pixels, or one longer than the longest, is refused, even with the pixels
        // to fill it.
        let longer = FORMAT_MAX_SIDE + 1;
        for (width, height) in [(0, 0), (0, 1), (1, 0), (longer, 1), (1, longer)] {
            let outcome = icon_resources(&varied_frame(width, height));
            assert!(
                matches!(outcome, Err(IconError::Decoded)),
                "{width}x{height}"
            );
        }
    }

    #[test]
    fn a_refused_icon_keeps_the_tpls_rule_and_words() {
        // Every TPL the installer refuses is refused for the icon by the same rule, in the same
        // words, with nothing behind the decoder's refusal.
        let vectors = vectors();
        let mut rules = BTreeSet::new();
        for case in vectors["cases"].as_array().unwrap() {
            let name = text(&case["name"]);
            let bytes = edited(&built(text(&case["base"])), &case["edits"]);
            let refused = first_frame(&bytes).expect_err("a refusal");
            assert!(
                matches!(refused, MediaError::Tpl { .. }),
                "{name}: {refused:?}"
            );
            let error = IconError::Tpl(refused);
            assert_eq!(error.rule(), text(&case["rule"]), "{name}");
            assert_eq!(error.to_string(), text(&case["detail"]), "{name}");
            assert!(error.source().is_none(), "{name}");
            rules.insert(error.rule());
        }
        assert_eq!(rules.len(), 6, "{rules:?}");
        // A frame no decoder gives has a rule of its own.
        assert_eq!(IconError::Decoded.rule(), "decoded");
        assert_eq!(IconError::Decoded.to_string(), "Invalid decoded icon");
        assert!(IconError::Decoded.source().is_none());
    }

    #[test]
    fn a_resources_debug_form_holds_no_pixel() {
        let made = resources_of(&tpl(FORMAT_RGB5A3, 8, 8));
        let shown = format!("{made:?}");
        assert_eq!(
            shown,
            "IconResources { group_len: 90, image_lens: [1128, 4264, 9640, 16936, 67624, 270376], .. }"
        );
    }

    #[test]
    fn errors_stay_small() {
        assert!(size_of::<IconError>() <= MOST_ERROR_BYTES);
    }
}
