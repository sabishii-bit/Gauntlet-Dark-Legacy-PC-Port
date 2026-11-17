//! How strictly an extraction holds the dump it reads: as the extraction scripts hold one, for
//! developers and the parity tests, or as an installation holds one, to the [`Trust`] it is given.
//! An installation's extraction holds the dump's identity to the trust's before it decodes or reads
//! a file the dump holds, every sector a disc's reader reads to its error detection code, every
//! track a disc's sheet lists to being there, and the disc's high-density area to starting where a
//! pressed disc's does; when the trust holds fingerprints, it holds IP.BIN and every disc file to
//! them too, all before anything is written.

use gdl_media::{
    DiscIdentity, DiscRules, DumpRefusal, INSTALL_RULES, SCRIPT_RULES, Sha1Digest, US_DISC,
    US_SHA1, Verdict,
};

use crate::{ExtractError, Fingerprints};

/// How strictly an extraction holds the dump it reads.
#[derive(Clone, Copy, Debug)]
pub enum Strictness<'a> {
    /// As the extraction scripts hold a dump: no gate on its identity, a disc's sectors held to
    /// their headers alone and its audio tracks never opened, with only the launcher's refusals of
    /// what no real dump holds beside the scripts' own.
    Script,
    /// As an installation holds a dump, to the trust it is given.
    Install(&'a Trust),
}

impl Strictness<'_> {
    /// The strictness's stable machine name, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            Strictness::Script => "script",
            Strictness::Install(_) => "install",
        }
    }

    /// The rules a disc is read by under the strictness.
    pub(crate) fn disc_rules(self) -> &'static DiscRules {
        match self {
            Strictness::Script => &SCRIPT_RULES,
            Strictness::Install(_) => &INSTALL_RULES,
        }
    }
}

/// What an installation holds the dumps it installs the expansions from to: each by its identity,
/// and the disc's files by their fingerprints.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Trust {
    /// The SHA-1 digest of the cartridge the N64 Legends expansion is installed from, its ROM in
    /// the big-endian byte order.
    pub n64_canonical_sha1: Sha1Digest,
    /// The identity the boot area keeps of the disc the Dreamcast Legends expansion is installed
    /// from.
    pub disc: DiscIdentity,
    /// The fingerprints that disc's files are held to, or none to hold the disc to its identity
    /// alone.
    pub fingerprints: Option<Fingerprints>,
}

impl Trust {
    /// What a player's installation is held to: the US cartridge, the US disc, and the
    /// fingerprints of the US disc's files the launcher carries.
    pub fn shipped() -> Trust {
        Trust {
            n64_canonical_sha1: US_SHA1,
            disc: US_DISC,
            fingerprints: Some(Fingerprints::committed()),
        }
    }
}

/// Holds a dump to `verdict`, how its identity stands against a trust: only the dump the trust
/// names passes, and any other is refused for the reason the verdict gives.
pub(crate) fn held(verdict: Verdict) -> Result<(), ExtractError> {
    let refusal = match verdict {
        Verdict::Supported => return Ok(()),
        Verdict::Refused(refusal) => refusal,
        // A verdict that judged nothing names no dump the trust holds.
        Verdict::Unchecked => DumpRefusal::Unrecognized,
    };
    Err(ExtractError::Refused { refusal })
}

#[cfg(test)]
mod tests {
    use std::ffi::OsStr;
    use std::ops::Range;

    use gdl_files::{Cancel, Job, Staging};
    use gdl_fixtures::{DiscSet, Edc, raw_sector, sha256_hex};
    use gdl_media::{
        AREA, BOOT_LBA, DiscIdentity, DumpRefusal, INSTALL_RULES, N64Rom, PRODUCT, SCRIPT_RULES,
        Sha1Digest, Sha256Digest, SourceName, US_DISC, US_SHA1, VERSION, Verdict,
    };
    use serde_json::Value;

    use super::{Strictness, Trust, held};
    use crate::vectors::{
        ChangedAtUnpack, Folders, Listed, Recorded, STAGING_PREFIX, array, disc_file_lba,
        disc_trust, dreamcast_root, dreamcast_vectors, extraction_case, fingerprint_files, frozen,
        full_golden, installable_disc, items_of, listed, n64_root, n64_vectors, phases_of, quiet,
        sector_place, stage_dreamcast, stage_n64, text, tree_listing,
    };
    use crate::{
        CHECK, DISC_FOLDER, ExtractError, ExtractSettings, Fingerprints, IP_BIN, READ_DISC, UNPACK,
        check_dreamcast, check_n64, extract_dreamcast,
    };

    /// Where an N64 ROM's header keeps its game code: a cartridge's letter, the game's two and
    /// the region's.
    const GAME_CODE: Range<usize> = 0x3B..0x3F;

    /// The full set's stream, the disc file whose sectors the tests change.
    const STREAM: &str = "AUDIO/X.STR";

    /// Where the tests change a byte of the stream's first sector, within its user data.
    const CHANGED_AT: usize = 100;

    /// What the reader answers for a sector at `lba` that fails its error detection code.
    fn fails_its_code(lba: u64) -> String {
        format!("Raw sector at LBA {lba} fails its error detection code")
    }

    /// The name of a dump's file called `text`.
    fn named(text: &str) -> SourceName {
        SourceName::from_units(text.encode_utf16().collect())
    }

    /// The digest `listing` gives the file at `path`.
    fn digest_in(listing: &[Listed], path: &str) -> String {
        let row = listing.iter().find(|(listed, _, _)| listed == path);
        row.map(|(_, _, digest)| digest.clone())
            .unwrap_or_else(|| panic!("no {path} in {listing:?}"))
    }

    /// The user data the full set `set` keeps in its sector at `lba`, as the set's golden manifest
    /// places that sector.
    fn sector_data(set: &DiscSet, golden: &Value, lba: u64) -> Vec<u8> {
        let (track, place) = sector_place(golden, lba);
        let held = set.file(&track).contents();
        let sector = &held[usize::try_from(place).unwrap()..][..gdl_fixtures::RAW_SECTOR];
        sector[gdl_media::USER_DATA].to_vec()
    }

    /// `set` with its sector at `lba` holding `data` as a pressed disc's would: under a header
    /// that places it where it is, and with its code worked out from what it holds.
    fn with_sector(set: DiscSet, golden: &Value, lba: u64, data: &[u8]) -> DiscSet {
        let (track, place) = sector_place(golden, lba);
        let sector = raw_sector(u32::try_from(lba).unwrap(), data, Edc::Computed);
        set.patched(&track, place, &sector)
    }

    /// What the stream's first sector holds once a bit of it is changed, and what the stream then
    /// holds: its two sectors' user data, cut to its size.
    fn another_stream(golden: &Value) -> (Vec<u8>, Vec<u8>) {
        let set = installable_disc();
        let lba = disc_file_lba(golden, STREAM);
        let mut first = sector_data(&set, golden, lba);
        first[CHANGED_AT] ^= 1;
        let rows = array(&golden["files"]);
        let row = rows.iter().find(|row| row["path"] == STREAM).unwrap();
        let size = usize::try_from(row["size"].as_u64().unwrap()).unwrap();
        let mut stream = first.clone();
        stream.extend_from_slice(&sector_data(&set, golden, lba + 1));
        stream.truncate(size);
        (first, stream)
    }

    #[test]
    fn the_shipped_trust_holds_the_us_dumps() {
        let shipped = Trust::shipped();
        assert_eq!(shipped.n64_canonical_sha1, US_SHA1);
        assert_eq!(shipped.disc, US_DISC);
        assert_eq!(shipped.fingerprints, Some(Fingerprints::committed()));
        // Each strictness reads a disc by its own rules, and has a stable name.
        let install = Strictness::Install(&shipped);
        assert_eq!(Strictness::Script.as_str(), "script");
        assert_eq!(install.as_str(), "install");
        assert_eq!(*Strictness::Script.disc_rules(), SCRIPT_RULES);
        assert_eq!(*install.disc_rules(), INSTALL_RULES);
    }

    #[test]
    fn only_the_dump_the_trust_names_passes() {
        held(Verdict::Supported).unwrap();
        for refusal in [
            DumpRefusal::OtherGame,
            DumpRefusal::OtherRevision,
            DumpRefusal::HashMismatch,
        ] {
            let error = held(Verdict::Refused(refusal)).unwrap_err();
            assert!(
                matches!(error, ExtractError::Refused { refusal: given } if given == refusal),
                "{error:?}"
            );
            assert_eq!(error.rule(), refusal.as_str());
            let words = format!(
                "the dump is refused for an installation: {}",
                refusal.as_str()
            );
            assert_eq!(error.to_string(), words);
        }
        // A verdict that judged nothing passes nothing.
        let error = held(Verdict::Unchecked).unwrap_err();
        assert_eq!(error.rule(), DumpRefusal::Unrecognized.as_str());
    }

    #[test]
    fn install_refuses_another_n64_rom_before_writing() {
        let vectors = n64_vectors();
        let image = frozen(&vectors, "default");
        let folders = Folders::new();
        let shipped = Trust::shipped();
        let install = Strictness::Install(&shipped);
        let settings = ExtractSettings::SHIPPED;
        // The default image's header gives four NULs as its game code, no cartridge of the game's;
        // the US cartridge's code on other bytes, a code of another region or revision of the
        // game, or of another game, are each refused for what they say.
        let codes = [
            (None, DumpRefusal::OtherGame),
            (Some(*b"NGXE"), DumpRefusal::HashMismatch),
            (Some(*b"NGXP"), DumpRefusal::OtherRevision),
            (Some(*b"NGDJ"), DumpRefusal::OtherRevision),
            (Some(*b"NABE"), DumpRefusal::OtherGame),
        ];
        for (code, refusal) in codes {
            let mut rom = image.clone();
            if let Some(code) = code {
                rom[GAME_CODE].copy_from_slice(&code);
            }
            let error = folders.refused_rom(&rom, &install, &settings);
            assert!(
                matches!(error, ExtractError::Refused { refusal: given } if given == refusal),
                "{code:?}: {error:?}"
            );
        }

        // A trust that holds another cartridge refuses the image too, before a file of its
        // ROMDISK is decoded; the extractor's rules take it.
        let other = Trust {
            n64_canonical_sha1: Sha1Digest::of(b"another cartridge"),
            ..Trust::shipped()
        };
        let error = folders.refused_rom(&image, &Strictness::Install(&other), &settings);
        assert_eq!(error.rule(), DumpRefusal::OtherGame.as_str());
        let rom = N64Rom::from_bytes(named("source.z64"), image).unwrap();
        let cancel = Cancel::default();
        let progress = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        check_n64(&rom, &Strictness::Install(&other), &job).unwrap_err();
        let phases = phases_of(progress.told());
        assert_eq!(phases.len(), 1);
        assert!(phases[0].1.is_empty(), "{phases:?}");
        check_n64(&rom, &Strictness::Script, &quiet(&cancel)).unwrap();
    }

    #[test]
    fn install_takes_a_rom_whose_digest_the_trust_holds() {
        let vectors = n64_vectors();
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        // Neither image's header names a cartridge of the game; held to its own digest, each is
        // taken all the same, and writes the tree the extractor writes of it.
        for image in ["default", "mixed"] {
            let case = extraction_case(&vectors, image);
            let bytes = frozen(&vectors, image);
            let rom = N64Rom::from_bytes(named(text(&case["name"])), bytes.clone()).unwrap();
            assert!(!rom.header().game_code.starts_with(b"NG"), "{image}");
            let trust = Trust {
                n64_canonical_sha1: rom.canonical_sha1(),
                ..Trust::shipped()
            };
            let dump = folders.dump(OsStr::new(text(&case["name"])), &bytes);
            let mode = Strictness::Install(&trust);
            let settings = ExtractSettings::SHIPPED;
            let (staging, _) = stage_n64(&dump, &folders.parent, &mode, &settings, &job).unwrap();
            let tree = tree_listing(&n64_root().path_under(staging.path()));
            assert_eq!(tree, listed(&vectors["trees"][image]), "{image}");
        }
    }

    #[test]
    fn install_refuses_another_disc_revision_before_writing() {
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        let trust = disc_trust();
        let install = Strictness::Install(&trust);
        let folders = Folders::new();
        let cancel = Cancel::default();
        let boot_lba = u64::from(BOOT_LBA);
        let boot = sector_data(&installable_disc(), &golden, boot_lba);
        // The boot area of another product, version or area, each sector whole: refused before
        // the disc's native table is looked at, and taken by the extractor's rules.
        let fields: [(Range<usize>, &[u8]); 3] = [
            (PRODUCT, b"T9710P    "),
            (VERSION, b"V1.002"),
            (AREA, b"J       "),
        ];
        for (field, other) in fields {
            let mut changed = boot.clone();
            changed[field.clone()].copy_from_slice(other);
            let set = with_sector(installable_disc(), &golden, boot_lba, &changed);
            let error = folders.refused_disc(&set, &install);
            assert_eq!(error.rule(), "otherRevision", "{field:?}");
            let words = "the dump is refused for an installation: otherRevision";
            assert_eq!(error.to_string(), words);
            let sheet = folders.disc(&set);
            check_dreamcast(&sheet, &Strictness::Script, &quiet(&cancel)).unwrap();
        }
        // The small set's boot area names no version and no area. Its disc holds no native
        // table, but its identity is refused first.
        let small = DiscSet::small(Edc::Computed);
        assert_eq!(
            folders.refused_disc(&small, &install).rule(),
            "otherRevision"
        );
        // A trust that names another revision refuses the disc the vectors' trust names.
        let other = Trust {
            disc: DiscIdentity {
                version: *b"V1.002",
                ..US_DISC
            },
            ..disc_trust()
        };
        let error = folders.refused_disc(&installable_disc(), &Strictness::Install(&other));
        assert_eq!(error.rule(), "otherRevision");
    }

    #[test]
    fn install_needs_every_track_file() {
        // The full set leaves out its audio track's file, which the extractor's rules never look
        // for and an installation's do.
        let set = DiscSet::full(Edc::Computed);
        let folders = Folders::new();
        let trust = disc_trust();
        let error = folders.refused_disc(&set, &Strictness::Install(&trust));
        assert_eq!(error.rule(), "missingTrack", "{error}");
        let cancel = Cancel::default();
        let sheet = folders.disc(&set);
        check_dreamcast(&sheet, &Strictness::Script, &quiet(&cancel)).unwrap();
    }

    #[test]
    fn install_refuses_a_sector_whose_edc_fails() {
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        let tree = listed(&vectors["sets"]["full"]["tree"]);
        let stream = format!("{DISC_FOLDER}/{STREAM}");
        // A bit of the stream's first sector's user data changed, its code left as it was.
        let lba = disc_file_lba(&golden, STREAM);
        let (track, place) = sector_place(&golden, lba);
        let set = installable_disc();
        let at = place + (gdl_media::USER_DATA.start + CHANGED_AT) as u64;
        let byte = set.file(&track).contents()[usize::try_from(at).unwrap()];
        let damaged = set.patched(&track, at, &[byte ^ 1]);
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let shipped = ExtractSettings::SHIPPED;

        // The extractor's rules read the sector as it now is: the tree holds the stream with
        // its bit changed.
        let sheet = folders.disc(&damaged);
        let script = Strictness::Script;
        let (staging, _) =
            stage_dreamcast(&sheet, &folders.parent, &script, &shipped, &job).unwrap();
        let written = tree_listing(&dreamcast_root().path_under(staging.path()));
        drop(staging);
        assert_ne!(digest_in(&written, &stream), digest_in(&tree, &stream));

        // An installation that holds the disc to its fingerprints reads every disc file before it
        // writes one: the sector is refused by its code before any staging is made.
        let trust = disc_trust();
        let error = folders.refused_disc(&damaged, &Strictness::Install(&trust));
        assert_eq!(error.rule(), "sectorEdc");
        assert_eq!(error.to_string(), fails_its_code(lba));

        // One that holds the disc to its identity alone reads the stream first as it writes it,
        // and refuses it there, within the staging, which goes once dropped.
        let identity_only = Trust {
            fingerprints: None,
            ..disc_trust()
        };
        let mode = Strictness::Install(&identity_only);
        let sheet = folders.disc(&damaged);
        let before = folders.before();
        let mut checked = check_dreamcast(&sheet, &mode, &job).unwrap();
        let staging = Staging::create_in(&folders.parent, STAGING_PREFIX).unwrap();
        let error = extract_dreamcast(&mut checked, &staging, &dreamcast_root(), &job).unwrap_err();
        assert_eq!(error.rule(), "sectorEdc");
        assert_eq!(error.to_string(), fails_its_code(lba));
        folders.assert_outside_unchanged(&before);
        drop(staging);
        drop(checked);
        folders.assert_staging_gone(&before);
    }

    #[test]
    fn install_checks_every_file_against_the_fingerprints() {
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        let files = fingerprint_files(&listed(&vectors["sets"]["full"]["tree"]));
        let disk_rom = format!("{DISC_FOLDER}/DISK.ROM");
        let empty_bin = format!("{DISC_FOLDER}/EMPTY.BIN");
        let other_file = format!("{DISC_FOLDER}/OTHER.BIN");
        let native_file = "native/levels/a.wad".to_owned();
        let another = Sha256Digest::of(b"another");
        // The trust of the vectors' set, its fingerprints those of `files`.
        let trust_of = |files: Vec<(String, Sha256Digest)>| Trust {
            fingerprints: Some(Fingerprints::from_files(files).unwrap()),
            ..disc_trust()
        };
        let without = |path: &str| -> Vec<(String, Sha256Digest)> {
            let kept = files.iter().filter(|(held, _)| held != path);
            kept.cloned().collect()
        };
        let with = |path: &str| -> Vec<(String, Sha256Digest)> {
            let added = (path.to_owned(), another);
            files.iter().cloned().chain([added]).collect()
        };
        let changed = |path: &str| -> Vec<(String, Sha256Digest)> {
            let rows = files.iter().map(|(held, digest)| {
                let digest = if held == path { another } else { *digest };
                (held.clone(), digest)
            });
            rows.collect()
        };
        let key_of = |path: &str| Fingerprints::path_key(path).hex();
        let stream = format!("{DISC_FOLDER}/{STREAM}");
        let (first, _) = another_stream(&golden);
        let stream_lba = disc_file_lba(&golden, STREAM);
        let another_stream = with_sector(installable_disc(), &golden, stream_lba, &first);
        let boot_area = with_sector(
            installable_disc(),
            &golden,
            u64::from(BOOT_LBA) + 1,
            b"another boot area",
        );
        let cases = [
            (
                "a disc file without a fingerprint",
                trust_of(without(&disk_rom)),
                installable_disc(),
                "fingerprintMissing",
                disk_rom.clone(),
            ),
            (
                "IP.BIN without a fingerprint",
                trust_of(without(IP_BIN)),
                installable_disc(),
                "fingerprintMissing",
                IP_BIN.to_owned(),
            ),
            (
                "a fingerprint of no disc file",
                trust_of(with(&other_file)),
                installable_disc(),
                "fingerprintUnexpected",
                key_of(&other_file),
            ),
            (
                "a native file's fingerprint",
                trust_of(with(&native_file)),
                installable_disc(),
                "fingerprintUnexpected",
                key_of(&native_file),
            ),
            (
                "an empty file of another digest",
                trust_of(changed(&empty_bin)),
                installable_disc(),
                "fingerprintDigest",
                empty_bin.clone(),
            ),
            (
                "a byte of a file changed",
                disc_trust(),
                another_stream,
                "fingerprintDigest",
                stream,
            ),
            (
                "the boot area changed",
                disc_trust(),
                boot_area,
                "fingerprintDigest",
                IP_BIN.to_owned(),
            ),
        ];
        let folders = Folders::new();
        for (case, trust, set, rule, path) in cases {
            let error = folders.refused_disc(&set, &Strictness::Install(&trust));
            assert_eq!(error.rule(), rule, "{case}: {error}");
            assert_eq!(error.to_string(), format!("{path}: {rule}"), "{case}");
        }
    }

    #[test]
    fn a_dump_that_changes_between_the_passes_is_refused() {
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        let lba = disc_file_lba(&golden, STREAM);
        let (track, place) = sector_place(&golden, lba);
        let set = installable_disc();
        let path = format!("{DISC_FOLDER}/{STREAM}");
        // The stream's first sector, holding other bytes under a header that places it where it
        // is and its code worked out anew, written over the dump as the extraction begins to
        // write: after the check's read, and before the extraction's.
        let (first, stream) = another_stream(&golden);
        let sector = raw_sector(u32::try_from(lba).unwrap(), &first, Edc::Computed);
        let trust = disc_trust();
        let cancel = Cancel::default();
        let shipped = ExtractSettings::SHIPPED;
        for mode in [Strictness::Install(&trust), Strictness::Script] {
            // The dump lies apart from the test's dump folder, since the test changes it: only
            // what lies outside the staging is held to stay as it was.
            let dumps = tempfile::tempdir().unwrap();
            let sheet = set.write(dumps.path()).unwrap();
            let folders = Folders::new();
            let before = folders.before();
            let progress = ChangedAtUnpack {
                path: dumps.path().join(&track),
                place,
                bytes: sector.clone(),
            };
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let staged = stage_dreamcast(&sheet, &folders.parent, &mode, &shipped, &job);
            match mode {
                // An installation held the stream to its fingerprint as it checked the disc, and
                // holds what it writes to what it checked.
                Strictness::Install(_) => {
                    let error = staged.unwrap_err();
                    assert_eq!(error.rule(), "changed", "{error:?}");
                    let words = format!("the dump changed while it was read: {path}");
                    assert_eq!(error.to_string(), words);
                }
                // The extractor's rules read the stream once, as they write it, and take it as it
                // then is.
                Strictness::Script => {
                    let (staging, _) = staged.unwrap();
                    let listing = tree_listing(&dreamcast_root().path_under(staging.path()));
                    assert_eq!(digest_in(&listing, &path), sha256_hex(&stream));
                }
            }
            folders.assert_staging_gone(&before);
        }
    }

    #[test]
    fn install_and_script_write_the_same_tree() {
        let folders = Folders::new();
        let cancel = Cancel::default();
        let job = quiet(&cancel);
        let shipped = ExtractSettings::SHIPPED;

        // The N64 default image, under the extractor's rules and under a trust of its own digest.
        let vectors = n64_vectors();
        let case = extraction_case(&vectors, "default");
        let image = frozen(&vectors, "default");
        let name = text(&case["name"]);
        let rom = N64Rom::from_bytes(named(name), image.clone()).unwrap();
        let trust = Trust {
            n64_canonical_sha1: rom.canonical_sha1(),
            ..Trust::shipped()
        };
        let dump = folders.dump(OsStr::new(name), &image);
        let mut written = Vec::new();
        for mode in [Strictness::Script, Strictness::Install(&trust)] {
            let (staging, extraction) =
                stage_n64(&dump, &folders.parent, &mode, &shipped, &job).unwrap();
            let tree = tree_listing(&n64_root().path_under(staging.path()));
            written.push((tree, extraction));
        }
        assert_eq!(written[0], written[1]);
        assert_eq!(written[0].0, listed(&vectors["trees"]["default"]));

        // The full set as a pressed disc's dump holds it, under the extractor's rules and under
        // the vectors' trust.
        let vectors = dreamcast_vectors();
        let sheet = folders.disc(&installable_disc());
        let trust = disc_trust();
        let mut written = Vec::new();
        for mode in [Strictness::Script, Strictness::Install(&trust)] {
            let (staging, extraction) =
                stage_dreamcast(&sheet, &folders.parent, &mode, &shipped, &job).unwrap();
            let tree = tree_listing(&dreamcast_root().path_under(staging.path()));
            written.push((tree, extraction));
        }
        assert_eq!(written[0], written[1]);
        assert_eq!(written[0].0, listed(&vectors["sets"]["full"]["tree"]));
    }

    #[test]
    fn install_reads_the_disc_twice_and_script_once() {
        let vectors = dreamcast_vectors();
        let golden = full_golden(&vectors);
        // The disc's files that hold bytes, in the order the walk meets them, each of which the
        // progress names as it is read; and how many bytes the disc's files hold together.
        let rows = array(&golden["files"]);
        let read: Vec<String> = rows
            .iter()
            .filter(|row| row["size"] != 0)
            .map(|row| format!("{DISC_FOLDER}/{}", text(&row["path"])))
            .collect();
        let total: u64 = rows.iter().map(|row| row["size"].as_u64().unwrap()).sum();
        let folders = Folders::new();
        let sheet = folders.disc(&installable_disc());
        let trust = disc_trust();
        let identity_only = Trust {
            fingerprints: None,
            ..disc_trust()
        };
        let cancel = Cancel::default();
        let shipped = ExtractSettings::SHIPPED;
        let modes = [
            (Strictness::Script, false),
            (Strictness::Install(&identity_only), false),
            (Strictness::Install(&trust), true),
        ];
        for (mode, twice) in modes {
            let progress = Recorded::default();
            let job = Job {
                cancel: &cancel,
                progress: &progress,
            };
            let (staging, _) =
                stage_dreamcast(&sheet, &folders.parent, &mode, &shipped, &job).unwrap();
            drop(staging);
            let phases = phases_of(progress.told());
            let names: Vec<&str> = phases.iter().map(|(name, _)| *name).collect();
            assert_eq!(names, [READ_DISC.0, UNPACK.0, CHECK.0], "{}", mode.as_str());
            // Every disc file is read as it is written.
            let unpacked = items_of(&phases[1].1);
            let disc_folder = format!("{DISC_FOLDER}/");
            let written: Vec<&String> = unpacked
                .iter()
                .filter(|item| item.starts_with(&disc_folder))
                .collect();
            assert_eq!(
                written,
                read.iter().collect::<Vec<_>>(),
                "{}",
                mode.as_str()
            );
            // Held to fingerprints, each is read once before, as the disc is checked, the check
            // telling what it has read of them all; otherwise the check reads none but SIZES.ROM,
            // and tells nothing.
            let (_, checked) = &phases[0];
            if twice {
                assert_eq!(items_of(checked), read);
                assert!(checked.iter().all(|(_, of, _)| *of == total));
                let done: Vec<u64> = checked.iter().map(|(done, _, _)| *done).collect();
                assert!(done.windows(2).all(|pair| pair[0] < pair[1]));
                assert_eq!(done.last(), Some(&total));
            } else {
                assert!(checked.is_empty(), "{}: {checked:?}", mode.as_str());
            }
        }
    }
}
