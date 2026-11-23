//! The sparse CISO container a GameCube disc image may come in, as the installer reads one: a
//! header of 32 KiB holding the mark, the size of a block and a map of a byte a block, then the
//! blocks the map keeps, in their order. A block the map leaves out holds only zeros.

use std::fmt;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use super::{DISC_SIZE, fill, outside_image};
use crate::{GameCubeRule, MediaError};

/// What a CISO begins with.
pub(crate) const CISO_MARK: [u8; 4] = *b"CISO";

/// The length of a CISO's header: its mark, its block size and its map of blocks.
pub(crate) const CISO_HEADER_LEN: usize = 0x8000;

/// Where the header gives the size of a block, a little-endian word.
pub(crate) const CISO_BLOCK_SIZE_AT: usize = 4;

/// Where the header's map of blocks starts: a byte a block of the disc, to its end.
pub(crate) const CISO_MAP_AT: usize = 8;

/// The smallest block a GameCube CISO keeps. A PSP's CSO, which shares the mark, gives the length
/// of its header in that place, which is smaller.
pub(crate) const MIN_CISO_BLOCK: u32 = 512;

/// The largest block a GameCube CISO keeps.
pub(crate) const MAX_CISO_BLOCK: u32 = 16 << 20;

/// The map's flag of a block the CISO leaves out.
const ABSENT: u8 = 0;

/// The map's flag of a block the CISO keeps.
const KEPT: u8 = 1;

/// What the installer answers for a file that begins as a CISO but is shorter than its header.
const TRUNCATED_HEADER: &str = "Truncated CISO header";

/// What it answers for a block size no GameCube CISO keeps.
const UNSUPPORTED_BLOCK_SIZE: &str = "Unsupported GameCube CISO block size (not PSP CSO)";

/// What it answers for a map that flags a block with neither 0 nor 1.
const INVALID_MAP: &str = "Invalid CISO block map";

/// What it answers for a CISO that ends before the blocks its map keeps.
const TRUNCATED_DATA: &str = "Truncated CISO data";

/// A CISO's size of a block, and where it keeps each block of the disc.
pub(crate) struct Ciso {
    /// How many bytes of the disc a block holds.
    block: u32,
    /// For each block of the disc, in its order, how many kept blocks come before it in the file
    /// when the CISO keeps it, or `None` when it leaves it out.
    places: Box<[Option<u16>]>,
}

impl Ciso {
    /// The header of the CISO `source`, `len` bytes long, read and checked as the installer
    /// checks one: whole, with a block size of a GameCube CISO, a map of 0 and 1 alone, and room
    /// in the file for every block the map keeps. Bytes after the last block are passed over.
    pub(crate) fn read<R: Read + Seek>(
        source: &mut R,
        len: u64,
        path: &Path,
    ) -> Result<Ciso, MediaError> {
        let failed = |error| MediaError::io(path, error);
        // A file shorter than the header is told by its length, so that no read reaches past it.
        if len < CISO_HEADER_LEN as u64 {
            return Err(MediaError::gamecube(
                GameCubeRule::CisoHeader,
                TRUNCATED_HEADER,
            ));
        }
        let mut header = vec![0; CISO_HEADER_LEN];
        source.seek(SeekFrom::Start(0)).map_err(failed)?;
        if fill(source, &mut header).map_err(failed)? != CISO_HEADER_LEN {
            return Err(MediaError::gamecube(
                GameCubeRule::CisoHeader,
                TRUNCATED_HEADER,
            ));
        }
        let block = header
            .get(CISO_BLOCK_SIZE_AT..)
            .and_then(<[u8]>::first_chunk)
            .map_or(0, |word| u32::from_le_bytes(*word));
        if !(MIN_CISO_BLOCK..=MAX_CISO_BLOCK).contains(&block) {
            let refused = MediaError::gamecube(GameCubeRule::CisoBlockSize, UNSUPPORTED_BLOCK_SIZE);
            return Err(refused);
        }
        let mut places = Vec::with_capacity(CISO_HEADER_LEN - CISO_MAP_AT);
        let mut kept: u16 = 0;
        for &flag in &header[CISO_MAP_AT..] {
            let place = match flag {
                ABSENT => None,
                KEPT => {
                    let place = kept;
                    kept += 1;
                    Some(place)
                }
                _ => return Err(MediaError::gamecube(GameCubeRule::CisoMap, INVALID_MAP)),
            };
            places.push(place);
        }
        let data_end = CISO_HEADER_LEN as u64 + u64::from(kept) * u64::from(block);
        if data_end > len {
            return Err(MediaError::gamecube(GameCubeRule::CisoData, TRUNCATED_DATA));
        }
        Ok(Ciso {
            block,
            places: places.into_boxed_slice(),
        })
    }

    /// How many bytes of the disc the map covers, never more than a whole disc holds.
    pub(crate) fn disc_len(&self) -> u64 {
        (self.places.len() as u64 * u64::from(self.block)).min(DISC_SIZE)
    }

    /// Fills `buffer` with the disc's bytes from `offset` on, a block at a time: a block the CISO
    /// leaves out gives zeros, and one it keeps is read from `source`, the file at `path`. The
    /// caller holds every read within [`Ciso::disc_len`]. A file that ends before a kept block's
    /// bytes is refused, as one cut short while it is read is.
    pub(crate) fn read_at<R: Read + Seek>(
        &self,
        source: &mut R,
        path: &Path,
        offset: u64,
        buffer: &mut [u8],
    ) -> Result<(), MediaError> {
        let block = u64::from(self.block);
        let mut at = offset;
        let mut rest = buffer;
        while !rest.is_empty() {
            let within = at % block;
            let count = rest
                .len()
                .min(usize::try_from(block - within).unwrap_or(usize::MAX));
            let (part, after) = rest.split_at_mut(count);
            let index = usize::try_from(at / block).unwrap_or(usize::MAX);
            // The disc's length bounds every read, so a block past the map is never asked for.
            match self.places.get(index).copied().ok_or_else(outside_image)? {
                None => part.fill(0),
                Some(place) => {
                    let from = CISO_HEADER_LEN as u64 + u64::from(place) * block + within;
                    source
                        .seek(SeekFrom::Start(from))
                        .map_err(|error| MediaError::io(path, error))?;
                    let read = fill(source, part).map_err(|error| MediaError::io(path, error))?;
                    if read != count {
                        return Err(MediaError::gamecube(GameCubeRule::CisoData, TRUNCATED_DATA));
                    }
                }
            }
            at += count as u64;
            rest = after;
        }
        Ok(())
    }
}

impl fmt::Debug for Ciso {
    /// The block size and how many blocks the CISO keeps, without its map.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let kept = self.places.iter().flatten().count();
        formatter
            .debug_struct("Ciso")
            .field("block", &self.block)
            .field("kept", &kept)
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use std::io::Cursor;
    use std::path::Path;

    use super::Ciso;
    use crate::DISC_SIZE;

    /// The length of a CISO's header. The format's facts are given here apart from the reader's
    /// own constants, so that a reader that looks elsewhere is caught.
    const FORMAT_HEADER_LEN: usize = 0x8000;

    /// How many blocks a CISO's map flags: a byte a block, after the mark and the block size.
    const FORMAT_MAP_LEN: usize = FORMAT_HEADER_LEN - 8;

    /// A CISO's header of blocks of `block` bytes, keeping the blocks `kept` flags, then `data`.
    fn ciso(block: u32, kept: &[usize], data: &[u8]) -> Vec<u8> {
        let mut bytes = vec![0; FORMAT_HEADER_LEN];
        bytes[..4].copy_from_slice(b"CISO");
        bytes[4..8].copy_from_slice(&block.to_le_bytes());
        for &index in kept {
            bytes[8 + index] = 1;
        }
        bytes.extend_from_slice(data);
        bytes
    }

    /// The CISO `bytes`, read.
    fn read(bytes: &[u8]) -> Ciso {
        Ciso::read(
            &mut Cursor::new(bytes),
            bytes.len() as u64,
            Path::new("disc.ciso"),
        )
        .unwrap()
    }

    #[test]
    fn each_kept_block_lies_after_those_kept_before_it() {
        // Blocks 1 and 3 kept, of 512 bytes each: block 1 is the first in the file, block 3 the
        // second, and blocks 0, 2 and 4 read as zeros.
        let data = [vec![0x11; 512], vec![0x33; 512]].concat();
        let bytes = ciso(512, &[1, 3], &data);
        let disc = read(&bytes);
        assert_eq!(format!("{disc:?}"), "Ciso { block: 512, kept: 2 }");
        let mut buffer = vec![0xEE; 5 * 512];
        let mut source = Cursor::new(bytes.as_slice());
        disc.read_at(&mut source, Path::new("disc.ciso"), 0, &mut buffer)
            .unwrap();
        let expected = [
            vec![0; 512],
            vec![0x11; 512],
            vec![0; 512],
            vec![0x33; 512],
            vec![0; 512],
        ]
        .concat();
        assert_eq!(buffer, expected);
        // A read across a block's end takes each part from its own block.
        let mut across = [0xEE; 4];
        disc.read_at(&mut source, Path::new("disc.ciso"), 1022, &mut across)
            .unwrap();
        assert_eq!(across, [0x11, 0x11, 0, 0]);
        // Bytes after the last kept block are passed over.
        let mut trailing = bytes.clone();
        trailing.extend_from_slice(&[0x55; 100]);
        assert_eq!(read(&trailing).disc_len(), read(&bytes).disc_len());
    }

    #[test]
    fn the_disc_a_ciso_holds_is_bounded_by_a_whole_disc() {
        // The map covers its blocks' bytes, up to the length of a whole disc.
        assert_eq!(
            read(&ciso(512, &[], &[])).disc_len(),
            512 * FORMAT_MAP_LEN as u64
        );
        assert_eq!(read(&ciso(16 << 20, &[], &[])).disc_len(), DISC_SIZE);
        assert_eq!(DISC_SIZE, 1_459_978_240);
    }
}
