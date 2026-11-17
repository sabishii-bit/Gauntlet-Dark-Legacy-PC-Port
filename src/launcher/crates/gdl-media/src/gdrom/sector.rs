//! A raw sector as a GD-ROM's track files keep it, in MODE1/2352: twelve bytes of sync, a header
//! that gives the sector's own address, the minute, second and frame of the disc's time in BCD,
//! and its mode, then 2,048 bytes of user data and the codes that guard them. The Dreamcast
//! extractor places every sector by the address its header gives, never by where its track file
//! keeps it, and so does the reader.

use std::ops::Range;

use crate::{DiscRule, MediaError};

/// The length of a raw sector as a track file keeps it: its sync, header, user data and codes.
pub const RAW_SECTOR: usize = 2352;

/// The length of a sector's user data, the blocks the disc's file system is made of.
pub const SECTOR: usize = 2048;

/// The length of a raw sector's sync and header, which give its address and mode.
pub const HEADER_LEN: usize = 16;

/// The twelve bytes a raw sector begins with.
pub const SYNC: [u8; 12] = [
    0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
];

/// Where a raw sector keeps its user data: the 2,048 bytes right after its header.
pub const USER_DATA: Range<usize> = HEADER_LEN..HEADER_LEN + SECTOR;

/// The most sectors a read takes from a track file at once, as the Dreamcast extractor reads an
/// extent: 512 raw sectors, a little over a MiB.
pub const MAX_CHUNK_SECTORS: u64 = 512;

/// The frames a sector's address counts before LBA 0: the two seconds of pregap the disc's time
/// begins with.
pub const LBA_BIAS: i64 = 150;

/// The seconds of a minute of the disc's time.
pub const SECONDS_PER_MINUTE: i64 = 60;

/// The frames of a second of the disc's time, a sector each.
pub const FRAMES_PER_SECOND: i64 = 75;

/// The mode of a data sector: 2,048 bytes of user data, then the codes that guard them.
const MODE1: u8 = 1;

/// How many bits a digit of BCD takes: a byte's low four hold its units, its high four its tens.
const DIGIT_BITS: u32 = 4;

/// The bits of a byte of BCD that hold its units.
const UNITS_MASK: u8 = 0x0F;

/// The most a decimal digit may be.
const MOST_DIGIT: u8 = 9;

/// The base the digits of BCD count in.
const DIGIT_BASE: i64 = 10;

/// What the Dreamcast extractor answers for bytes that do not begin with the sync and a header of
/// mode 1.
const NOT_MODE1_HEADER: &str = "Expected a MODE1/2352 sector header";

/// What it answers for an address whose digits are not BCD.
const NOT_BCD: &str = "Invalid BCD sector address";

/// What it answers for an address whose second or frame is past the last.
const INVALID_TIME: &str = "Invalid sector time";

/// The LBA of the raw sector whose first bytes `header` holds, read from its address as the
/// Dreamcast extractor reads it: the minute, second and frame of the disc's time, in BCD, less the
/// [`LBA_BIAS`] frames before LBA 0, so that the first addresses give negative LBAs. Refused unless
/// `header` begins with the sync and a header of mode 1, then unless every digit of the address is
/// decimal but the minute's tens, which count past nine on a disc of more than 100 minutes, as a
/// GD-ROM's high-density area is, and then unless the second is under 60 and the frame under 75.
/// Only the first [`HEADER_LEN`] bytes are read, so a whole raw sector may be given.
pub fn sector_lba(header: &[u8]) -> Result<i64, MediaError> {
    let Some(&[sync @ .., minute, second, frame, mode]) = header.first_chunk::<HEADER_LEN>() else {
        return Err(MediaError::disc(DiscRule::SectorHeader, NOT_MODE1_HEADER));
    };
    if sync != SYNC || mode != MODE1 {
        return Err(MediaError::disc(DiscRule::SectorHeader, NOT_MODE1_HEADER));
    }
    let units_decimal = [minute, second, frame]
        .into_iter()
        .all(|byte| units(byte) <= MOST_DIGIT);
    let tens_decimal = [second, frame]
        .into_iter()
        .all(|byte| tens(byte) <= MOST_DIGIT);
    if !units_decimal || !tens_decimal {
        return Err(MediaError::disc(DiscRule::SectorBcd, NOT_BCD));
    }
    let [minute, second, frame] = [minute, second, frame].map(value);
    if second >= SECONDS_PER_MINUTE || frame >= FRAMES_PER_SECOND {
        return Err(MediaError::disc(DiscRule::SectorTime, INVALID_TIME));
    }
    Ok((minute * SECONDS_PER_MINUTE + second) * FRAMES_PER_SECOND + frame - LBA_BIAS)
}

/// The units digit of a byte of BCD.
fn units(byte: u8) -> u8 {
    byte & UNITS_MASK
}

/// The tens digit of a byte of BCD, which in a minute may count past nine.
fn tens(byte: u8) -> u8 {
    byte >> DIGIT_BITS
}

/// The number a byte of BCD gives, its tens taken as they stand, even past nine.
fn value(byte: u8) -> i64 {
    i64::from(tens(byte)) * DIGIT_BASE + i64::from(units(byte))
}

#[cfg(test)]
mod tests {
    use std::ops::Range;

    use gdl_fixtures::{Edc, raw_sector, unhex};

    use super::{
        FRAMES_PER_SECOND, HEADER_LEN, LBA_BIAS, MAX_CHUNK_SECTORS, RAW_SECTOR, SECONDS_PER_MINUTE,
        SECTOR, SYNC, USER_DATA, sector_lba,
    };
    use crate::MediaError;
    use crate::vectors::{dreamcast_vectors, edc_payload, text};

    /// The twelve bytes the format begins a raw sector with. The format's facts are given here
    /// apart from the reader's own constants, so that a reader that looks elsewhere is caught.
    const FORMAT_SYNC: [u8; 12] = [
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    ];

    /// How many bytes the format gives a raw sector's sync and header.
    const FORMAT_HEADER_LEN: usize = 16;

    /// Where the format keeps a raw sector's user data.
    const FORMAT_USER_DATA: Range<usize> = 16..2064;

    /// The mode the format gives a data sector.
    const FORMAT_MODE1: u8 = 1;

    /// The seconds of a minute of the disc's time.
    const FORMAT_SECONDS: i64 = 60;

    /// The frames of a second of it.
    const FORMAT_FRAMES: i64 = 75;

    /// The frames it counts before LBA 0.
    const FORMAT_BIAS: i64 = 150;

    /// The most a decimal digit may be.
    const FORMAT_MOST_DIGIT: u8 = 9;

    /// The address 00:02:00, the one LBA 0 has, as a header writes it.
    const LBA_ZERO: [u8; 3] = [0x00, 0x02, 0x00];

    /// What the extractor answers for bytes that do not begin with the sync and a header of mode 1.
    const NOT_MODE1: &str = "Expected a MODE1/2352 sector header";

    /// What it answers for an address that is not BCD.
    const NOT_BCD: &str = "Invalid BCD sector address";

    /// What it answers for an address whose second or frame is past the last.
    const INVALID_TIME: &str = "Invalid sector time";

    /// The payload, of a whole sector's user data, that the vectors' EDC cases name so.
    const WHOLE_PAYLOAD: &str = "bytes0to255x8";

    /// What the reader gives a sector's first bytes, as the vectors write it: the LBA, or the rule
    /// and the words it refuses them with.
    type Outcome = Result<i64, (&'static str, String)>;

    /// The first bytes of a raw sector: the format's sync, then `address`, the minute, second and
    /// frame as the header writes them, then `mode`.
    fn header(address: [u8; 3], mode: u8) -> Vec<u8> {
        let mut bytes = FORMAT_SYNC.to_vec();
        bytes.extend_from_slice(&address);
        bytes.push(mode);
        bytes
    }

    /// What the reader gives `bytes`: by a rule of the disc's format when it refuses them.
    fn outcome(bytes: &[u8]) -> Outcome {
        sector_lba(bytes).map_err(|error| {
            assert!(matches!(error, MediaError::Disc { .. }), "{error:?}");
            (error.rule(), error.to_string())
        })
    }

    /// The refusal by `rule`, in the extractor's `words` for it.
    fn refusal(rule: &'static str, words: &str) -> Outcome {
        Err((rule, words.to_owned()))
    }

    /// A digit of BCD as the format reads it: the units of a byte, or its tens.
    fn digits(byte: u8) -> (u8, u8) {
        (byte % 16, byte / 16)
    }

    #[test]
    fn addresses_follow_the_shared_vectors() {
        let vectors = dreamcast_vectors();
        let cases = vectors["sectors"].as_array().unwrap();
        assert!(!cases.is_empty());
        let (mut rules, mut lbas) = (Vec::new(), Vec::new());
        for case in cases {
            // The launcher reads every address as the extractor does.
            assert!(case.get("deviation").is_none(), "{case}");
            let bytes = unhex(&case["header"]);
            match (outcome(&bytes), case["rule"].as_str()) {
                (Ok(lba), None) => {
                    assert!(case["python"].is_null(), "{case}");
                    assert_eq!(case["lba"], lba, "{case}");
                    lbas.push(lba);
                }
                (Err((rule, detail)), Some(expected)) => {
                    assert_eq!(rule, expected, "{case}");
                    assert!(case["lba"].is_null(), "{case}");
                    // The extractor's own words, which are fixed.
                    assert_eq!(detail, text(&case["python"]), "{case}");
                    rules.push(rule);
                }
                (given, rule) => panic!("{case}: {given:?} where {rule:?} was expected"),
            }
        }
        rules.sort_unstable();
        rules.dedup();
        assert_eq!(rules, ["sectorBcd", "sectorHeader", "sectorTime"]);
        // Among the addresses taken: the first, before LBA 0; LBA 0; the high-density area's start;
        // and the last a header holds, its minute's tens at fifteen.
        for lba in [-150, 0, 45_000, 719_849] {
            assert!(lbas.contains(&lba), "{lba} among {lbas:?}");
        }
    }

    #[test]
    fn each_digit_is_decimal_but_the_minutes_tens() {
        // Every value of each byte of the address, the other two those of LBA 0.
        for place in 0..LBA_ZERO.len() {
            for byte in 0..=u8::MAX {
                let mut address = LBA_ZERO;
                address[place] = byte;
                let decimal = address.iter().enumerate().all(|(at, &byte)| {
                    let (units, tens) = digits(byte);
                    // The minute, first, counts its tens past nine.
                    units <= FORMAT_MOST_DIGIT && (at == 0 || tens <= FORMAT_MOST_DIGIT)
                });
                let [minute, second, frame] = address.map(|byte| {
                    let (units, tens) = digits(byte);
                    i64::from(tens) * 10 + i64::from(units)
                });
                let expected = if !decimal {
                    refusal("sectorBcd", NOT_BCD)
                } else if second >= FORMAT_SECONDS || frame >= FORMAT_FRAMES {
                    refusal("sectorTime", INVALID_TIME)
                } else {
                    Ok((minute * FORMAT_SECONDS + second) * FORMAT_FRAMES + frame - FORMAT_BIAS)
                };
                let given = outcome(&header(address, FORMAT_MODE1));
                assert_eq!(given, expected, "{place} {byte:#04x}");
            }
        }

        // The digits are judged before the time: a second of 0x6A is not BCD, though read as 70 it
        // would be past the last too, and neither is one of 0xA0, which would read as 100.
        for second in [0x6A, 0xA0] {
            let given = outcome(&header([0x00, second, 0x00], FORMAT_MODE1));
            assert_eq!(given, refusal("sectorBcd", NOT_BCD), "{second:#04x}");
        }
        // The last second and frame of a minute, and the first past them.
        let last = outcome(&header([0x00, 0x59, 0x74], FORMAT_MODE1));
        assert_eq!(last, Ok(59 * 75 + 74 - 150));
        for address in [[0x00, 0x60, 0x00], [0x00, 0x00, 0x75], [0x00, 0x99, 0x99]] {
            let given = outcome(&header(address, FORMAT_MODE1));
            assert_eq!(given, refusal("sectorTime", INVALID_TIME), "{address:x?}");
        }
        // A minute past 99: its tens count on from nine, to fifteen at most.
        let minutes = [(0x99, 99), (0xA0, 100), (0xC2, 122), (0xF9, 159)];
        for (byte, minute) in minutes {
            let given = outcome(&header([byte, 0x00, 0x00], FORMAT_MODE1));
            assert_eq!(given, Ok(minute * 60 * 75 - 150), "{byte:#04x}");
        }
    }

    #[test]
    fn only_the_sync_and_mode_1_begin_a_sector() {
        let whole = header(LBA_ZERO, FORMAT_MODE1);
        assert_eq!(whole.len(), FORMAT_HEADER_LEN);
        assert_eq!(outcome(&whole), Ok(0));
        let not_mode1 = refusal("sectorHeader", NOT_MODE1);

        // Every bit of the sync is part of it.
        for at in 0..FORMAT_SYNC.len() {
            for bit in 0..u8::BITS {
                let mut bytes = whole.clone();
                bytes[at] ^= 1 << bit;
                assert_eq!(outcome(&bytes), not_mode1, "{at} {bit}");
            }
        }
        // A sector of any mode but 1, the mode of data, holds no user data the reader takes.
        for mode in (0..=u8::MAX).filter(|&mode| mode != FORMAT_MODE1) {
            let given = outcome(&header(LBA_ZERO, mode));
            assert_eq!(given, not_mode1, "{mode}");
        }
        // Fewer bytes than a header, however they begin.
        for len in 0..FORMAT_HEADER_LEN {
            assert_eq!(outcome(&whole[..len]), not_mode1, "{len}");
        }
        // More are taken, and only the header is read: whatever follows it.
        for follows in [0x00, 0xFF] {
            let mut longer = whole.clone();
            longer.resize(RAW_SECTOR, follows);
            assert_eq!(outcome(&longer), Ok(0), "{follows:#04x}");
        }

        // The header is judged before its address: an address neither BCD nor a time, after a
        // damaged sync or another mode, is refused for the header.
        let mut damaged = header([0xFA, 0xFA, 0xFA], FORMAT_MODE1);
        damaged[1] = 0;
        assert_eq!(outcome(&damaged), not_mode1);
        assert_eq!(outcome(&header([0xFA, 0x99, 0x99], 2)), not_mode1);
    }

    #[test]
    fn a_built_sector_gives_back_its_lba_and_keeps_its_payload_in_its_user_data() {
        let vectors = dreamcast_vectors();
        let payload = edc_payload(&vectors, WHOLE_PAYLOAD);
        assert_eq!(payload.len(), FORMAT_USER_DATA.len());
        // LBA 0, the last frame of a second and the first of the next, the last of a minute and the
        // first of the next, the last before the high-density area and its first, the first of
        // minute 100, the extractor's tests' own, the high-density area's last, and the last a
        // header holds.
        let lbas = [
            0, 74, 75, 4_349, 4_350, 44_999, 45_000, 449_850, 547_163, 549_149, 719_849,
        ];
        for lba in lbas {
            for code in [Edc::Zero, Edc::Computed] {
                let sector = raw_sector(lba, &payload, code);
                assert_eq!(sector.len(), RAW_SECTOR);
                assert_eq!(outcome(&sector), Ok(i64::from(lba)), "{lba} {code:?}");
                let given = outcome(&sector[..FORMAT_HEADER_LEN]);
                assert_eq!(given, Ok(i64::from(lba)), "{lba} {code:?}");
                // The payload lies where the format keeps the user data, and the reader looks
                // for it there.
                assert!(sector[FORMAT_USER_DATA] == payload[..], "{lba} {code:?}");
                assert!(sector[USER_DATA] == payload[..], "{lba} {code:?}");
            }
        }

        // The reader's sizes are the format's.
        assert_eq!(SYNC, FORMAT_SYNC);
        assert_eq!(HEADER_LEN, FORMAT_HEADER_LEN);
        assert_eq!(USER_DATA, FORMAT_USER_DATA);
        assert_eq!((RAW_SECTOR, SECTOR), (2352, 2048));
        assert_eq!(
            (SECONDS_PER_MINUTE, FRAMES_PER_SECOND, LBA_BIAS),
            (FORMAT_SECONDS, FORMAT_FRAMES, FORMAT_BIAS)
        );
        // A read takes at most 512 sectors at once, as the extractor's does.
        assert_eq!(MAX_CHUNK_SECTORS, 512);
    }
}
