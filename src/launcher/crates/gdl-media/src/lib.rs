//! Read-only readers of the player's dumps: the GameCube ISO and CISO with its file table, TPL
//! icons, the N64 ROM with its ROMDISK and overlays, and the Dreamcast GD-ROM (CUE and GDI) with
//! ISO 9660. A dump is only ever opened for reading.
#![forbid(unsafe_code)]

mod digest;
mod error;
mod identify;
mod n64;
mod source_name;

pub use digest::{Sha1Digest, Sha256Digest};
pub use error::{MediaError, RomRule};
pub use identify::Region;
pub use n64::{ByteOrder, MAX_ROM_LEN, N64Rom, ROM_HEADER_LEN, RomHeader, US_SHA1};
pub use source_name::SourceName;
