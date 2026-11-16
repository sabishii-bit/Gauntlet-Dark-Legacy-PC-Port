//! What a dump says of itself, for the reports that describe it: the region of the world it was
//! made for, which an N64 ROM's header names.

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
