//! The digests the readers give a dump and what it holds, SHA-256 and SHA-1. They print as the
//! extraction scripts write digests: lowercase hexadecimal, two digits a byte.

use std::fmt;

use sha1::Sha1;
use sha2::{Digest, Sha256};

/// How many bytes a SHA-256 digest holds.
const SHA256_LEN: usize = 32;

/// How many bytes a SHA-1 digest holds.
const SHA1_LEN: usize = 20;

/// A SHA-256 digest. Digests order as their bytes do, which is also how their text orders.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Sha256Digest(pub [u8; SHA256_LEN]);

/// A SHA-1 digest, the one an N64 ROM is known by.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Sha1Digest(pub [u8; SHA1_LEN]);

impl Sha256Digest {
    /// The digest of `bytes`.
    pub fn of(bytes: &[u8]) -> Self {
        Sha256Digest(Sha256::digest(bytes).into())
    }

    /// The digest in lowercase hexadecimal, as the extraction scripts write it.
    pub fn hex(&self) -> String {
        self.to_string()
    }

    /// The digest `text` writes as exactly 64 lowercase hexadecimal digits, the one form the
    /// extraction scripts write. Any other text is no digest.
    pub fn from_hex(text: &str) -> Option<Self> {
        hex_bytes::<SHA256_LEN>(text).map(Sha256Digest)
    }
}

impl Sha1Digest {
    /// The digest of `bytes`.
    pub fn of(bytes: &[u8]) -> Self {
        Sha1Digest(Sha1::digest(bytes).into())
    }

    /// The digest in lowercase hexadecimal, as the extraction scripts write it.
    pub fn hex(&self) -> String {
        self.to_string()
    }

    /// The digest `text` writes as exactly 40 lowercase hexadecimal digits, for a constant
    /// written as the extraction scripts write it. Any other text panics, so that a constant
    /// written otherwise stops the build.
    pub const fn from_hex_const(text: &str) -> Self {
        match hex_bytes::<SHA1_LEN>(text) {
            Some(bytes) => Sha1Digest(bytes),
            None => panic!("a SHA-1 digest is written as 40 lowercase hexadecimal digits"),
        }
    }
}

impl fmt::Display for Sha256Digest {
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        write_hex(&self.0, to)
    }
}

impl fmt::Debug for Sha256Digest {
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        write_hex(&self.0, to)
    }
}

impl fmt::Display for Sha1Digest {
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        write_hex(&self.0, to)
    }
}

impl fmt::Debug for Sha1Digest {
    fn fmt(&self, to: &mut fmt::Formatter<'_>) -> fmt::Result {
        write_hex(&self.0, to)
    }
}

/// Writes `bytes` in lowercase hexadecimal, two digits a byte, the high one first.
fn write_hex(bytes: &[u8], to: &mut fmt::Formatter<'_>) -> fmt::Result {
    bytes.iter().try_for_each(|byte| write!(to, "{byte:02x}"))
}

/// The `N` bytes `text` spells in lowercase hexadecimal, two digits a byte, the high one first,
/// or `None` when `text` is anything else.
const fn hex_bytes<const N: usize>(text: &str) -> Option<[u8; N]> {
    let digits = text.as_bytes();
    if digits.len() != 2 * N {
        return None;
    }
    let mut bytes = [0; N];
    let mut at = 0;
    while at < N {
        let (Some(high), Some(low)) =
            (digit_value(digits[2 * at]), digit_value(digits[2 * at + 1]))
        else {
            return None;
        };
        bytes[at] = (high << 4) | low;
        at += 1;
    }
    Some(bytes)
}

/// The value of a lowercase hexadecimal digit, or `None` for any other byte.
const fn digit_value(digit: u8) -> Option<u8> {
    match digit {
        b'0'..=b'9' => Some(digit - b'0'),
        b'a'..=b'f' => Some(digit - b'a' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use std::hint::black_box;

    use super::{Sha1Digest, Sha256Digest};
    use crate::US_SHA1;

    /// The digest the N64 extractor knows the US ROM by, in its canonical byte order, as it
    /// writes it.
    const US_SHA1_TEXT: &str = "0489dcce749c6a5102681d288ed0616a4b94e99d";

    #[test]
    fn digests_print_in_lowercase_hex_and_read_back() {
        // The US ROM's digest, which a constant reads while compiling, prints as it was written.
        assert_eq!(US_SHA1.hex(), US_SHA1_TEXT);
        assert_eq!(US_SHA1.to_string(), US_SHA1_TEXT);
        assert_eq!(format!("{US_SHA1:?}"), US_SHA1_TEXT);
        assert_eq!(US_SHA1.0[..2], [0x04, 0x89]);
        assert_eq!(US_SHA1.0[19], 0x9d);

        // The digests of FIPS 180's own examples, the empty message and "abc".
        let sha1 = [
            (&b""[..], "da39a3ee5e6b4b0d3255bfef95601890afd80709"),
            (b"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"),
        ];
        for (bytes, text) in sha1 {
            let digest = Sha1Digest::of(bytes);
            assert_eq!(digest.hex(), text);
            assert_eq!(format!("{digest:?}"), text);
        }
        let sha256 = [
            (
                &b""[..],
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            ),
            (
                b"abc",
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            ),
        ];
        for (bytes, text) in sha256 {
            let digest = Sha256Digest::of(bytes);
            assert_eq!(digest.hex(), text);
            assert_eq!(digest.to_string(), text);
            assert_eq!(format!("{digest:?}"), text);
            assert_eq!(Sha256Digest::from_hex(text), Some(digest));
        }
    }

    #[test]
    fn a_digest_is_read_only_as_the_scripts_write_it() {
        let text = Sha256Digest::of(b"abc").hex();
        let refused = [
            String::new(),
            text.to_uppercase(),
            text[..63].to_owned(),
            format!("{text}0"),
            format!("{}g", &text[..63]),
            format!("0x{}", &text[..62]),
            format!(" {}", &text[..63]),
            // Sixty-four bytes, but the last two are one letter that is no digit.
            format!("{}\u{e9}", &text[..62]),
        ];
        for text in refused {
            assert_eq!(Sha256Digest::from_hex(&text), None, "{text:?}");
        }
    }

    #[test]
    #[should_panic(expected = "a SHA-1 digest is written as 40 lowercase hexadecimal digits")]
    fn a_constant_digest_written_otherwise_is_refused() {
        // Outside a constant, the refusal comes when the program runs.
        Sha1Digest::from_hex_const(black_box("0489DCCE749C6A5102681D288ED0616A4B94E99D"));
    }

    #[test]
    fn digests_order_as_their_text_does() {
        let mut digests: Vec<Sha256Digest> =
            (0u8..64).map(|byte| Sha256Digest::of(&[byte])).collect();
        let mut texts: Vec<String> = digests.iter().map(Sha256Digest::hex).collect();
        digests.sort();
        texts.sort();
        let sorted: Vec<String> = digests.iter().map(Sha256Digest::hex).collect();
        assert_eq!(sorted, texts);
    }
}
