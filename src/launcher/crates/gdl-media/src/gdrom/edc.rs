//! The error detection code ECMA-130 gives a raw sector: a CRC of 32 bits over its sync, header and
//! user data, which the sector keeps right after them. An installation's extraction holds every
//! sector it reads to it, so that a sector damaged in the dump is refused; the Dreamcast extractor
//! does not look at it.

use std::ops::Range;

use crate::USER_DATA;

/// The code's polynomial, (x^16 + x^15 + x^2 + 1)(x^16 + x^2 + x + 1), reflected, since the code
/// takes each byte from its lowest bit.
pub const EDC_POLYNOMIAL: u32 = 0xD801_8001;

/// The bytes of a raw sector the code covers: the sync, the header and the user data.
pub const EDC_SPAN: Range<usize> = 0..USER_DATA.end;

/// Where a raw sector keeps the code, little-endian: right after its user data.
pub const EDC_FIELD: Range<usize> = USER_DATA.end..USER_DATA.end + EDC_LEN;

/// How many bytes the code takes.
const EDC_LEN: usize = 4;

/// How many values a byte takes, one entry of the table each.
const BYTE_VALUES: usize = 256;

/// The bits of the code that pick its entry of the table: its lowest byte.
const LOW_BYTE: u32 = 0xFF;

/// The code of each byte by itself, from which [`edc`] runs a byte at a time.
const TABLE: [u32; BYTE_VALUES] = table();

/// The error detection code of ECMA-130 over `bytes`: the CRC of [`EDC_POLYNOMIAL`] from zero,
/// taking each byte from its lowest bit, with no final inversion. A raw sector is intact when the
/// code of its [`EDC_SPAN`] is the one its [`EDC_FIELD`] keeps.
pub fn edc(bytes: &[u8]) -> u32 {
    bytes.iter().fold(0, |code, &byte| {
        let entry = (code ^ u32::from(byte)) & LOW_BYTE;
        TABLE[entry as usize] ^ (code >> u8::BITS)
    })
}

/// The code of each byte by itself, worked out a bit at a time: each set bit that leaves the code
/// at its lowest end brings the polynomial in.
const fn table() -> [u32; BYTE_VALUES] {
    let mut table = [0; BYTE_VALUES];
    let mut byte = 0;
    while byte < BYTE_VALUES {
        let mut code = byte as u32;
        let mut bit = 0;
        while bit < u8::BITS {
            let carried = code & 1 == 1;
            code >>= 1;
            if carried {
                code ^= EDC_POLYNOMIAL;
            }
            bit += 1;
        }
        table[byte] = code;
        byte += 1;
    }
    table
}

#[cfg(test)]
mod tests {
    use std::ops::Range;

    use gdl_fixtures::{Edc, raw_sector};
    use serde_json::Value;

    use super::{EDC_FIELD, EDC_POLYNOMIAL, EDC_SPAN, edc};
    use crate::vectors::{dreamcast_vectors, edc_payload, text, word};
    use crate::{RAW_SECTOR, sector_lba};

    /// What ECMA-130's code gives the nine ASCII digits `123456789`, the value a CRC is known by.
    const CHECK_VALUE: u32 = 0x6EC2_EDC4;

    /// The two factors of the code's polynomial as ECMA-130 gives them, a bit a term from x^0 up:
    /// x^16 + x^15 + x^2 + 1, and x^16 + x^2 + x + 1.
    const FACTORS: [u64; 2] = [
        (1 << 16) | (1 << 15) | (1 << 2) | 1,
        (1 << 16) | (1 << 2) | (1 << 1) | 1,
    ];

    /// The degree of the code's polynomial, the bits the code holds.
    const DEGREE: u32 = 32;

    /// The bytes of a raw sector the format's code covers: the sync, the header and the user data.
    /// The format's places are given here apart from the reader's own, so that a reader that looks
    /// elsewhere is caught.
    const FORMAT_SPAN: Range<usize> = 0..2064;

    /// Where the format keeps the code, little-endian.
    const FORMAT_FIELD: Range<usize> = 2064..2068;

    /// The payload, of a whole sector's user data, that the vectors' EDC cases name so.
    const WHOLE_PAYLOAD: &str = "bytes0to255x8";

    /// A code as the vectors write it: eight lowercase hexadecimal digits.
    fn code(value: &Value) -> u32 {
        let digits = text(value);
        assert_eq!(digits.len(), 8, "{digits}");
        assert_eq!(digits, digits.to_ascii_lowercase());
        u32::from_str_radix(digits, 16).unwrap()
    }

    /// The code a raw sector keeps where the format keeps it.
    fn kept(sector: &[u8]) -> u32 {
        u32::from_le_bytes(sector[FORMAT_FIELD].try_into().unwrap())
    }

    /// The carry-less product of two polynomials over the field of two elements, a bit a term.
    fn product(left: u64, right: u64) -> u64 {
        (0..u64::BITS)
            .filter(|bit| (right >> bit) & 1 == 1)
            .fold(0, |sum, bit| sum ^ (left << bit))
    }

    /// The code over `bytes` worked out a bit at a time, with no table: each bit, from a byte's
    /// lowest, enters the code at its lowest end, and a set bit leaving it there brings the
    /// polynomial in.
    fn bit_by_bit(bytes: &[u8]) -> u32 {
        let mut code = 0_u32;
        for &byte in bytes {
            for bit in 0..u8::BITS {
                let entering = (u32::from(byte) >> bit) & 1;
                let carried = (code ^ entering) & 1 == 1;
                code >>= 1;
                if carried {
                    code ^= EDC_POLYNOMIAL;
                }
            }
        }
        code
    }

    #[test]
    fn the_check_value_and_sector_vectors_hold() {
        let vectors = dreamcast_vectors();
        let cases = &vectors["edc"];
        let check = &cases["check"];
        assert_eq!(text(&check["ascii"]), "123456789");
        assert_eq!(code(&check["edc"]), CHECK_VALUE);
        assert_eq!(edc(b"123456789"), CHECK_VALUE);
        // The code starts from nought, so nothing, and any run of noughts, leaves it there.
        assert_eq!(edc(&[]), 0);
        assert_eq!(edc(&[0; RAW_SECTOR]), 0);

        let sectors = cases["sectors"].as_array().unwrap();
        assert_eq!(sectors.len(), 8);
        let mut payloads = Vec::new();
        for case in sectors {
            let lba = word(&case["lba"]);
            let name = text(&case["payload"]);
            let payload = edc_payload(&vectors, name);
            let expected = code(&case["edc"]);
            // The sector as the extractor's tests build it, its code field nought, and the code
            // over the bytes the format says it covers.
            let built = raw_sector(lba, &payload, Edc::Zero);
            assert_eq!(built.len(), RAW_SECTOR);
            assert_eq!(sector_lba(&built).unwrap(), i64::from(lba), "{case}");
            assert_eq!(edc(&built[FORMAT_SPAN]), expected, "{case}");
            assert_eq!(edc(&built[EDC_SPAN]), expected, "{case}");
            assert_eq!(kept(&built), 0, "{case}");
            // The same sector as a disc keeps it: the code, after the user data, is that of the
            // bytes before it, which it leaves as they were.
            let intact = raw_sector(lba, &payload, Edc::Computed);
            assert_eq!(kept(&intact), expected, "{case}");
            assert!(intact[EDC_FIELD] == expected.to_le_bytes(), "{case}");
            assert!(intact[FORMAT_SPAN] == built[FORMAT_SPAN], "{case}");
            assert_eq!(edc(&intact[EDC_SPAN]), kept(&intact), "{case}");
            payloads.push(name);
        }
        // Sectors of no payload, a whole sector's and a few bytes'.
        payloads.sort_unstable();
        payloads.dedup();
        assert_eq!(payloads, [WHOLE_PAYLOAD, "hello", "zero"]);
        assert_eq!((EDC_SPAN, EDC_FIELD), (FORMAT_SPAN, FORMAT_FIELD));
    }

    #[test]
    fn the_code_is_ecma_130s_polynomial_a_bit_at_a_time() {
        // The polynomial is the product of ECMA-130's two factors, of degree 32, its terms below
        // x^32 taken from the highest down.
        let polynomial = product(FACTORS[0], FACTORS[1]);
        assert_eq!(polynomial >> DEGREE, 1);
        let below = u32::try_from(polynomial & u64::from(u32::MAX)).unwrap();
        assert_eq!(below.reverse_bits(), EDC_POLYNOMIAL);
        assert_eq!(bit_by_bit(b"123456789"), CHECK_VALUE);

        // Every byte by itself, each its own entry of the table, and a run of every byte.
        for byte in 0..=u8::MAX {
            assert_eq!(edc(&[byte]), bit_by_bit(&[byte]), "{byte:#04x}");
        }
        let every: Vec<u8> = (0..=u8::MAX).collect();
        assert_eq!(edc(&every), bit_by_bit(&every));
        // A whole sector, a byte at a time as a bit at a time.
        let payload = edc_payload(&dreamcast_vectors(), WHOLE_PAYLOAD);
        let sector = raw_sector(45_000, &payload, Edc::Zero);
        assert_eq!(edc(&sector[FORMAT_SPAN]), bit_by_bit(&sector[FORMAT_SPAN]));
    }

    #[test]
    fn a_change_to_any_byte_the_code_covers_is_caught() {
        let payload = edc_payload(&dreamcast_vectors(), WHOLE_PAYLOAD);
        let intact = raw_sector(45_000, &payload, Edc::Computed);
        let code = kept(&intact);
        assert_eq!(edc(&intact[EDC_SPAN]), code);
        // One bit of any byte of the sync, the header or the user data changed, a bit of another
        // place in each byte, and the code the sector keeps no longer holds.
        for at in FORMAT_SPAN {
            let mut damaged = intact.clone();
            damaged[at] ^= 1 << (at % 8);
            assert_ne!(edc(&damaged[EDC_SPAN]), code, "{at}");
        }
        // Nor does the code itself, changed.
        for at in FORMAT_FIELD {
            let mut damaged = intact.clone();
            damaged[at] ^= 0x01;
            assert_ne!(edc(&damaged[EDC_SPAN]), kept(&damaged), "{at}");
        }
        // What follows the code, the parities that correct a sector, is no part of what it covers.
        let mut corrected = intact.clone();
        corrected[FORMAT_FIELD.end..].fill(0xFF);
        assert_eq!(edc(&corrected[EDC_SPAN]), kept(&corrected));
    }
}
