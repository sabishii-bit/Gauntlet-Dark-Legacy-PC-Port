//! The disc's icon as Windows shows a program's: the first frame of the disc's TPL made into the
//! icon resources that the game's Windows executable carries. The group names six square images,
//! from 16 to 256 pixels a side; each is a 32-bit bitmap from its bottom row up, with its alpha and
//! the mask that older parts of Windows draw by, sampled from the frame as the installer samples
//! it, so that the resources are the installer's byte for byte. On Windows they are set into the
//! staged executable, which is then hashed afresh, and the shell is told of the installed one so
//! that Explorer shows the icon in place of the one it remembers.

use std::fmt;
use std::io;
use std::path::{Path, PathBuf};

use gdl_media::{Frame, MAX_TPL_SIDE, MediaError};

#[cfg(windows)]
use std::fs::OpenOptions;

#[cfg(windows)]
use gdl_files::{BLOCK_LEN, fill_from, retry_locked};
#[cfg(windows)]
use gdl_media::{Sha256Digest, first_frame};
#[cfg(windows)]
use sha2::{Digest, Sha256};

#[cfg(windows)]
use crate::platform::{self, Resource};

/// The sides of the icon's square images, in pixels, smallest first.
pub const ICON_SIZES: [u16; 6] = [16, 32, 48, 64, 128, 256];

/// The disc's icon, by its path in the disc's tree, which an installation keeps as it is.
pub const DISC_ICON: &str = "carddemo/icon.tpl";

/// The resource id of the icon's first image; each image after it takes the next.
pub(crate) const FIRST_ICON_ID: u16 = 1;

/// The resource id of the icon's group, the icon a program is shown by.
#[cfg(windows)]
pub(crate) const ICON_GROUP_ID: u16 = 1;

/// The language the icon's resources are kept under: the neutral one, as the installer keeps them.
#[cfg(windows)]
pub(crate) const RESOURCE_LANGUAGE: u16 = 0;

/// The resource type of an icon's image (RT_ICON).
#[cfg(windows)]
pub(crate) const RT_ICON: u16 = 3;

/// The resource type of an icon's group (RT_GROUP_ICON).
#[cfg(windows)]
pub(crate) const RT_GROUP_ICON: u16 = 14;

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

/// What setting the disc's icon into the game's Windows executable gives: the executable's digest
/// with the icon in it, which the installation records in place of the release's, and the digest
/// of the disc's icon it was made from.
#[cfg(windows)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Personalised {
    /// The SHA-256 digest of the executable, its icon set.
    pub executable_sha256: Sha256Digest,
    /// The SHA-256 digest of the disc's icon, the TPL.
    pub icon_sha256: Sha256Digest,
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
    /// The system did not set the icon into the executable at `path`: the file is no program it
    /// edits, or could not be read or rewritten.
    #[error("{}: {source}", .path.display())]
    Embed {
        /// The executable.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The executable at `path`, its icon set, could not be read back whole or written through to
    /// its disk.
    #[error("{}: {source}", .path.display())]
    Rehash {
        /// The executable.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
}

impl IconError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the rule of its
    /// format that the TPL breaks, `decoded`, `embed` or `rehash`.
    pub fn rule(&self) -> &'static str {
        match self {
            IconError::Tpl(error) => error.rule(),
            IconError::Decoded => "decoded",
            IconError::Embed { .. } => "embed",
            IconError::Rehash { .. } => "rehash",
        }
    }
}

/// Sets the disc's icon, the TPL `tpl`, into the game's Windows executable at `executable`, as
/// the installer personalises the game it stages: the TPL's first frame made into its
/// [`icon_resources`], set in as icon images 1 to 6 and their group 1 in the neutral language,
/// beside every resource the program holds already, its manifest among them. The executable is
/// then read back whole, hashed and written through to its disk, and its new digest given with
/// the TPL's. A TPL that does not decode is refused before the executable is opened. A program
/// that another holds for a moment, as a virus scanner may hold one just written, is waited for
/// as the launcher waits for any file held so: the edit, which leaves the executable as it was
/// when it fails, is begun afresh, and so is the reading back.
#[cfg(windows)]
pub fn personalise(executable: &Path, tpl: &[u8]) -> Result<Personalised, IconError> {
    let frame = first_frame(tpl).map_err(IconError::Tpl)?;
    let made = icon_resources(&frame)?;
    let mut resources: Vec<Resource<'_>> = (FIRST_ICON_ID..)
        .zip(&made.images)
        .map(|(id, image)| Resource {
            kind: RT_ICON,
            id,
            language: RESOURCE_LANGUAGE,
            bytes: image,
        })
        .collect();
    resources.push(Resource {
        kind: RT_GROUP_ICON,
        id: ICON_GROUP_ID,
        language: RESOURCE_LANGUAGE,
        bytes: &made.group,
    });
    retry_locked(|| platform::update_resources(executable, &resources)).map_err(|source| {
        IconError::Embed {
            path: executable.to_path_buf(),
            source,
        }
    })?;
    let executable_sha256 = rehash(executable).map_err(|source| IconError::Rehash {
        path: executable.to_path_buf(),
        source,
    })?;
    Ok(Personalised {
        executable_sha256,
        icon_sha256: Sha256Digest::of(tpl),
    })
}

/// The digest of the file at `executable`, read whole a block at a time through a handle that may
/// also write, so that the file is then written through to its disk before it is closed. A file
/// another holds for a moment is waited for.
#[cfg(windows)]
fn rehash(executable: &Path) -> io::Result<Sha256Digest> {
    let mut options = OpenOptions::new();
    options.read(true).write(true);
    let mut file = retry_locked(|| options.open(executable))?;
    let mut block = vec![0; BLOCK_LEN];
    let mut hasher = Sha256::new();
    loop {
        let read = fill_from(&mut file, &mut block)?;
        hasher.update(&block[..read]);
        if read < block.len() {
            break;
        }
    }
    file.sync_all()?;
    Ok(Sha256Digest(hasher.finalize().into()))
}

/// Tells the shell that the program at `executable` carries a new icon, so that Explorer shows it
/// in place of the one it remembers. Only Windows shows a program by an icon it carries, so
/// elsewhere nothing is done.
pub fn refresh_shell_icon(executable: &Path) {
    #[cfg(windows)]
    platform::notify_item_updated(executable);
    #[cfg(not(windows))]
    let _ = executable;
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
pub(crate) mod tests {
    use std::collections::BTreeSet;
    use std::error::Error;
    use std::fs;
    use std::io;
    use std::path::Path;

    use gdl_fixtures::{edited, first_frame_only_tpl, snapshot, tpl, unhex};
    use gdl_media::{Frame, MediaError, Sha256Digest, first_frame};
    use serde_json::Value;

    use super::{ICON_SIZES, IconError, IconResources, icon_resources, refresh_shell_icon};

    #[cfg(windows)]
    use std::collections::BTreeMap;
    #[cfg(windows)]
    use std::ffi::OsString;
    #[cfg(windows)]
    use std::path::PathBuf;
    #[cfg(windows)]
    use std::sync::mpsc;
    #[cfg(windows)]
    use std::thread;
    #[cfg(windows)]
    use std::time::{Duration, Instant};

    #[cfg(windows)]
    use gdl_files::{RETRY_INTERVAL, RETRY_WINDOW, retry_locked};

    #[cfg(windows)]
    use super::{ICON_GROUP_ID, RESOURCE_LANGUAGE, RT_GROUP_ICON, RT_ICON, personalise, rehash};
    #[cfg(windows)]
    use crate::platform::{self, Resource};

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

    /// What a test's stand-in for a program holds: no program at all.
    const NOT_A_PROGRAM: &[u8] = b"not a PE";

    /// The resource type of an icon's image.
    #[cfg(windows)]
    const FORMAT_RT_ICON: u16 = 3;

    /// The resource type of an icon's group.
    #[cfg(windows)]
    const FORMAT_RT_GROUP_ICON: u16 = 14;

    /// The resource type of a program's manifest.
    #[cfg(windows)]
    const FORMAT_RT_MANIFEST: u16 = 24;

    /// The id of the manifest Windows reads as it starts a program.
    #[cfg(windows)]
    const MANIFEST_ID: u16 = 1;

    /// The neutral language, which the icon's resources are kept under.
    #[cfg(windows)]
    const NEUTRAL_LANGUAGE: u16 = 0;

    /// The language US English, which a program's own manifest is often kept under.
    #[cfg(windows)]
    const ENGLISH_LANGUAGE: u16 = 1033;

    /// The manifest a test program is given in the neutral language. The program is never run.
    #[cfg(windows)]
    const NEUTRAL_MANIFEST: &str = concat!(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>"#,
        r#"<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">"#,
        r#"<trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>"#,
        r#"<requestedExecutionLevel level="asInvoker" uiAccess="false"/>"#,
        r#"</requestedPrivileges></security></trustInfo></assembly>"#,
    );

    /// The manifest a test program is given in US English, another than the neutral one, so that
    /// each is seen kept as its own.
    #[cfg(windows)]
    const ENGLISH_MANIFEST: &str = concat!(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>"#,
        r#"<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">"#,
        r#"<application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>"#,
        r#"<dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>"#,
        r#"</windowsSettings></application></assembly>"#,
    );

    /// Where a program's first header keeps the offset of its PE signature.
    #[cfg(windows)]
    const PE_OFFSET_AT: usize = 0x3C;

    /// Where, from the PE signature, the count of the program's sections is kept.
    #[cfg(windows)]
    const SECTION_COUNT_AT: usize = 6;

    /// Where, from the PE signature, the length of the optional header is kept.
    #[cfg(windows)]
    const OPTIONAL_HEADER_LEN_AT: usize = 20;

    /// Where, from the PE signature, the optional header begins: after the signature and the file
    /// header. The section table follows it.
    #[cfg(windows)]
    const OPTIONAL_HEADER_AT: usize = 24;

    /// How many bytes each row of the section table takes.
    #[cfg(windows)]
    const SECTION_ROW_LEN: usize = 40;

    /// How many bytes a section's name takes at the start of its row, NUL-padded.
    #[cfg(windows)]
    const SECTION_NAME_LEN: usize = 8;

    /// Where in a section's row the length of its bytes in the file is kept.
    #[cfg(windows)]
    const RAW_LEN_AT: usize = 16;

    /// Where in a section's row the offset of its bytes in the file is kept.
    #[cfg(windows)]
    const RAW_START_AT: usize = 20;

    /// Where in a section's row its flags are kept.
    #[cfg(windows)]
    const SECTION_FLAGS_AT: usize = 36;

    /// The flag of a section that holds code (IMAGE_SCN_CNT_CODE).
    #[cfg(windows)]
    const CODE_SECTION: u32 = 0x20;

    /// The share mode of an open that lets no other handle on the file exist beside it.
    #[cfg(windows)]
    const NO_SHARING: u32 = 0;

    /// How long a test holds a program, as a scanner may hold one just written, while the
    /// launcher works on it.
    #[cfg(windows)]
    const HELD_FOR: Duration = Duration::from_millis(300);

    /// Runs `during` while another thread holds the file at `path` open, sharing nothing, and lets
    /// it go [`HELD_FOR`] after the hold was taken; gives what `during` gave and how long it ran.
    #[cfg(windows)]
    fn while_held<T>(path: &Path, during: impl FnOnce() -> T) -> (T, Duration) {
        use std::os::windows::fs::OpenOptionsExt;

        let (taken, held) = mpsc::channel();
        thread::scope(|scope| {
            scope.spawn(move || {
                let mut options = fs::OpenOptions::new();
                options.read(true).share_mode(NO_SHARING);
                // Taken once nothing else holds the file, as a scanner may for a moment.
                let hold = retry_locked(|| options.open(path))
                    .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
                taken.send(()).unwrap();
                thread::sleep(HELD_FOR);
                drop(hold);
            });
            held.recv().expect("the hold was taken");
            let begun = Instant::now();
            let given = during();
            (given, begun.elapsed())
        })
    }

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

    /// Every section of the program `bytes` that holds code, by its name: what a change to the
    /// program's resources leaves as it was.
    #[cfg(windows)]
    fn code_sections(bytes: &[u8]) -> BTreeMap<Vec<u8>, Vec<u8>> {
        let pe = usize::try_from(word(bytes, PE_OFFSET_AT)).unwrap();
        let count = usize::from(half(bytes, pe + SECTION_COUNT_AT));
        let optional_len = usize::from(half(bytes, pe + OPTIONAL_HEADER_LEN_AT));
        let table = pe + OPTIONAL_HEADER_AT + optional_len;
        let mut sections = BTreeMap::new();
        for index in 0..count {
            let row = &bytes[table + index * SECTION_ROW_LEN..][..SECTION_ROW_LEN];
            if word(row, SECTION_FLAGS_AT) & CODE_SECTION == 0 {
                continue;
            }
            let start = usize::try_from(word(row, RAW_START_AT)).unwrap();
            let len = usize::try_from(word(row, RAW_LEN_AT)).unwrap();
            let name = row[..SECTION_NAME_LEN].to_vec();
            sections.insert(name, bytes[start..start + len].to_vec());
        }
        assert!(!sections.is_empty(), "the program holds no code");
        sections
    }

    /// The resource of type `kind`, id `id` and language `language` that the program at `program`
    /// holds, or `None` when it holds none such.
    #[cfg(windows)]
    fn resource(program: &Path, kind: u16, id: u16, language: u16) -> Option<Vec<u8>> {
        platform::resource_bytes(program, kind, id, language)
            .unwrap_or_else(|error| panic!("{}: {error}", program.display()))
    }

    /// `path` made absolute, in its verbatim form, as a path the launcher's own folders give may
    /// come.
    #[cfg(windows)]
    fn verbatim_of(path: &Path) -> PathBuf {
        let absolute = std::path::absolute(path).unwrap();
        let plain = absolute
            .to_str()
            .is_some_and(|text| !text.starts_with('\\'));
        assert!(plain, "{}", absolute.display());
        let mut verbatim = OsString::from(r"\\?\");
        verbatim.push(absolute);
        PathBuf::from(verbatim)
    }

    /// Writes at `path` a copy of the program running these tests, a real linker's output with no
    /// signature, and gives its bytes. No test runs a copy once it is changed.
    #[cfg(windows)]
    fn copy_of_this_program(path: &Path) -> Vec<u8> {
        let original = fs::read(std::env::current_exe().unwrap()).unwrap();
        fs::write(path, &original).unwrap();
        original
    }

    /// Asserts that the program at `program` carries the icon the vectors hold of the icon tests'
    /// TPL `name`: its group in the neutral language, and the six images it names under ids 1 to
    /// 6, each of the golden length and digest, with none past them.
    #[cfg(windows)]
    pub(crate) fn assert_carries_the_golden_icon(program: &Path, name: &str) {
        let vectors = vectors();
        let golden = &vectors["resources"][name];
        let group = resource(program, FORMAT_RT_GROUP_ICON, 1, NEUTRAL_LANGUAGE);
        let group = group.expect("the icon's group");
        assert!(group == unhex(&golden["groupHex"]), "{name}: the group");
        let listed = golden["images"].as_array().expect("a list of images");
        assert_eq!(listed.len(), FORMAT_SIZES.len(), "{name}");
        for (id, (golden_image, size)) in (1..).zip(listed.iter().zip(FORMAT_SIZES)) {
            let image = resource(program, FORMAT_RT_ICON, id, NEUTRAL_LANGUAGE);
            let image = image.unwrap_or_else(|| panic!("{name}: the {size}-pixel image"));
            assert_eq!(
                image.len(),
                number(&golden_image["length"]),
                "{name}: {size}"
            );
            assert_eq!(
                digest(&image),
                text(&golden_image["sha256"]),
                "{name}: {size}"
            );
        }
        let past = u16::try_from(FORMAT_SIZES.len() + 1).unwrap();
        assert_eq!(
            resource(program, FORMAT_RT_ICON, past, NEUTRAL_LANGUAGE),
            None,
            "{name}"
        );
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

    #[test]
    fn embed_and_rehash_failures_name_the_program_and_keep_the_systems_answer() {
        let program = Path::new("staging").join("gauntlet.exe");
        let refused = || io::Error::other("refused");
        let failures = [
            (
                IconError::Embed {
                    path: program.clone(),
                    source: refused(),
                },
                "embed",
            ),
            (
                IconError::Rehash {
                    path: program.clone(),
                    source: refused(),
                },
                "rehash",
            ),
        ];
        for (error, rule) in failures {
            assert_eq!(error.rule(), rule);
            assert_eq!(
                error.to_string(),
                format!("{}: refused", program.display()),
                "{rule}"
            );
            let source = error.source().expect("the system's answer");
            assert_eq!(source.to_string(), "refused", "{rule}");
        }
    }

    #[test]
    fn telling_the_shell_changes_nothing() {
        // The shell is told of a file the test made, and of a name no file can have, which is
        // never named to it; nothing is made, changed or taken away.
        let dir = tempfile::tempdir().unwrap();
        let program = dir.path().join("gauntlet.exe");
        fs::write(&program, NOT_A_PROGRAM).unwrap();
        let before = snapshot(dir.path()).unwrap();
        refresh_shell_icon(&program);
        refresh_shell_icon(&dir.path().join("no\0name.exe"));
        assert_eq!(snapshot(dir.path()).unwrap(), before);
    }

    #[cfg(windows)]
    #[test]
    fn the_resource_types_are_the_systems() {
        use windows_sys::Win32::UI::WindowsAndMessaging as system;

        for (ours, format, systems) in [
            (RT_ICON, FORMAT_RT_ICON, system::RT_ICON),
            (RT_GROUP_ICON, FORMAT_RT_GROUP_ICON, system::RT_GROUP_ICON),
        ] {
            assert_eq!(ours, format);
            assert_eq!(usize::from(ours), systems.addr());
        }
        assert_eq!(usize::from(FORMAT_RT_MANIFEST), system::RT_MANIFEST.addr());
        assert_eq!(ICON_GROUP_ID, 1);
        assert_eq!(RESOURCE_LANGUAGE, NEUTRAL_LANGUAGE);
    }

    #[cfg(windows)]
    #[test]
    fn personalising_a_real_program_keeps_its_code_and_manifest() {
        let dir = tempfile::tempdir().unwrap();
        let program = dir.path().join("program.exe");
        let original = copy_of_this_program(&program);
        // The program is given a manifest in two languages first, as the game's executable carries
        // one, and holds no icon of its own.
        let manifests = [
            (NEUTRAL_LANGUAGE, NEUTRAL_MANIFEST),
            (ENGLISH_LANGUAGE, ENGLISH_MANIFEST),
        ];
        let given: Vec<Resource<'_>> = manifests
            .iter()
            .map(|&(language, manifest)| Resource {
                kind: FORMAT_RT_MANIFEST,
                id: MANIFEST_ID,
                language,
                bytes: manifest.as_bytes(),
            })
            .collect();
        platform::update_resources(&program, &given).unwrap();
        let manifests_kept = || {
            for (language, manifest) in manifests {
                let kept = resource(&program, FORMAT_RT_MANIFEST, MANIFEST_ID, language);
                assert_eq!(kept.as_deref(), Some(manifest.as_bytes()), "{language}");
            }
        };
        manifests_kept();
        let no_group = resource(&program, FORMAT_RT_GROUP_ICON, 1, NEUTRAL_LANGUAGE);
        assert_eq!(no_group, None);

        // Through its verbatim path, the program is given the icon tests' RGB5A3 icon, while
        // another holds it for a moment, as a scanner may hold a program just written: the edit
        // waits until it is let go.
        let icon = tpl(FORMAT_RGB5A3, 8, 8);
        let verbatim = verbatim_of(&program);
        let (personalised, waited) = while_held(&program, || personalise(&verbatim, &icon));
        let personalised = personalised.unwrap_or_else(|error| panic!("{error}"));
        assert!(waited >= RETRY_INTERVAL, "{waited:?}");
        assert!(waited < RETRY_WINDOW, "{waited:?}");

        // The group and the six images it now holds are the golden vector's, and nothing past
        // them.
        assert_carries_the_golden_icon(&program, "rgb5a3");

        // Its code is as the linker wrote it, and both manifests are as they were given.
        let changed = fs::read(&program).unwrap();
        assert!(changed != original);
        assert!(
            code_sections(&changed) == code_sections(&original),
            "the program's code"
        );
        manifests_kept();

        // The digests given are those of the program as it now is and of the icon.
        assert_eq!(personalised.executable_sha256, Sha256Digest::of(&changed));
        assert_eq!(personalised.icon_sha256, Sha256Digest::of(&icon));
        let icon_digest = text(&vectors()["tpls"]["rgb5a3"]["sha256"]).to_owned();
        assert_eq!(personalised.icon_sha256.hex(), icon_digest);

        // Telling the shell of the program leaves it as it is.
        refresh_shell_icon(&verbatim);
        assert!(fs::read(&program).unwrap() == changed);
    }

    #[cfg(windows)]
    #[test]
    fn a_program_held_for_a_moment_is_read_back_once_let_go() {
        // The program as the edit left it is read back whole for its digest, once another that
        // holds it for a moment lets it go.
        let dir = tempfile::tempdir().unwrap();
        let program = dir.path().join("gauntlet.exe");
        fs::write(&program, NOT_A_PROGRAM).unwrap();
        let (digest, waited) = while_held(&program, || rehash(&program));
        let digest = digest.unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(digest, Sha256Digest::of(NOT_A_PROGRAM));
        assert!(waited >= RETRY_INTERVAL, "{waited:?}");
        assert!(waited < RETRY_WINDOW, "{waited:?}");
        assert_eq!(fs::read(&program).unwrap(), NOT_A_PROGRAM);
    }

    #[cfg(windows)]
    #[test]
    fn a_file_that_is_no_program_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("invalid.exe");
        fs::write(&file, NOT_A_PROGRAM).unwrap();
        let error = personalise(&file, &tpl(FORMAT_RGB5A3, 8, 8)).unwrap_err();
        let IconError::Embed { path, source } = &error else {
            panic!("{error:?}");
        };
        // The system's own answer, whatever its code, naming the file, which is as it was.
        assert_eq!(path, &file);
        assert!(source.raw_os_error().is_some(), "{source:?}");
        assert_eq!(error.rule(), "embed");
        assert_eq!(error.to_string(), format!("{}: {source}", file.display()));
        assert_eq!(fs::read(&file).unwrap(), NOT_A_PROGRAM);
    }

    #[cfg(windows)]
    #[test]
    fn a_damaged_icon_is_refused_before_the_program_is_opened() {
        // Every TPL the installer refuses is refused by the decoder's rule, and before the file
        // is opened: one that is no program, which the edit would refuse, never gets that far.
        let vectors = vectors();
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("invalid.exe");
        fs::write(&file, NOT_A_PROGRAM).unwrap();
        for case in vectors["cases"].as_array().unwrap() {
            let name = text(&case["name"]);
            let bytes = edited(&built(text(&case["base"])), &case["edits"]);
            let error = personalise(&file, &bytes).unwrap_err();
            assert!(
                matches!(error, IconError::Tpl(MediaError::Tpl { .. })),
                "{name}: {error:?}"
            );
            assert_eq!(error.rule(), text(&case["rule"]), "{name}");
        }
        assert_eq!(fs::read(&file).unwrap(), NOT_A_PROGRAM);
    }
}
