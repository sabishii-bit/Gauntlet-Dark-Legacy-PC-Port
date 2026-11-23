//! GameCube disc images and the TPL icons they carry, for the tests of the launcher's GameCube
//! reader, icon decoder and install, built as tests/scripts/test_installer.py and
//! test_installer_icon.py build the installer's: the US disc of nine file table rows, the sparse
//! CISO of any image, that disc with a real icon, the icon tests' TPLs, and the edits the shared
//! vectors make to them. Each builder gives the bytes its Python counterpart gives, which the
//! shared vectors pin.

use serde_json::Value;

use crate::runtime::unhex;

/// The length of [`synthetic_iso`]'s image.
pub const SYNTHETIC_ISO_LEN: usize = 0x4000;

/// Where a disc of these builders keeps its file table.
pub const FST_AT: usize = 0x800;

/// The length of a CISO's header: its mark, its block size and its map of blocks.
pub const CISO_HEADER_LEN: usize = 0x8000;

/// The block size of [`synthetic_ciso`]'s images.
pub const SYNTHETIC_CISO_BLOCK: u32 = 512;

/// Where [`synthetic_iso`] keeps its icon's bytes, and [`iso_with_icon`] writes a TPL.
pub const ICON_DATA_AT: usize = 0x2900;

/// The game ID a disc's header begins with: the US release of the game.
const GAME_ID: &[u8; 6] = b"GUNE5D";

/// Where the header gives the disc's revision.
const REVISION_AT: usize = 7;

/// The revision these discs give, as the installer tests' disc gives it.
const REVISION: u8 = 1;

/// Where the header keeps the mark that says the disc is a GameCube one.
const GAMECUBE_MARK_AT: usize = 0x1C;

/// The mark a GameCube disc's header carries.
const GAMECUBE_MARK: [u8; 4] = [0xC2, 0x33, 0x9F, 0x3D];

/// Where the header gives the file table's offset, then its length, both big-endian.
const FST_EXTENT_AT: usize = 0x424;

/// The length of a row of the file table: its kind and name offset, then its offset and size.
const FST_ROW_LEN: usize = 12;

/// The kind a folder's row gives in the top byte of its first word; a file's is nought.
const FOLDER_KIND: u32 = 0x0100_0000;

/// The largest offset into the names the low 24 bits of a row's first word can give.
const MAX_NAME_OFFSET: u32 = 0x00FF_FFFF;

/// Where [`synthetic_iso`] keeps the war's WAD: 16 bytes before a block's end, so the file runs
/// on into a block that holds only zeros and that a sparse CISO leaves out.
const WAR_WAD_AT: usize = 0x1FF0;

/// The war's WAD's length on the disc: its 16 written bytes and the 16 zeros after them.
const WAR_WAD_LEN: u32 = 32;

/// The bytes written at the start of the war's WAD.
const WAR_WAD_DATA: &[u8] = b"AAAAAAAAAAAAAAAA";

/// Where [`synthetic_iso`] keeps the tower's WAD.
const TOWER_WAD_AT: usize = 0x2800;

/// What the tower's WAD holds.
const TOWER_WAD_DATA: &[u8] = b"tower";

/// What [`synthetic_iso`]'s icon holds: four bytes, too few for any TPL.
const ICON_STAND_IN: &[u8] = b"icon";

/// Where the size of [`synthetic_iso`]'s icon lies: the last word of its file table's ninth row.
const ICON_SIZE_FIELD: usize = FST_AT + 8 * FST_ROW_LEN + 8;

/// The mark a CISO's header begins with.
const CISO_MARK: &[u8; 4] = b"CISO";

/// [`SYNTHETIC_CISO_BLOCK`] as a length of bytes.
const CISO_BLOCK_LEN: usize = SYNTHETIC_CISO_BLOCK as usize;

/// Where a CISO's header gives its block size, little-endian.
const CISO_BLOCK_SIZE_AT: usize = 4;

/// Where a CISO's map of blocks starts: a byte a block, 1 for a block it keeps.
const CISO_MAP_AT: usize = 8;

/// The magic a TPL begins with.
const TPL_MAGIC: u32 = 0x0020_AF30;

/// How many bytes the TPL builder writes before the pixels: the directory, the image table and
/// the one image's header, then zeros.
const TPL_HEADER_LEN: usize = 64;

/// Where a TPL's directory gives its image count.
const TPL_COUNT_AT: usize = 4;

/// Where a TPL's directory gives its image table's offset.
const TPL_TABLE_FIELD: usize = 8;

/// Where the builder's TPLs keep their image table, after the directory's three words.
const TPL_TABLE_AT: usize = 12;

/// Where the builder's TPLs keep the one image's header, after its row of the table.
const TPL_IMAGE_HEADER_AT: usize = 20;

/// The length of a TPL image's header: its height, width, encoding, the pixels' offset and the
/// sampler's fields after them.
const TPL_IMAGE_HEADER_LEN: usize = 36;

/// The encoding of the icon tests' default TPL, RGB5A3.
const RGB5A3: u32 = 5;

/// The encoding whose tiles keep alpha and red, then green and blue, a 64-byte tile: RGBA8.
const RGBA8: u32 = 6;

/// The side of a TPL's tile, in texels.
const TILE_SIDE: u16 = 4;

/// How many texels a tile holds.
const TILE_TEXELS: usize = 16;

/// The alpha and red bytes of every texel of the builder's RGBA8 tiles.
const RGBA8_ALPHA_RED: [u8; 2] = [0x44, 0x88];

/// The green and blue bytes of every texel of the builder's RGBA8 tiles.
const RGBA8_GREEN_BLUE: [u8; 2] = [0x22, 0x11];

/// The texel of each tile of the builder's other TPLs, taken in turn so that each tile has a
/// colour of its own, one of them partly transparent and one wholly.
const TILE_TEXELS_IN_TURN: [u16; 4] = [0xFC00, 0x83E0, 0x3123, 0x0456];

/// The side of the icon tests' default TPL.
const ICON_SIDE: u16 = 8;

/// The width of the TPL whose header was moved past a second row of its table.
const FIRST_FRAME_ONLY_WIDTH: u16 = 5;

/// Its height: one row of tiles, both of them partial.
const FIRST_FRAME_ONLY_HEIGHT: u16 = 3;

/// How many images its table names.
const FIRST_FRAME_ONLY_IMAGES: u32 = 2;

/// Where its one image's header lies, past the table's second row.
const FIRST_FRAME_ONLY_HEADER_AT: usize = 28;

/// A row of a disc's file table below its root, as the installer's tests describe one. A folder
/// gives the index of the folder it lies in, the root's being 0, and the index past its last
/// entry, as the table nests its folders; a file gives where its bytes lie on the disc and how
/// many there are. Every field may be changed to make the row a test needs, a broken one included.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FstEntry {
    /// Its name, whose UTF-8 bytes are written as they are.
    pub name: String,
    /// Whether it is a folder.
    pub folder: bool,
    /// A folder's parent's index, or where a file's bytes start.
    pub offset: u32,
    /// The index past a folder's last entry, or a file's length.
    pub size: u32,
}

impl FstEntry {
    /// A folder inside the folder at index `parent`, holding every entry before index `end`.
    pub fn folder(name: &str, parent: u32, end: u32) -> Self {
        FstEntry {
            name: name.to_owned(),
            folder: true,
            offset: parent,
            size: end,
        }
    }

    /// A file of `size` bytes from `offset` on.
    pub fn file(name: &str, offset: u32, size: u32) -> Self {
        FstEntry {
            name: name.to_owned(),
            folder: false,
            offset,
            size,
        }
    }
}

/// A disc's file table holding `entries` below its root, in their order, as the installer's tests
/// write one: the root's row, whose count is the entries' and its own, each entry's row, then the
/// names, the root's empty one first, each ended by a NUL. Panics when the names outgrow the 24
/// bits a row gives their offset.
pub fn fst(entries: &[FstEntry]) -> Vec<u8> {
    let count = u32::try_from(entries.len() + 1).expect("a table's count fits 32 bits");
    let mut table = Vec::with_capacity(FST_ROW_LEN * (entries.len() + 1));
    let mut names = vec![0];
    push_row(&mut table, FOLDER_KIND, 0, count);
    for entry in entries {
        let name_at = u32::try_from(names.len())
            .ok()
            .filter(|&at| at <= MAX_NAME_OFFSET)
            .expect("a name's offset fits its row's 24 bits");
        let kind = if entry.folder { FOLDER_KIND } else { 0 };
        push_row(&mut table, kind | name_at, entry.offset, entry.size);
        names.extend_from_slice(entry.name.as_bytes());
        names.push(0);
    }
    table.extend(names);
    table
}

/// A disc image of `len` bytes: the US game's header (`GUNE5D`, disc 0, revision 1 and the
/// GameCube mark), the file table `table` at [`FST_AT`] with its extent in the header, each run of
/// `data` at its offset, and zeros everywhere else. Panics when the table or a run does not fit.
pub fn iso_of(table: &[u8], data: &[(usize, &[u8])], len: usize) -> Vec<u8> {
    let table_len = u32::try_from(table.len()).expect("a table's length fits 32 bits");
    let mut image = vec![0; len];
    put(&mut image, 0, GAME_ID);
    put(&mut image, REVISION_AT, &[REVISION]);
    put(&mut image, GAMECUBE_MARK_AT, &GAMECUBE_MARK);
    put(&mut image, FST_EXTENT_AT, &(FST_AT as u32).to_be_bytes());
    put(&mut image, FST_EXTENT_AT + 4, &table_len.to_be_bytes());
    put(&mut image, FST_AT, table);
    for &(at, bytes) in data {
        put(&mut image, at, bytes);
    }
    image
}

/// The installer tests' disc: 16 KiB with a file table of nine rows, `Gauntlet` holding `PDATA`
/// with `WAR.WAD`, `WDATA` with `TOWER.WAD` and the empty `EMPTY`, then `carddemo` holding a
/// four-byte `icon.tpl`. `WAR.WAD` runs across a block's end into zeros, so the sparse CISO of the
/// disc reads part of it from a block it leaves out.
pub fn synthetic_iso() -> Vec<u8> {
    // The rows' numbers are the installer tests' own: each folder's parent and the index past its
    // last entry, each file's place and length.
    let table = fst(&[
        FstEntry::folder("Gauntlet", 0, 7),
        FstEntry::folder("PDATA", 1, 4),
        FstEntry::file("WAR.WAD", WAR_WAD_AT as u32, WAR_WAD_LEN),
        FstEntry::folder("WDATA", 1, 6),
        FstEntry::file(
            "TOWER.WAD",
            TOWER_WAD_AT as u32,
            TOWER_WAD_DATA.len() as u32,
        ),
        FstEntry::folder("EMPTY", 1, 7),
        FstEntry::folder("carddemo", 0, 9),
        FstEntry::file("icon.tpl", ICON_DATA_AT as u32, ICON_STAND_IN.len() as u32),
    ]);
    iso_of(
        &table,
        &[
            (WAR_WAD_AT, WAR_WAD_DATA),
            (TOWER_WAD_AT, TOWER_WAD_DATA),
            (ICON_DATA_AT, ICON_STAND_IN),
        ],
        SYNTHETIC_ISO_LEN,
    )
}

/// `image` as a sparse CISO of 512-byte blocks: the mark, the block size, a map byte of 1 for each
/// block holding a byte that is not zero, then those blocks in order. A partial block at the end
/// is left out, as the installer tests' builder leaves it. Panics when a block it keeps lies past
/// what the map can flag.
pub fn synthetic_ciso(image: &[u8]) -> Vec<u8> {
    let mut header = vec![0; CISO_HEADER_LEN];
    put(&mut header, 0, CISO_MARK);
    put(
        &mut header,
        CISO_BLOCK_SIZE_AT,
        &SYNTHETIC_CISO_BLOCK.to_le_bytes(),
    );
    let mut blocks = Vec::new();
    for (index, block) in image.as_chunks::<CISO_BLOCK_LEN>().0.iter().enumerate() {
        if block.iter().any(|&byte| byte != 0) {
            *header
                .get_mut(CISO_MAP_AT + index)
                .expect("the CISO's map flags every block it keeps") = 1;
            blocks.extend_from_slice(block);
        }
    }
    header.extend(blocks);
    header
}

/// [`synthetic_iso`] with `icon` as its icon, as the installer's Windows icon tests make it: the
/// icon's row gives the icon's length, and its bytes are written at [`ICON_DATA_AT`] over the
/// stand-in's, the image growing to hold an icon that runs past its end.
pub fn iso_with_icon(icon: &[u8]) -> Vec<u8> {
    let mut image = synthetic_iso();
    let icon_len = u32::try_from(icon.len()).expect("an icon's length fits 32 bits");
    put(&mut image, ICON_SIZE_FIELD, &icon_len.to_be_bytes());
    let end = ICON_DATA_AT + icon.len();
    if image.len() < end {
        image.resize(end, 0);
    }
    put(&mut image, ICON_DATA_AT, icon);
    image
}

/// The disc the install tests copy: [`synthetic_iso`] with the 8×8 RGB5A3 icon of the icon tests,
/// [`tpl`]`(5, 8, 8)`, so that its icon decodes.
pub fn icon_iso() -> Vec<u8> {
    iso_with_icon(&tpl(RGB5A3, ICON_SIDE, ICON_SIDE))
}

/// The icon tests' TPL of one `width` by `height` image in `encoding`: the magic, a count of 1 and
/// the table at 12; the table's one row naming the image's header at 20 and no palette; the
/// header's height, width, encoding and the pixels at 64; then a tile at a time, 64 bytes of the
/// same texel for encoding 6 and 32 for any other, each tile but those of 6 taking the next of
/// four texels in turn.
pub fn tpl(encoding: u32, width: u16, height: u16) -> Vec<u8> {
    let mut data = vec![0; TPL_HEADER_LEN];
    put(&mut data, 0, &TPL_MAGIC.to_be_bytes());
    put(&mut data, TPL_COUNT_AT, &1_u32.to_be_bytes());
    put(
        &mut data,
        TPL_TABLE_FIELD,
        &(TPL_TABLE_AT as u32).to_be_bytes(),
    );
    put(
        &mut data,
        TPL_TABLE_AT,
        &(TPL_IMAGE_HEADER_AT as u32).to_be_bytes(),
    );
    let mut header = Vec::with_capacity(TPL_IMAGE_HEADER_LEN);
    header.extend(height.to_be_bytes());
    header.extend(width.to_be_bytes());
    header.extend(encoding.to_be_bytes());
    header.extend((TPL_HEADER_LEN as u32).to_be_bytes());
    put(&mut data, TPL_IMAGE_HEADER_AT, &header);
    let tiles = usize::from(width.div_ceil(TILE_SIDE)) * usize::from(height.div_ceil(TILE_SIDE));
    for tile in 0..tiles {
        if encoding == RGBA8 {
            data.extend(RGBA8_ALPHA_RED.repeat(TILE_TEXELS));
            data.extend(RGBA8_GREEN_BLUE.repeat(TILE_TEXELS));
        } else {
            let texel = TILE_TEXELS_IN_TURN[tile % TILE_TEXELS_IN_TURN.len()].to_be_bytes();
            data.extend(texel.repeat(TILE_TEXELS));
        }
    }
    data
}

/// The icon tests' 5×3 RGB5A3 TPL whose table names two images: its one image's header moved past
/// the table's second row, which names the same header, so only the first frame is decoded.
pub fn first_frame_only_tpl() -> Vec<u8> {
    let mut data = tpl(RGB5A3, FIRST_FRAME_ONLY_WIDTH, FIRST_FRAME_ONLY_HEIGHT);
    let header = data[TPL_IMAGE_HEADER_AT..TPL_IMAGE_HEADER_AT + TPL_IMAGE_HEADER_LEN].to_vec();
    put(&mut data, FIRST_FRAME_ONLY_HEADER_AT, &header);
    put(
        &mut data,
        TPL_COUNT_AT,
        &FIRST_FRAME_ONLY_IMAGES.to_be_bytes(),
    );
    let row = [FIRST_FRAME_ONLY_HEADER_AT as u32, 0];
    for (index, word) in row.iter().chain(&row).enumerate() {
        put(&mut data, TPL_TABLE_AT + 4 * index, &word.to_be_bytes());
    }
    data
}

/// `base` with a shared vector's `edits` made to it in their order, as
/// tests/scripts/installer_vectors.py makes them: `u32be` writes the big-endian word `value` at
/// `at`, `byte` the byte `value`, `bytes` the bytes `hex` spells; `truncate` keeps the first `to`
/// bytes and `dropLast` drops the last `count`; `append` adds the bytes `hex` spells and
/// `appendZeros` `count` zeros. Panics on an edit it does not know and on one that reaches past
/// the bytes it changes, as the generator refuses them.
pub fn edited(base: &[u8], edits: &Value) -> Vec<u8> {
    let mut data = base.to_vec();
    for edit in edits.as_array().expect("a list of edits") {
        match edit["op"].as_str().expect("an edit's name") {
            "u32be" => {
                let value = u32::try_from(number(&edit["value"])).expect("a word of 32 bits");
                put(&mut data, offset(&edit["at"]), &value.to_be_bytes());
            }
            "byte" => {
                let value = u8::try_from(number(&edit["value"])).expect("a byte");
                put(&mut data, offset(&edit["at"]), &[value]);
            }
            "bytes" => put(&mut data, offset(&edit["at"]), &unhex(&edit["hex"])),
            "truncate" => {
                let to = offset(&edit["to"]);
                within(data.len(), 0, to);
                data.truncate(to);
            }
            "dropLast" => {
                let count = offset(&edit["count"]);
                within(data.len(), 0, count);
                data.truncate(data.len() - count);
            }
            "append" => data.extend(unhex(&edit["hex"])),
            "appendZeros" => data.resize(data.len() + offset(&edit["count"]), 0),
            other => panic!("no edit is called {other:?}"),
        }
    }
    data
}

/// Appends a row of a file table: its first word, then its offset and size, big-endian.
fn push_row(table: &mut Vec<u8>, first: u32, offset: u32, size: u32) {
    for word in [first, offset, size] {
        table.extend(word.to_be_bytes());
    }
}

/// Writes `bytes` over `data` from `at` on. Panics when they reach past its end.
fn put(data: &mut [u8], at: usize, bytes: &[u8]) {
    within(data.len(), at, bytes.len());
    data[at..at + bytes.len()].copy_from_slice(bytes);
}

/// Panics unless `length` bytes from `at` lie within `len` bytes.
fn within(len: usize, at: usize, length: usize) {
    assert!(
        at.checked_add(length).is_some_and(|end| end <= len),
        "{length} bytes at {at} reach past {len} bytes"
    );
}

/// An edit's number.
fn number(value: &Value) -> u64 {
    value.as_u64().expect("a number")
}

/// An edit's number taken as an offset or a count of bytes.
fn offset(value: &Value) -> usize {
    usize::try_from(number(value)).expect("an offset this system can address")
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::panic::{self, UnwindSafe};

    use serde_json::{Value, json};

    use super::{
        CISO_HEADER_LEN, FST_AT, FstEntry, ICON_DATA_AT, ICON_SIZE_FIELD, SYNTHETIC_CISO_BLOCK,
        SYNTHETIC_ISO_LEN, edited, first_frame_only_tpl, fst, icon_iso, iso_of, iso_with_icon,
        synthetic_ciso, synthetic_iso, tpl,
    };
    use crate::runtime::{hex, sha256_hex, unhex};

    /// The installer's disc cases, which the launcher's GameCube reader is held to.
    const GAMECUBE_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/installer/gamecube-vectors.json"
    ));

    /// The installer's icon cases, which the launcher's TPL decoder and icon resources are held to.
    const ICON_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/installer/icon-vectors.json"
    ));

    /// The libraries the icon vectors give by encoding and size, by their names there.
    const BUILT_TPLS: [&str; 3] = ["rgb5a3", "rgb565", "rgba8"];

    fn gamecube_vectors() -> Value {
        let vectors: Value =
            serde_json::from_str(GAMECUBE_VECTORS).expect("the GameCube vectors are JSON");
        assert_eq!(vectors["format"], "gdl-gamecube-vectors");
        assert_eq!(vectors["version"], 1);
        vectors
    }

    fn icon_vectors() -> Value {
        let vectors: Value = serde_json::from_str(ICON_VECTORS).expect("the icon vectors are JSON");
        assert_eq!(vectors["format"], "gdl-icon-vectors");
        assert_eq!(vectors["version"], 1);
        vectors
    }

    /// The big-endian word at `at`.
    fn word_at(bytes: &[u8], at: usize) -> u32 {
        u32::from_be_bytes(bytes[at..at + 4].try_into().unwrap())
    }

    /// A vector's number taken as an offset or a length.
    fn index(value: &Value) -> usize {
        usize::try_from(value.as_u64().expect("a number")).unwrap()
    }

    /// The vector's list of the numbers in `value`.
    fn indices(value: &Value) -> Vec<usize> {
        value.as_array().unwrap().iter().map(index).collect()
    }

    /// Whether `run` panics.
    fn panics<T>(run: impl FnOnce() -> T + UnwindSafe) -> bool {
        panic::catch_unwind(run).is_err()
    }

    #[test]
    fn the_builders_are_the_installer_tests_builders() {
        let vectors = gamecube_vectors();
        let images = &vectors["images"];

        let iso = synthetic_iso();
        assert_eq!(iso.len(), SYNTHETIC_ISO_LEN);
        assert_eq!(iso.len(), images["iso"]["length"]);
        assert_eq!(sha256_hex(&iso), images["iso"]["sha256"]);
        assert_eq!(&iso[..6], b"GUNE5D");
        assert_eq!(iso[7], images["iso"]["revision"]);
        assert_eq!(&iso[0x1C..0x20], [0xC2, 0x33, 0x9F, 0x3D]);
        assert_eq!(word_at(&iso, 0x424), FST_AT as u32);
        let table_len = word_at(&iso, 0x428) as usize;
        assert_eq!(
            hex(&iso[FST_AT..FST_AT + table_len]),
            images["iso"]["fstHex"]
        );
        // Each file's bytes lie where its row says, as the installer read them.
        for entry in vectors["entries"].as_array().unwrap() {
            if entry["folder"] == false {
                let at = index(&entry["offset"]);
                let bytes = &iso[at..at + index(&entry["size"])];
                assert_eq!(sha256_hex(bytes), entry["sha256"], "{entry}");
            }
        }

        let ciso = synthetic_ciso(&iso);
        assert_eq!(ciso.len(), images["ciso"]["length"]);
        assert_eq!(sha256_hex(&ciso), images["ciso"]["sha256"]);
        assert_eq!(&ciso[..4], b"CISO");
        let block_size = u32::from_le_bytes(ciso[4..8].try_into().unwrap());
        assert_eq!(block_size, SYNTHETIC_CISO_BLOCK);
        assert_eq!(block_size, images["ciso"]["blockSize"]);
        let used: Vec<usize> = (0..CISO_HEADER_LEN - 8)
            .filter(|&block| ciso[8 + block] != 0)
            .collect();
        assert_eq!(used, indices(&images["ciso"]["usedBlocks"]));
        assert!(ciso[8..CISO_HEADER_LEN].iter().all(|&flag| flag <= 1));
        // The blocks it keeps, put back where the map says, are the disc again.
        let block = block_size as usize;
        let mut rebuilt = vec![0; iso.len()];
        for (kept, &at) in used.iter().enumerate() {
            let stored = CISO_HEADER_LEN + kept * block;
            rebuilt[at * block..(at + 1) * block].copy_from_slice(&ciso[stored..stored + block]);
        }
        assert_eq!(rebuilt, iso);

        let disc = icon_iso();
        assert_eq!(disc.len(), images["isoWithIcon"]["length"]);
        assert_eq!(sha256_hex(&disc), images["isoWithIcon"]["sha256"]);
        assert_eq!(disc, iso_with_icon(&tpl(5, 8, 8)));
        assert_eq!(word_at(&disc, ICON_SIZE_FIELD), 192);
        assert_eq!(&disc[ICON_DATA_AT..ICON_DATA_AT + 192], tpl(5, 8, 8));
    }

    #[test]
    fn the_tpls_are_the_icon_tests_builders() {
        let vectors = icon_vectors();
        let tpls = &vectors["tpls"];
        for name in BUILT_TPLS {
            let listed = &tpls[name];
            let encoding = u32::try_from(listed["encoding"].as_u64().unwrap()).unwrap();
            let width = u16::try_from(listed["width"].as_u64().unwrap()).unwrap();
            let height = u16::try_from(listed["height"].as_u64().unwrap()).unwrap();
            let built = tpl(encoding, width, height);
            assert_eq!(hex(&built), listed["hex"], "{name}");
            assert_eq!(sha256_hex(&built), listed["sha256"], "{name}");
        }
        assert_eq!(tpl(5, 8, 8).len(), 192);
        assert_eq!(tpl(6, 8, 8).len(), 320);
        assert_eq!(hex(&first_frame_only_tpl()), tpls["firstFrameOnly"]["hex"]);
        // Two partial tiles of one row, after the header the second row of the table shares.
        let only = first_frame_only_tpl();
        assert_eq!(only.len(), 64 + 2 * 32);
        assert_eq!(word_at(&only, 4), 2);
        assert_eq!(
            [12, 16, 20, 24].map(|at| word_at(&only, at)),
            [28, 0, 28, 0]
        );
        assert_eq!(&only[28..32], [0, 3, 0, 5]);
    }

    #[test]
    fn edits_apply_in_order() {
        let base = [0_u8; 8];
        assert_eq!(edited(&base, &json!([])), base);
        let edits = json!([
            {"op": "u32be", "at": 0, "value": 0x0102_0304_u32},
            {"op": "byte", "at": 1, "value": 0xFF},
            {"op": "bytes", "at": 3, "hex": "aabbcc"},
            {"op": "append", "hex": "dd"},
            {"op": "appendZeros", "count": 3},
            {"op": "byte", "at": 11, "value": 0xEE},
            {"op": "truncate", "to": 10},
            {"op": "dropLast", "count": 1},
        ]);
        assert_eq!(
            edited(&base, &edits),
            [0x01, 0xFF, 0x03, 0xAA, 0xBB, 0xCC, 0, 0, 0xDD]
        );
        // Each edit acts on what the edits before it left.
        let undone = json!([{"op": "dropLast", "count": 8}, {"op": "append", "hex": "01"}]);
        assert_eq!(edited(&base, &undone), [0x01]);
        // The edges of the bytes are reached, never passed.
        let edges = json!([
            {"op": "u32be", "at": 4, "value": 0xFFFF_FFFF_u32},
            {"op": "byte", "at": 0, "value": 7},
            {"op": "bytes", "at": 8, "hex": ""},
            {"op": "truncate", "to": 8},
        ]);
        assert_eq!(edited(&base, &edges), [7, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF]);
        assert!(edited(&base, &json!([{"op": "dropLast", "count": 8}])).is_empty());

        for refused in [
            json!([{"op": "u32be", "at": 5, "value": 1}]),
            json!([{"op": "u32be", "at": 0, "value": 0x1_0000_0000_u64}]),
            json!([{"op": "byte", "at": 8, "value": 1}]),
            json!([{"op": "byte", "at": 0, "value": 256}]),
            json!([{"op": "bytes", "at": 7, "hex": "0102"}]),
            json!([{"op": "bytes", "at": 0, "hex": "123"}]),
            json!([{"op": "truncate", "to": 9}]),
            json!([{"op": "dropLast", "count": 9}]),
            json!([{"op": "append", "hex": "00"}, {"op": "byte", "at": 9, "value": 1}]),
            json!([{"op": "xor", "at": 0, "value": 1}]),
            json!([{"at": 0, "value": 1}]),
            json!({"op": "byte", "at": 0, "value": 1}),
        ] {
            assert!(panics(|| edited(&base, &refused)), "{refused}");
        }
    }

    #[test]
    fn the_vectors_cases_edit_their_bases() {
        let gamecube = gamecube_vectors();
        let iso = synthetic_iso();
        let ciso = synthetic_ciso(&iso);
        let mut discs = BTreeMap::new();
        for case in gamecube["cases"].as_array().unwrap() {
            let base: &[u8] = match case["base"].as_str().unwrap() {
                "iso" => &iso,
                "ciso" => &ciso,
                "empty" => &[],
                other => panic!("no disc is called {other}"),
            };
            let name = case["name"].as_str().unwrap().to_owned();
            discs.insert(name, edited(base, &case["edits"]));
        }
        assert_eq!(discs.len(), gamecube["cases"].as_array().unwrap().len());
        assert!(discs["empty"].is_empty());
        assert_eq!(discs["cisoZeros"].len(), 4 + CISO_HEADER_LEN);
        assert_eq!(discs["pspCso"].len(), CISO_HEADER_LEN);
        assert_eq!(discs["name"][0x86D], b'/');
        assert_eq!(word_at(&discs["fst"], 0x424), 0xFFFF_FF00);
        assert_eq!(&discs["alphaName"][0x8AC..0x8BE], b"installation.json\0");
        assert_eq!(word_at(&discs["alphaName"], 0x428), 190);
        assert_eq!(discs["trimmedToLastByte"].len(), ICON_DATA_AT + 4);
        assert_eq!(discs["trimmedOneShort"].len(), ICON_DATA_AT + 3);
        assert_eq!(discs["cisoMap"][8], 2);
        assert_eq!(discs["cisoTruncated"].len(), ciso.len() - 1);
        assert_eq!(discs["cisoTrailing"].len(), ciso.len() + 1);
        assert_eq!(discs["revision0"][..7], iso[..7]);
        assert_eq!(discs["revision0"][7], 0);

        let icon = icon_vectors();
        for case in icon["cases"].as_array().unwrap() {
            let base = match case["base"].as_str().unwrap() {
                "empty" => Vec::new(),
                name => unhex(&icon["tpls"][name]["hex"]),
            };
            let bytes = edited(&base, &case["edits"]);
            match case["name"].as_str().unwrap() {
                "empty" => assert!(bytes.is_empty()),
                "short" => assert_eq!(bytes, base[..base.len() - 1]),
                "tooLarge" => {
                    assert_eq!(bytes.len(), (1 << 20) + 1);
                    assert!(bytes.iter().all(|&byte| byte == 0));
                }
                "pixels" => assert_eq!(word_at(&bytes, 28), 0xFFFF_FF00),
                _ => assert_eq!(bytes.len(), base.len(), "{case}"),
            }
        }
    }

    #[test]
    fn file_tables_and_images_are_laid_out_as_the_installer_reads_them() {
        // An empty table is the root's row and its empty name.
        assert_eq!(
            hex(&fst(&[])),
            ["01000000", "00000000", "00000001", "00"].concat()
        );
        let table = fst(&[FstEntry::folder("A", 0, 3), FstEntry::file("bc", 0x1234, 5)]);
        // The root's row, the folder's (its name at 1), the file's (its name at 3), then the
        // names: the root's empty one, `A` and `bc`.
        assert_eq!(
            hex(&table),
            [
                "010000000000000000000003",
                "010000010000000000000003",
                "000000030000123400000005",
                "004100626300",
            ]
            .concat()
        );
        assert_eq!(
            FstEntry::folder("A", 0, 3),
            FstEntry {
                name: "A".to_owned(),
                folder: true,
                offset: 0,
                size: 3,
            }
        );

        let image = iso_of(&table, &[(0x1234, &b"hello"[..])], 0x1300);
        assert_eq!(image.len(), 0x1300);
        assert_eq!(&image[..8], b"GUNE5D\0\x01");
        assert_eq!(word_at(&image, 0x424), 0x800);
        assert_eq!(word_at(&image, 0x428), table.len() as u32);
        assert_eq!(&image[FST_AT..FST_AT + table.len()], table);
        assert_eq!(&image[0x1234..0x1239], b"hello");
        let written = [
            0..8,
            0x1C..0x20,
            0x424..0x42C,
            FST_AT..FST_AT + table.len(),
            0x1234..0x1239,
        ];
        assert!(
            (0..image.len())
                .filter(|at| !written.iter().any(|range| range.contains(at)))
                .all(|at| image[at] == 0)
        );
        // What does not fit is refused, never cut short.
        assert!(panics(|| iso_of(&table, &[], FST_AT + table.len() - 1)));
        assert!(panics(|| iso_of(&table, &[(0x12FF, &b"ab"[..])], 0x1300)));
        assert_eq!(
            iso_of(&table, &[(0x12FE, &b"ab"[..])], 0x1300)[0x12FF],
            b'b'
        );

        // An icon replaces as much of the stand-in as it covers, and one that runs past the disc's
        // end makes the disc longer.
        let short = iso_with_icon(b"xy");
        assert_eq!(short.len(), SYNTHETIC_ISO_LEN);
        assert_eq!(&short[ICON_DATA_AT..ICON_DATA_AT + 4], b"xyon");
        assert_eq!(word_at(&short, ICON_SIZE_FIELD), 2);
        let long = vec![0xAB; 0x2000];
        let grown = iso_with_icon(&long);
        assert_eq!(grown.len(), ICON_DATA_AT + long.len());
        assert_eq!(grown[ICON_DATA_AT..], long);
        let mut before = synthetic_iso();
        before[ICON_SIZE_FIELD..ICON_SIZE_FIELD + 4].copy_from_slice(&0x2000_u32.to_be_bytes());
        assert!(
            grown[..ICON_DATA_AT] == before[..ICON_DATA_AT],
            "the disc before the icon is the synthetic disc's, its icon's length aside"
        );

        // A CISO keeps no partial block, and refuses a kept block past its map.
        let mut ragged = vec![0; 3 * 512 + 100];
        ragged[600] = 1;
        ragged[3 * 512 + 10] = 1;
        let ciso = synthetic_ciso(&ragged);
        assert_eq!(ciso.len(), CISO_HEADER_LEN + 512);
        assert_eq!(&ciso[8..12], [0, 1, 0, 0]);
        let mut past = vec![0; (CISO_HEADER_LEN - 8 + 1) * 512];
        let last = past.len() - 1;
        past[last] = 1;
        assert!(panics(|| synthetic_ciso(&past)));
    }
}
