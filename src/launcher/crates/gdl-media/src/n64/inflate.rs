//! Raw DEFLATE as the N64 extractor inflates the ROMDISK's compressed files and the executable
//! overlays: decoded never to more than a byte past the size a record gives, so that nothing past
//! where the bytes should end is ever looked at, then held to the stream's end marker at exactly
//! that size, with nothing after it but, for an overlay, the few bytes that pad its extent.

use flate2::{Decompress, FlushDecompress, Status};
use gdl_files::BLOCK_LEN;

use crate::{MediaError, RomRule};

/// The most bytes an executable overlay's extent may hold past the end of its stream. The boot
/// table aligns each extent for its copy into memory, and the bytes that pad it may hold any
/// value; 16 or more are refused, as the N64 extractor refuses them.
pub const MAX_OVERLAY_PADDING: usize = 15;

/// The most bytes one call to the decoder adds to the output, so that room for a file is made only
/// as its bytes are decoded, never at once for the size its record declares, which nothing bounds
/// for an overlay.
pub const INFLATE_STEP: usize = BLOCK_LEN;

/// What may follow the end of a raw DEFLATE stream, within the bytes that hold it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Trailer {
    /// Nothing: a ROMDISK file's bytes end where its stream ends.
    None,
    /// Up to [`MAX_OVERLAY_PADDING`] bytes, which pad an executable overlay's extent.
    OverlayPadding,
}

/// What the N64 extractor answers for a stream that reaches no end marker within a byte past its
/// size, or decodes to other than exactly that size.
const WRONG_SIZE_OR_END: &str = "DEFLATE output size or end marker does not match its directory";

/// What it answers for bytes past the end of a ROMDISK file's stream.
const BYTES_AFTER_STREAM: &str = "unexpected bytes after ROMDISK DEFLATE stream";

/// What it answers for an overlay's extent that holds more padding than the most it may.
const UNSUPPORTED_PADDING: &str = "unsupported executable overlay padding";

/// The `size` bytes the raw DEFLATE `stream` holds, decoded as the N64 extractor decodes them:
/// never to more than a byte past `size`, so that damage past where the bytes should end is left
/// unread as the extractor leaves it, and refused unless the stream's end marker comes at exactly
/// `size` bytes, followed by nothing but what `trailer` allows.
pub fn inflate(stream: &[u8], size: usize, trailer: Trailer) -> Result<Vec<u8>, MediaError> {
    inflate_in_steps(stream, size, trailer, INFLATE_STEP)
}

/// [`inflate`], with the output grown by at most `step` bytes a call to the decoder, which tests
/// lower so that a short stream spans many steps.
fn inflate_in_steps(
    stream: &[u8],
    size: usize,
    trailer: Trailer,
    step: usize,
) -> Result<Vec<u8>, MediaError> {
    let decoded = decode(stream, size, step)?;
    if !decoded.ended || decoded.output.len() != size {
        return Err(MediaError::rom(RomRule::DeflateEnd, WRONG_SIZE_OR_END));
    }
    let after = stream.len() - decoded.used;
    match trailer {
        Trailer::None if after > 0 => {
            Err(MediaError::rom(RomRule::DeflateTrailer, BYTES_AFTER_STREAM))
        }
        Trailer::OverlayPadding if after > MAX_OVERLAY_PADDING => Err(MediaError::rom(
            RomRule::OverlayPadding,
            UNSUPPORTED_PADDING,
        )),
        Trailer::None | Trailer::OverlayPadding => Ok(decoded.output),
    }
}

/// Where decoding a stream stopped.
struct Decoded {
    /// The bytes decoded: at most a byte more than the size asked for.
    output: Vec<u8>,
    /// Whether the stream's end marker was reached.
    ended: bool,
    /// How many of the stream's bytes the decoder took.
    used: usize,
}

/// Decodes `stream` until its end marker, until a byte more than `size` is decoded, or until the
/// decoder can go no further with what is left of it, growing the output by at most `step` bytes
/// a call and handing the decoder only the room just made.
fn decode(stream: &[u8], size: usize, step: usize) -> Result<Decoded, MediaError> {
    // A byte past the size, which a stream holding more than its record gives decodes to.
    let most = size.saturating_add(1);
    // Raw DEFLATE, with no header before it, over the format's whole window.
    let mut decoder = Decompress::new(false);
    let mut output = Vec::new();
    loop {
        // What the decoder has taken and given so far, each within the slice it was handed.
        let (used, filled) = (decoder.total_in() as usize, output.len());
        output.resize(filled + step.min(most - filled), 0);
        let status = decoder
            .decompress(
                &stream[used..],
                &mut output[filled..],
                FlushDecompress::None,
            )
            .map_err(|error| {
                let detail = format!("invalid raw DEFLATE stream: {error}");
                MediaError::rom(RomRule::Deflate, detail)
            })?;
        output.truncate(decoder.total_out() as usize);
        let ended = status == Status::StreamEnd;
        let stuck = decoder.total_in() as usize == used && output.len() == filled;
        if ended || stuck || output.len() == most {
            let used = decoder.total_in() as usize;
            return Ok(Decoded {
                output,
                ended,
                used,
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use gdl_fixtures::unhex;
    use serde_json::Value;

    use super::{INFLATE_STEP, MAX_OVERLAY_PADDING, Trailer, decode, inflate, inflate_in_steps};
    use crate::{MediaError, Sha256Digest};

    /// The N64 cases the extractor's tests and the launcher's read alike.
    const VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/legends/n64-vectors.json"
    ));

    /// What the N64 extractor answers for a stream that does not end at exactly its size.
    const WRONG_END: &str = "DEFLATE output size or end marker does not match its directory";

    /// How the launcher's words for a damaged stream begin, as the extractor's do.
    const DAMAGED: &str = "invalid raw DEFLATE stream: ";

    /// The bytes the vectors' `nativeBytes` stream holds.
    const NATIVE_BYTES: &[u8] = b"original native bytes";

    /// How many times that stream holds them.
    const NATIVE_REPEATS: usize = 100;

    /// What is left of an outcome to compare: the bytes decoded, or the rule and the words a
    /// stream was refused with.
    type Said = Result<Vec<u8>, (&'static str, String)>;

    fn vectors() -> Value {
        serde_json::from_str(VECTORS).expect("the N64 vectors are JSON")
    }

    /// The base stream the vectors deflate under `name`: `nativeBytes`, or `test`, the four bytes
    /// `test`.
    fn deflated(name: &str) -> Vec<u8> {
        unhex(&vectors()["deflated"][name])
    }

    /// A string of the vectors.
    fn text(value: &Value) -> &str {
        value.as_str().expect("a string")
    }

    /// A number of the vectors.
    fn number(value: &Value) -> usize {
        usize::try_from(value.as_u64().expect("a number")).unwrap()
    }

    /// The trailer a case names.
    fn trailer(name: &Value) -> Trailer {
        match text(name) {
            "none" => Trailer::None,
            "overlayPadding" => Trailer::OverlayPadding,
            other => panic!("no trailer {other}"),
        }
    }

    /// `outcome` as what it decoded, or as the rule and the words it was refused with.
    fn said(outcome: Result<Vec<u8>, MediaError>) -> Said {
        outcome.map_err(|error| (error.rule(), error.to_string()))
    }

    /// A stored block of ten bytes that is not the stream's last, then the header of a block of a
    /// type the format does not have.
    fn cut_off() -> Vec<u8> {
        let mut stream = vec![0x00, 0x0A, 0x00, 0xF5, 0xFF];
        stream.extend_from_slice(&[b'A'; 10]);
        stream.push(0x07);
        stream
    }

    /// Checks that `error` refuses a stream in the extractor's words, where they are fixed, and
    /// otherwise in the launcher's own, which give the reason the decoder gave the extractor.
    fn assert_said(error: &MediaError, python: &str) {
        assert!(matches!(error, MediaError::Rom { .. }), "{error:?}");
        let detail = error.to_string();
        if error.rule() == "deflate" {
            let reason = python.rsplit(": ").next().unwrap();
            assert!(
                detail.starts_with(DAMAGED) && python.starts_with(DAMAGED),
                "{detail}"
            );
            assert!(detail.ends_with(reason), "{detail} for {python}");
        } else {
            assert_eq!(detail, python);
        }
    }

    #[test]
    fn streams_follow_the_shared_vectors() {
        let vectors = vectors();
        let cases = vectors["deflate"].as_array().unwrap();
        assert!(!cases.is_empty());
        let mut rules = Vec::new();
        for case in cases {
            // The launcher decides every stream as the extractor does.
            assert!(case.get("deviation").is_none(), "{case}");
            let stream = unhex(&case["stream"]);
            let size = number(&case["size"]);
            let outcome = inflate(&stream, size, trailer(&case["trailer"]));
            match (outcome, case["rule"].as_str()) {
                (Ok(output), None) => {
                    assert!(case["python"].is_null(), "{case}");
                    assert_eq!(output.len(), size, "{case}");
                    let digest = Sha256Digest::of(&output).hex();
                    assert_eq!(case["outputSha256"], digest, "{case}");
                }
                (Err(error), Some(rule)) => {
                    assert_eq!(error.rule(), rule, "{case}");
                    assert_said(&error, text(&case["python"]));
                    rules.push(rule);
                }
                (outcome, rule) => panic!("{case}: {outcome:?} where {rule:?} was expected"),
            }
        }
        rules.sort_unstable();
        rules.dedup();
        assert_eq!(
            rules,
            ["deflate", "deflateEnd", "deflateTrailer", "overlayPadding"]
        );

        // Among them, a distance back past the start of the output, which a decoder that does not
        // track where its window starts would take.
        let far = cases
            .iter()
            .find(|case| case["stream"] == "030200")
            .unwrap();
        assert_eq!(far["rule"], "deflate");
        assert!(text(&far["python"]).ends_with("invalid distance too far back"));
        // And the stream cut off at five bytes, where the decoding stops a byte past them within
        // the stored block, and at ten, where it reads the header after the block.
        let cut: Vec<(usize, &str)> = cases
            .iter()
            .filter(|case| unhex(&case["stream"]) == cut_off())
            .map(|case| (number(&case["size"]), text(&case["rule"])))
            .collect();
        assert_eq!(cut, [(5, "deflateEnd"), (10, "deflate")]);
    }

    #[test]
    fn nothing_past_a_byte_more_than_the_size_is_decoded() {
        // Under nine bytes, the decoding stops within the stored block, a byte past the size, and
        // never reaches the header after it. From nine, the block ends within a byte past the
        // size, and the header after it is read and refused, as the extractor reads it.
        let stream = cut_off();
        for size in 0..=12 {
            let error = inflate(&stream, size, Trailer::None).unwrap_err();
            let detail = error.to_string();
            if size < 9 {
                assert_eq!(error.rule(), "deflateEnd", "{size}");
                assert_eq!(detail, WRONG_END);
            } else {
                assert_eq!(error.rule(), "deflate", "{size}");
                let reason = detail.strip_prefix(DAMAGED).unwrap_or_default();
                assert!(reason.ends_with("invalid block type"), "{detail}");
            }
        }
    }

    #[test]
    fn padding_is_bounded_and_only_for_overlays() {
        let test = deflated("test");
        assert_eq!(MAX_OVERLAY_PADDING, 15);
        // Bytes of any value past the stream's end, of every length up to two past the most.
        for len in 0..=MAX_OVERLAY_PADDING + 2 {
            for byte in [0x00, 0x78, 0xFF] {
                let mut padded = test.clone();
                padded.resize(test.len() + len, byte);
                // An overlay's extent may hold fewer than sixteen.
                let overlay = inflate(&padded, 4, Trailer::OverlayPadding);
                if len <= MAX_OVERLAY_PADDING {
                    assert_eq!(overlay.unwrap(), b"test", "{len}");
                } else {
                    let error = overlay.unwrap_err();
                    assert_eq!(error.rule(), "overlayPadding", "{len}");
                    assert_said(&error, "unsupported executable overlay padding");
                }
                // A ROMDISK file's bytes end where its stream ends.
                let file = inflate(&padded, 4, Trailer::None);
                if len == 0 {
                    assert_eq!(file.unwrap(), b"test");
                } else {
                    let error = file.unwrap_err();
                    assert_eq!(error.rule(), "deflateTrailer", "{len}");
                    assert_said(&error, "unexpected bytes after ROMDISK DEFLATE stream");
                }
            }
        }

        // A stream that does not decode to its size is refused for that first, whatever follows.
        let mut padded = test.clone();
        padded.extend_from_slice(&[0x78; 20]);
        for trailer in [Trailer::None, Trailer::OverlayPadding] {
            for size in [0, 3, 5] {
                let error = inflate(&padded, size, trailer).unwrap_err();
                assert_eq!(error.rule(), "deflateEnd", "{size} {trailer:?}");
            }
        }
    }

    #[test]
    fn every_truncation_is_refused() {
        // Each base stream decodes whole, and cut short anywhere before its end it is refused under
        // either trailer: a cut stream reaches no end marker.
        let streams = [
            (deflated("nativeBytes"), NATIVE_BYTES.repeat(NATIVE_REPEATS)),
            (deflated("test"), b"test".to_vec()),
        ];
        for (stream, bytes) in streams {
            assert_eq!(inflate(&stream, bytes.len(), Trailer::None).unwrap(), bytes);
            for len in 0..stream.len() {
                for trailer in [Trailer::None, Trailer::OverlayPadding] {
                    let error = inflate(&stream[..len], bytes.len(), trailer).unwrap_err();
                    assert_eq!(error.rule(), "deflateEnd", "{len} {trailer:?}");
                    assert_eq!(error.to_string(), WRONG_END);
                }
            }
        }
    }

    #[test]
    fn a_large_declared_size_grows_the_output_only_as_it_decodes() {
        let test = deflated("test");
        let declared: usize = 2 << 30;
        for trailer in [Trailer::None, Trailer::OverlayPadding] {
            let error = inflate(&test, declared, trailer).unwrap_err();
            assert_eq!(error.rule(), "deflateEnd", "{trailer:?}");
            assert_eq!(error.to_string(), WRONG_END);
        }
        // The output was given one step's room, though far more was declared, and holds only the
        // four bytes decoded before the stream's end.
        let decoded = decode(&test, declared, INFLATE_STEP).unwrap();
        assert!(decoded.ended);
        assert_eq!(decoded.output, b"test");
        assert_eq!(decoded.used, test.len());
        let room = decoded.output.capacity();
        assert!(room <= INFLATE_STEP, "{room}");
        assert_eq!(INFLATE_STEP, 1 << 20);
    }

    #[test]
    fn small_steps_decode_as_one_does() {
        // Every shared case, and the cut-off stream at every size, with the output grown a few
        // bytes at a time, decide as when it is grown in steps no file here fills.
        let vectors = vectors();
        let mut inputs: Vec<(Vec<u8>, usize, Trailer)> = vectors["deflate"]
            .as_array()
            .unwrap()
            .iter()
            .map(|case| {
                let size = number(&case["size"]);
                (unhex(&case["stream"]), size, trailer(&case["trailer"]))
            })
            .collect();
        inputs.extend((0..=12).map(|size| (cut_off(), size, Trailer::None)));
        let steps: Vec<usize> = (1..=24)
            .chain([255, 256, 1024, 2099, 2100, 2101, 4096])
            .collect();
        for (stream, size, trailer) in &inputs {
            let whole = said(inflate(stream, *size, *trailer));
            for &step in &steps {
                let stepped = said(inflate_in_steps(stream, *size, *trailer, step));
                assert_eq!(stepped, whole, "{size} {trailer:?} by {step}");
            }
        }
    }
}
