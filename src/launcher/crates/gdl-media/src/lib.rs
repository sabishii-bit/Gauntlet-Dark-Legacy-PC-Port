//! Read-only readers of the player's dumps: the GameCube ISO and CISO with its file table, TPL
//! icons, the N64 ROM with its ROMDISK and overlays, and the Dreamcast GD-ROM (CUE and GDI) with
//! ISO 9660. A dump is only ever opened for reading.
#![forbid(unsafe_code)]
