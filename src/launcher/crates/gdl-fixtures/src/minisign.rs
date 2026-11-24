//! A minisign signer holding a test key of the launcher's own, for the tests of the release
//! feed's signature check: it signs a file as minisign signs one prehashed, so the check reads
//! its signatures as it reads a release's. The key's secret half is a fixed seed, so every
//! signature is the same from run to run; no release ever trusts it.

use ring::signature::{Ed25519KeyPair, KeyPair};

/// The test key's secret half: a fixed seed, which no release ever trusts.
pub const TEST_KEY_SEED: [u8; 32] = *b"gdl-launcher test key, not real!";

/// The test key's id, as a minisign key carries one.
pub const TEST_KEY_ID: [u8; 8] = *b"GDL-TEST";

/// What a minisign public key gives as its algorithm: Ed25519.
const PUBLIC_KEY_ALGORITHM: [u8; 2] = *b"Ed";

/// What a minisign signature gives as its algorithm: Ed25519 over the file's BLAKE2b-512, the
/// prehashed form.
const PREHASHED_ALGORITHM: [u8; 2] = *b"ED";

/// The first line of every signature the test key makes.
const UNTRUSTED_COMMENT: &str = "untrusted comment: signature from the launcher's test key";

/// What a signature's third line puts before its trusted comment.
const TRUSTED_COMMENT_PREFIX: &str = "trusted comment: ";

/// What ends each line of a signature.
const LINE_END: char = '\n';

/// The length of a BLAKE2b-512 digest.
const BLAKE2B_512_LEN: usize = 64;

/// How many bytes BLAKE2b compresses at a time.
const BLAKE2B_BLOCK: usize = 128;

/// How many bytes each of the state's and a block's words takes.
const BLAKE2B_WORD: usize = 8;

/// How many rounds each compression runs.
const BLAKE2B_ROUNDS: usize = 12;

/// The first word of an unkeyed hash's parameter block, its digest length aside: a fanout and a
/// depth of one and no key (RFC 7693 section 2.5).
const BLAKE2B_PARAMETERS: u64 = 0x0101_0000;

/// BLAKE2b's initial state, SHA-512's (RFC 7693 section 2.6).
const BLAKE2B_IV: [u64; 8] = [
    0x6A09_E667_F3BC_C908,
    0xBB67_AE85_84CA_A73B,
    0x3C6E_F372_FE94_F82B,
    0xA54F_F53A_5F1D_36F1,
    0x510E_527F_ADE6_82D1,
    0x9B05_688C_2B3E_6C1F,
    0x1F83_D9AB_FB41_BD6B,
    0x5BE0_CD19_137E_2179,
];

/// The order in which each round takes the block's words, two to a mix; the rounds after the
/// tenth start the table over (RFC 7693 section 2.7).
const BLAKE2B_SIGMA: [[usize; 16]; 10] = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
    [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4],
    [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
    [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13],
    [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
    [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11],
    [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
    [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5],
    [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0],
];

/// The working words each of a round's eight mixes takes: the four columns, then the four
/// diagonals (RFC 7693 section 3.2).
const BLAKE2B_MIXES: [[usize; 4]; 8] = [
    [0, 4, 8, 12],
    [1, 5, 9, 13],
    [2, 6, 10, 14],
    [3, 7, 11, 15],
    [0, 5, 10, 15],
    [1, 6, 11, 12],
    [2, 7, 8, 13],
    [3, 4, 9, 14],
];

/// How far a mix turns its words right, in the order it turns them (RFC 7693 section 2.1).
const BLAKE2B_ROTATIONS: [u32; 4] = [32, 24, 16, 63];

/// The standard base64 alphabet (RFC 4648 section 4), in which minisign writes keys and
/// signatures.
const BASE64_ALPHABET: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// What pads base64 text to whole groups.
const BASE64_PAD: char = '=';

/// How many bytes a group of base64 text spells.
const BASE64_GROUP_BYTES: usize = 3;

/// How many characters a group of base64 text takes.
const BASE64_GROUP_CHARACTERS: usize = 4;

/// The test key's public half as a minisign public key's base64 line: "Ed", the id, the key.
pub fn test_public_key() -> String {
    let pair = test_key_pair();
    base64(
        &[
            &PUBLIC_KEY_ALGORITHM[..],
            &TEST_KEY_ID,
            pair.public_key().as_ref(),
        ]
        .concat(),
    )
}

/// A minisign signature of `data` by the test key, prehashed (BLAKE2b-512), with
/// `trusted_comment` as its trusted comment: the four lines a `.sig` file holds, LF-ended.
/// Panics when `trusted_comment` holds a line break, which the format cannot carry.
pub fn sign_with_test_key(data: &[u8], trusted_comment: &str) -> String {
    assert!(
        !trusted_comment.contains(['\r', '\n']),
        "a trusted comment is one line, not {trusted_comment:?}"
    );
    let pair = test_key_pair();
    let file_signature = pair.sign(&blake2b_512(data));
    let file_signature = file_signature.as_ref();
    let file_line = [&PREHASHED_ALGORITHM[..], &TEST_KEY_ID, file_signature].concat();
    // The global signature covers the file's signature and the trusted comment together, so the
    // comment can be neither changed nor carried over to another file's signature.
    let global_signature = pair.sign(&[file_signature, trusted_comment.as_bytes()].concat());
    format!(
        "{UNTRUSTED_COMMENT}{LINE_END}{}{LINE_END}{TRUSTED_COMMENT_PREFIX}{trusted_comment}\
         {LINE_END}{}{LINE_END}",
        base64(&file_line),
        base64(global_signature.as_ref())
    )
}

/// BLAKE2b with a 64-byte digest and no key (RFC 7693).
pub fn blake2b_512(data: &[u8]) -> [u8; 64] {
    let mut state = BLAKE2B_IV;
    state[0] ^= BLAKE2B_PARAMETERS | BLAKE2B_512_LEN as u64;
    // Every block before the last is compressed as it stands; the last, which may be short and
    // for an empty input holds nothing, is padded with zeros and compressed as the final one.
    let before_last = data.len().saturating_sub(1) / BLAKE2B_BLOCK * BLAKE2B_BLOCK;
    let (whole, last) = data.split_at(before_last);
    let mut counted: u128 = 0;
    for block in whole.as_chunks::<BLAKE2B_BLOCK>().0 {
        counted += BLAKE2B_BLOCK as u128;
        compress(&mut state, block, counted, false);
    }
    let mut padded = [0; BLAKE2B_BLOCK];
    padded[..last.len()].copy_from_slice(last);
    counted += last.len() as u128;
    compress(&mut state, &padded, counted, true);

    let mut digest = [0; BLAKE2B_512_LEN];
    for (bytes, word) in digest
        .as_chunks_mut::<BLAKE2B_WORD>()
        .0
        .iter_mut()
        .zip(state)
    {
        *bytes = word.to_le_bytes();
    }
    digest
}

/// The test key's pair of halves, made from its seed.
fn test_key_pair() -> Ed25519KeyPair {
    Ed25519KeyPair::from_seed_unchecked(&TEST_KEY_SEED).expect("any 32 bytes are an Ed25519 seed")
}

/// BLAKE2b's compression function F (RFC 7693 section 3.2): folds `block` into `state`, with
/// `counted` the bytes hashed so far, this block's included, and `last` marking the final block.
fn compress(state: &mut [u64; 8], block: &[u8; BLAKE2B_BLOCK], counted: u128, last: bool) {
    let mut words = [0; 16];
    for (word, bytes) in words.iter_mut().zip(block.as_chunks::<BLAKE2B_WORD>().0) {
        *word = u64::from_le_bytes(*bytes);
    }
    let mut working = [0; 16];
    working[..8].copy_from_slice(state);
    working[8..].copy_from_slice(&BLAKE2B_IV);
    // The counter is 128 bits, its low half in the thirteenth word and its high half in the
    // fourteenth.
    working[12] ^= counted as u64;
    working[13] ^= (counted >> 64) as u64;
    if last {
        working[14] = !working[14];
    }
    for order in BLAKE2B_SIGMA.iter().cycle().take(BLAKE2B_ROUNDS) {
        for (lanes, &[x, y]) in BLAKE2B_MIXES.iter().zip(order.as_chunks::<2>().0) {
            mix(&mut working, *lanes, words[x], words[y]);
        }
    }
    for (at, word) in state.iter_mut().enumerate() {
        *word ^= working[at] ^ working[at + 8];
    }
}

/// BLAKE2b's mixing function G (RFC 7693 section 3.1) over the working words at `lanes`, taking
/// the block's words `x` and `y`.
fn mix(working: &mut [u64; 16], [a, b, c, d]: [usize; 4], x: u64, y: u64) {
    let [first, second, third, fourth] = BLAKE2B_ROTATIONS;
    working[a] = working[a].wrapping_add(working[b]).wrapping_add(x);
    working[d] = (working[d] ^ working[a]).rotate_right(first);
    working[c] = working[c].wrapping_add(working[d]);
    working[b] = (working[b] ^ working[c]).rotate_right(second);
    working[a] = working[a].wrapping_add(working[b]).wrapping_add(y);
    working[d] = (working[d] ^ working[a]).rotate_right(third);
    working[c] = working[c].wrapping_add(working[d]);
    working[b] = (working[b] ^ working[c]).rotate_right(fourth);
}

/// `bytes` in standard base64 with padding (RFC 4648 section 4), as minisign writes keys and
/// signatures.
fn base64(bytes: &[u8]) -> String {
    let groups = bytes.len().div_ceil(BASE64_GROUP_BYTES);
    let mut text = String::with_capacity(groups * BASE64_GROUP_CHARACTERS);
    for group in bytes.chunks(BASE64_GROUP_BYTES) {
        // The group's bytes as one 24-bit number, the first byte highest, short groups ending in
        // zero bits.
        let joined = group.iter().enumerate().fold(0_u32, |joined, (at, &byte)| {
            joined | (u32::from(byte) << (16 - 8 * at))
        });
        // A group of n bytes takes n + 1 letters, padded to four.
        for at in 0..BASE64_GROUP_CHARACTERS {
            if at <= group.len() {
                let sextet = (joined >> (18 - 6 * at)) & 0x3F;
                text.push(char::from(BASE64_ALPHABET[sextet as usize]));
            } else {
                text.push(BASE64_PAD);
            }
        }
    }
    text
}

#[cfg(test)]
mod tests {
    use ring::signature::{ED25519, UnparsedPublicKey};

    use super::{
        BASE64_ALPHABET, TEST_KEY_ID, base64, blake2b_512, sign_with_test_key, test_public_key,
    };
    use crate::runtime::hex;

    /// A trusted comment as a release's signature carries one.
    const COMMENT: &str = "timestamp:1764003600\t\
        file:GauntletDarkLegacy-1.0.0-beta-windows-x64.sha256\tversion:1.0.0-beta";

    /// The test key's public half as `test_public_key` gives it.
    const PINNED_PUBLIC_KEY: &str = "RWRHREwtVEVTVAIHRlatKKopKz17ZiTblA/hZSC2IbuaChLYupiY8oUG";

    /// The test key's signature of `a checksum list` and a line end, with `COMMENT` as its
    /// trusted comment.
    const PINNED_SIGNATURE: &str = "untrusted comment: signature from the launcher's test key\n\
        RURHREwtVEVTVAEj/PQqBR4Er7TG8QEWycjQwJIMogM/1LcF7zVxjPeDugAHfgvJqeZYDclYB+1RncTtU6c0DlKqSw8\
        PH/uFcAM=\n\
        trusted comment: timestamp:1764003600\t\
        file:GauntletDarkLegacy-1.0.0-beta-windows-x64.sha256\tversion:1.0.0-beta\n\
        Tl7c4nx3l/6z6MDvPmpNnPUnA682HqnaKGSnFiR791XGHj6MYewO5ase+lgM5qmiotSSrbPsEBu6+kohNRtCBg==\n";

    /// The bytes standard base64 `text` with padding spells.
    fn unbase64(text: &str) -> Vec<u8> {
        let sextets: Vec<u32> = text
            .trim_end_matches('=')
            .bytes()
            .map(|letter| {
                let at = BASE64_ALPHABET
                    .iter()
                    .position(|&known| known == letter)
                    .unwrap_or_else(|| panic!("{letter} is no base64 letter in {text:?}"));
                u32::try_from(at).expect("an alphabet of 64")
            })
            .collect();
        let mut bytes = Vec::new();
        for group in sextets.chunks(4) {
            let joined = group
                .iter()
                .enumerate()
                .fold(0_u32, |joined, (at, &sextet)| {
                    joined | (sextet << (18 - 6 * at))
                });
            // A group of n letters spells n - 1 bytes, the 24-bit number's highest first.
            bytes.extend_from_slice(&joined.to_be_bytes()[1..group.len()]);
        }
        bytes
    }

    #[test]
    fn blake2b_follows_rfc_7693() {
        // RFC 7693 Appendix A's example, and the empty input, hashed as one final block of zeros;
        // CPython's hashlib.blake2b(digest_size=64) gives both.
        assert_eq!(
            hex(&blake2b_512(b"abc")),
            "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1\
             7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"
        );
        assert_eq!(
            hex(&blake2b_512(b"")),
            "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419\
             d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce"
        );
        // Inputs about the edges of the 128-byte block, each byte its offset modulo 251, with the
        // digests CPython's hashlib.blake2b(digest_size=64) gives them: a whole last block is
        // compressed as the final one, never followed by an empty block.
        for (len, digest) in [
            (
                127,
                "b6292669ccd38d5f01caae96ba272c76a879a45743afa0725d83b9ebb26665b7\
                 31f1848c52f11972b6644f554c064fa90780dbbbf3a89d4fc31f67df3e5857ef",
            ),
            (
                128,
                "2319e3789c47e2daa5fe807f61bec2a1a6537fa03f19ff32e87eecbfd64b7e0e\
                 8ccff439ac333b040f19b0c4ddd11a61e24ac1fe0f10a039806c5dcc0da3d115",
            ),
            (
                129,
                "f59711d44a031d5f97a9413c065d1e614c417ede998590325f49bad2fd444d3e\
                 4418be19aec4e11449ac1a57207898bc57d76a1bcf3566292c20c683a5c4648f",
            ),
            (
                256,
                "93463ac058b6163eb43be3f5bb32b28541498f4e3366f1effe253ad44e1e076e\
                 41c3616046027c82a7124f8f4746668ad10b12e8e25a95ac8f3151df01cd5a93",
            ),
            (
                257,
                "9ca40e2ddee9436dbbd08efc65dbaf4870059f5eb3d76efd20241ae5bf13c60f\
                 250b882ea5c564838257a3fc95c496819ace2c6490b55b268535208dfc31822c",
            ),
            (
                1000,
                "c11e1c0340bd7e5a1b275f1230c962fad215ecb1391486e74e31b960a2f29963\
                 81a5fad092da06841d5f26e38f6ecfeaf441acbcd1c2de61aef121e7927175f5",
            ),
        ] {
            let data: Vec<u8> = (0..len)
                .map(|at: usize| u8::try_from(at % 251).expect("under 251"))
                .collect();
            assert_eq!(hex(&blake2b_512(&data)), digest, "{len} bytes");
        }
    }

    #[test]
    fn base64_follows_rfc_4648() {
        // RFC 4648 section 10's examples: a short group is padded to four letters.
        for (bytes, text) in [
            ("", ""),
            ("f", "Zg=="),
            ("fo", "Zm8="),
            ("foo", "Zm9v"),
            ("foob", "Zm9vYg=="),
            ("fooba", "Zm9vYmE="),
            ("foobar", "Zm9vYmFy"),
        ] {
            assert_eq!(base64(bytes.as_bytes()), text, "{bytes:?}");
            assert_eq!(unbase64(text), bytes.as_bytes(), "{text:?}");
        }
        // The bytes that spell the whole alphabet in order, CPython's base64.b64decode of it:
        // every letter, `+` and `/` included, stands for its own six bits.
        let alphabet = [
            0x00, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8B, 0x30, 0xD3, 0x8F, 0x41, 0x14,
            0x93, 0x51, 0x55, 0x97, 0x61, 0x96, 0x9B, 0x71, 0xD7, 0x9F, 0x82, 0x18, 0xA3, 0x92,
            0x59, 0xA7, 0xA2, 0x9A, 0xAB, 0xB2, 0xDB, 0xAF, 0xC3, 0x1C, 0xB3, 0xD3, 0x5D, 0xB7,
            0xE3, 0x9E, 0xBB, 0xF3, 0xDF, 0xBF,
        ];
        assert_eq!(base64(&alphabet).as_bytes(), BASE64_ALPHABET);
        assert_eq!(unbase64(&base64(&alphabet)), alphabet);
    }

    #[test]
    fn a_signature_is_minisigns_four_lines_by_the_test_key() {
        let list = b"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  \
                     GauntletDarkLegacy-1.0.0-beta-windows-x64-launcher.exe\n";
        let text = sign_with_test_key(list, COMMENT);
        assert!(!text.contains('\r'), "{text}");
        let lines: Vec<&str> = text.split_terminator('\n').collect();
        assert_eq!(lines.len(), 4, "{text}");
        assert!(text.ends_with('\n'), "{text}");
        assert_eq!(
            lines[0],
            "untrusted comment: signature from the launcher's test key"
        );
        assert_eq!(lines[2], format!("trusted comment: {COMMENT}"));

        // The public key: "Ed", the id, then the key.
        let public = unbase64(&test_public_key());
        assert_eq!(public.len(), 42);
        assert_eq!(&public[..2], b"Ed");
        assert_eq!(public[2..10], TEST_KEY_ID);
        let key = UnparsedPublicKey::new(&ED25519, &public[10..]);

        // The file's signature: "ED" (prehashed), the id, then Ed25519 over the list's BLAKE2b-512.
        let file_line = unbase64(lines[1]);
        assert_eq!(file_line.len(), 74);
        assert_eq!(&file_line[..2], b"ED");
        assert_eq!(file_line[2..10], TEST_KEY_ID);
        let file_signature = &file_line[10..];
        key.verify(&blake2b_512(list), file_signature)
            .expect("the list's signature verifies");
        assert!(
            key.verify(list, file_signature).is_err(),
            "not over the list itself"
        );
        assert!(
            key.verify(&blake2b_512(b"another list\n"), file_signature)
                .is_err()
        );

        // The global signature: Ed25519 over the file's signature and the trusted comment.
        let global_signature = unbase64(lines[3]);
        assert_eq!(global_signature.len(), 64);
        key.verify(
            &[file_signature, COMMENT.as_bytes()].concat(),
            &global_signature,
        )
        .expect("the global signature verifies");
        let other_comment = COMMENT.replace("1.0.0-beta", "1.0.0-beta.1");
        assert!(
            key.verify(
                &[file_signature, other_comment.as_bytes()].concat(),
                &global_signature
            )
            .is_err()
        );
    }

    #[test]
    fn signatures_are_deterministic() {
        let list = b"a checksum list\n";
        let signature = sign_with_test_key(list, COMMENT);
        assert_eq!(sign_with_test_key(list, COMMENT), signature);
        assert_eq!(test_public_key(), test_public_key());
        // Pinned, so that any change to the key, the hash or the layout shows here first.
        assert_eq!(test_public_key(), PINNED_PUBLIC_KEY);
        assert_eq!(signature, PINNED_SIGNATURE);
        // Another file, or another trusted comment, is another signature.
        assert_ne!(
            sign_with_test_key(b"a checksum list.\n", COMMENT),
            signature
        );
        assert_ne!(
            sign_with_test_key(list, &COMMENT.replace("1764003600", "1764003601")),
            signature
        );
    }

    #[test]
    #[should_panic(expected = "a trusted comment is one line")]
    fn a_trusted_comment_cannot_break_its_line() {
        sign_with_test_key(b"a checksum list\n", "file:list.sha256\nversion:1.0.0-beta");
    }
}
