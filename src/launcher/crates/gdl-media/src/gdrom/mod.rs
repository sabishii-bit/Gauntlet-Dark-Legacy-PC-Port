//! The Dreamcast GD-ROM as the Dreamcast extractor reads it: the sheet that lists the dump's
//! track files, and the raw sectors those files keep, each placed by the address its own header
//! gives and guarded by the error detection code it keeps after its user data.

use gdl_files::NameRule;

use crate::{DiscRule, MediaError};

mod cue;
mod edc;
mod sector;
mod sheet;
mod sheet_text;

pub use edc::{EDC_FIELD, EDC_POLYNOMIAL, EDC_SPAN, edc};
pub use sector::{
    FRAMES_PER_SECOND, HEADER_LEN, LBA_BIAS, MAX_CHUNK_SECTORS, RAW_SECTOR, SECONDS_PER_MINUTE,
    SECTOR, SYNC, USER_DATA, sector_lba,
};
pub use sheet::{MAX_SHEET_LEN, Sheet, SheetKind, SheetTrack, TrackMode, parse_sheet, read_sheet};
pub use sheet_text::{ascii_field, lines, strip};

/// The most characters of a name or a line taken from a dump that its refusal quotes: more than
/// the longest name the extracted tree may hold, so that a name refused for its length is shown
/// whole well past that bound, while text that runs on is shown by its length and its start
/// alone.
const MAX_QUOTED_CHARS: usize = 256;

/// What the Dreamcast extractor begins its answer with for a name the tree may not hold.
const UNSAFE_NAME: &str = "Unsafe disc name";

/// The refusal by `rule` of `name`, a name taken from the dump that breaks the rule of names
/// `broken`: the name quoted whole, or past [`MAX_QUOTED_CHARS`] characters its length and its
/// start, and then the rule it breaks.
fn unsafe_name(rule: DiscRule, name: &str, broken: NameRule) -> MediaError {
    let detail = match long_start(name) {
        Some((len, start)) => {
            format!("{UNSAFE_NAME} of {len} characters beginning {start:?} ({broken})")
        }
        None => format!("{UNSAFE_NAME}: {name:?} ({broken})"),
    };
    MediaError::disc(rule, detail)
}

/// The length of `text` in characters and its first [`MAX_QUOTED_CHARS`] characters, when it has
/// more than that.
fn long_start(text: &str) -> Option<(usize, &str)> {
    let (end, _) = text.char_indices().nth(MAX_QUOTED_CHARS)?;
    Some((text.chars().count(), &text[..end]))
}
