//! The install folder: its receipt, payload policy, embedded sections, lock and probes; installing
//! the game, updating it and the launcher, recovering interrupted work, the expansions, the disc
//! icon and launching. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]
