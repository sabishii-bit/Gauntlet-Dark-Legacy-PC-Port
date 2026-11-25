//! An update check's answer, kept for an hour so that starting the launcher again does not spend
//! GitHub's hourly budget of unauthenticated requests: what the feed answered, when, and to which
//! launcher on which platform. Its JSON form carries a stamp, as every state file of the
//! launcher's does, and keeps the keys a later launcher adds. Only answers are kept: a check that
//! failed is simply asked again. A release a kept answer names is offered again only as GitHub's
//! feed could have given it to the launcher that asked.

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use crate::http::Origins;
use crate::{Platform, Release, Version};

/// The schema of an update check's JSON form that this launcher reads and writes.
pub const UPDATE_CHECK_SCHEMA: u64 = 1;

/// How long an answer is kept, in seconds from its check.
pub const UPDATE_CHECK_LIFETIME: u64 = 3600;

/// How far ahead of the clock, in seconds, a check may say it was made and its answer still be
/// kept: past that the clock was set back, and the answer is asked for again.
pub const MAX_CLOCK_SKEW: u64 = 300;

/// The first schema of every kind of state file.
const FIRST_SCHEMA: u64 = 1;

/// The lowest launcher version allowed to change an update check of schema 1: the first
/// launcher's, whichever launcher wrote it.
const MIN_LAUNCHER: &str = "1.0.0-beta";

/// An update check's answer, with when and for whom it was asked.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateCheck {
    /// The shape of this JSON form: [`UPDATE_CHECK_SCHEMA`] for this launcher's.
    pub schema: u64,
    /// The lowest launcher version allowed to change it.
    pub min_launcher: String,
    /// When the check was made, in seconds since the Unix epoch.
    pub checked_at: u64,
    /// The version of the launcher that asked, as its text.
    pub launcher: String,
    /// The platform it asked for, by its name in a release's metadata.
    pub platform: String,
    /// What the feed answered.
    pub answer: Answer,
    /// The keys a later launcher wrote, kept as they are.
    #[serde(flatten)]
    pub other: Map<String, Value>,
}

/// What an update check found.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum Answer {
    /// The feed holds no release newer than the launcher that asked.
    UpToDate,
    /// The newest release above the launcher that asked.
    Available {
        /// The release, as the feed's scan gave it.
        release: Box<Release>,
    },
    /// GitHub refused the check, its limit of unauthenticated requests spent.
    RateLimited {
        /// When the limit is renewed, in seconds since the Unix epoch.
        until: u64,
    },
}

impl UpdateCheck {
    /// The answer of a check made at `now` by launcher `launcher` for `platform`.
    pub fn new(now: u64, launcher: &Version, platform: Platform, answer: Answer) -> UpdateCheck {
        UpdateCheck {
            schema: UPDATE_CHECK_SCHEMA,
            min_launcher: MIN_LAUNCHER.to_owned(),
            checked_at: now,
            launcher: launcher.to_string(),
            platform: platform.as_str().to_owned(),
            answer,
            other: Map::new(),
        }
    }

    /// The kept answer, when it was asked by exactly this launcher version, build metadata
    /// included, for `platform`, and is still fresh at `now`: made no more than [`MAX_CLOCK_SKEW`]
    /// ahead of it and less than [`UPDATE_CHECK_LIFETIME`] before it, and, for a rate limit, not
    /// past its renewal. A release it names must answer that launcher as GitHub's feed would:
    /// one for `platform`, above the launcher's version, its files named as the new line names
    /// them and at this repository's own download URLs. A check of a later schema, or one that
    /// asks for a later launcher, is not this launcher's to read.
    pub fn fresh(&self, now: u64, launcher: &Version, platform: Platform) -> Option<&Answer> {
        let readable = self
            .lowest_launcher()
            .is_some_and(|lowest| lowest <= *launcher);
        let asker = self.launcher == launcher.to_string() && self.platform == platform.as_str();
        let recent = self.checked_at <= now.saturating_add(MAX_CLOCK_SKEW)
            && now < self.checked_at.saturating_add(UPDATE_CHECK_LIFETIME);
        let answers = match &self.answer {
            Answer::Available { release } => {
                release.platform == platform
                    && release.version > *launcher
                    && release.check_origin(&Origins::GitHub).is_ok()
            }
            Answer::UpToDate | Answer::RateLimited { .. } => true,
        };
        let renewed = match self.answer {
            Answer::RateLimited { until } => now >= until,
            Answer::UpToDate | Answer::Available { .. } => false,
        };
        (readable && asker && recent && answers && !renewed).then_some(&self.answer)
    }

    /// The check `value` holds, as its JSON form writes one; `None` for anything else, and for a
    /// check of a later schema, whose form this launcher does not know.
    pub fn from_value(value: &Value) -> Option<UpdateCheck> {
        let check = UpdateCheck::deserialize(value).ok()?;
        check.lowest_launcher().is_some().then_some(check)
    }

    /// The lowest launcher version allowed to change the check, when its stamp is one this
    /// launcher reads: a schema it knows, and a version.
    fn lowest_launcher(&self) -> Option<Version> {
        if !(FIRST_SCHEMA..=UPDATE_CHECK_SCHEMA).contains(&self.schema) {
            return None;
        }
        Version::parse(&self.min_launcher).ok()
    }
}

/// A SHA-256 digest in JSON as its text, 64 lowercase hexadecimal digits, the form GitHub's digests
/// and the checksum lists give it; any other text is refused.
pub(crate) mod digest_text {
    use gdl_media::Sha256Digest;
    use serde::{Deserialize, Deserializer, Serializer, de};

    /// Writes `digest` as its text.
    pub(crate) fn serialize<S: Serializer>(
        digest: &Sha256Digest,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&digest.hex())
    }

    /// Reads a digest from its text.
    pub(crate) fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<Sha256Digest, D::Error> {
        let text = String::deserialize(deserializer)?;
        Sha256Digest::from_hex(&text).ok_or_else(|| {
            de::Error::custom(format_args!(
                "{text:?} is not a SHA-256 digest in lowercase hexadecimal"
            ))
        })
    }
}

#[cfg(test)]
mod tests {
    use std::fs;

    use gdl_files::{MAX_JSON_LEN, write_json_atomic};
    use gdl_media::Sha256Digest;
    use serde_json::{Value, json};

    use super::{
        Answer, MAX_CLOCK_SKEW, MIN_LAUNCHER, UPDATE_CHECK_LIFETIME, UPDATE_CHECK_SCHEMA,
        UpdateCheck,
    };
    use crate::{
        Asset, MAX_ASSET_LEN, MAX_LIST_LEN, Platform, REPOSITORY, Release, Version,
        checksums_asset, launcher_asset, signature_asset,
    };

    /// When the tests' checks are made, in seconds since the Unix epoch.
    const CHECKED_AT: u64 = 1_764_000_000;

    /// The launcher the tests' checks are made by.
    const RUNNING: &str = "1.0.0-beta";

    /// The release the tests' checks find.
    const NEWER: &str = "1.0.0-beta.1";

    /// The length and digest the tests' releases publish for their launcher file.
    const LAUNCHER_FILE: (u64, [u8; 32]) = (52_428_800, [0xa1; 32]);

    /// The length and digest the tests' releases publish for their checksum list.
    const CHECKSUMS_FILE: (u64, [u8; 32]) = (241, [0xb2; 32]);

    /// The length and digest the tests' releases publish for their signature.
    const SIGNATURE_FILE: (u64, [u8; 32]) = (312, [0xc3; 32]);

    /// The update check of a launcher with nothing newer to take, written whole.
    const UP_TO_DATE_JSON: &str = r#"{
  "schema": 1,
  "minLauncher": "1.0.0-beta",
  "checkedAt": 1764000000,
  "launcher": "1.0.0-beta",
  "platform": "windows-x64",
  "answer": {
    "kind": "upToDate"
  }
}
"#;

    /// The update check of a launcher that GitHub's limit stopped, written whole.
    const RATE_LIMITED_JSON: &str = r#"{
  "schema": 1,
  "minLauncher": "1.0.0-beta",
  "checkedAt": 1764000000,
  "launcher": "1.0.0-beta",
  "platform": "windows-x64",
  "answer": {
    "kind": "rateLimited",
    "until": 1764003600
  }
}
"#;

    /// The update check that found [`NEWER`], unsigned and without notes, written whole.
    const AVAILABLE_JSON: &str = r#"{
  "schema": 1,
  "minLauncher": "1.0.0-beta",
  "checkedAt": 1764000000,
  "launcher": "1.0.0-beta",
  "platform": "windows-x64",
  "answer": {
    "kind": "available",
    "release": {
      "version": "1.0.0-beta.1",
      "tag": "v1.0.0-beta.1",
      "platform": "windows-x64",
      "launcher": {
        "name": "GauntletDarkLegacy-1.0.0-beta.1-windows-x64-launcher.exe",
        "url": "https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/v1.0.0-beta.1/GauntletDarkLegacy-1.0.0-beta.1-windows-x64-launcher.exe",
        "size": 52428800,
        "sha256": "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
      },
      "checksums": {
        "name": "GauntletDarkLegacy-1.0.0-beta.1-windows-x64.sha256",
        "url": "https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/v1.0.0-beta.1/GauntletDarkLegacy-1.0.0-beta.1-windows-x64.sha256",
        "size": 241,
        "sha256": "b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2"
      }
    }
  }
}
"#;

    fn version(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
    }

    /// The asset `name` of the release `release`, published as holding `(size, digest)`.
    fn asset(release: &Version, name: String, (size, digest): (u64, [u8; 32])) -> Asset {
        Asset {
            url: format!("https://github.com/{REPOSITORY}/releases/download/v{release}/{name}"),
            name,
            size,
            sha256: Sha256Digest(digest),
        }
    }

    /// The release `text` for `platform`, as GitHub's feed gives it: signed and with notes when
    /// `whole`, else with neither.
    fn release_of(text: &str, platform: Platform, whole: bool) -> Release {
        let release = version(text);
        Release {
            tag: format!("v{text}"),
            platform,
            launcher: asset(&release, launcher_asset(&release, platform), LAUNCHER_FILE),
            checksums: asset(
                &release,
                checksums_asset(&release, platform),
                CHECKSUMS_FILE,
            ),
            signature: whole.then(|| {
                let name = signature_asset(&release, platform);
                asset(&release, name, SIGNATURE_FILE)
            }),
            notes: whole.then(|| "## Added\n- A new realm\n\n## Fixed\n- A crash\n".to_owned()),
            version: release,
        }
    }

    /// [`NEWER`]'s Windows release: signed and with notes when `whole`, else with neither.
    fn release(whole: bool) -> Release {
        release_of(NEWER, Platform::WindowsX64, whole)
    }

    /// The answer that found the release [`release`] gives for `whole`.
    fn available(whole: bool) -> Answer {
        Answer::Available {
            release: Box::new(release(whole)),
        }
    }

    /// `answer`, as the running launcher's check on Windows made at [`CHECKED_AT`].
    fn check(answer: Answer) -> UpdateCheck {
        UpdateCheck::new(CHECKED_AT, &version(RUNNING), Platform::WindowsX64, answer)
    }

    /// The answer `check` keeps at `now` for the running launcher on Windows.
    fn kept(check: &UpdateCheck, now: u64) -> Option<&Answer> {
        check.fresh(now, &version(RUNNING), Platform::WindowsX64)
    }

    /// `check`'s JSON form.
    fn json_of(check: &UpdateCheck) -> Value {
        serde_json::to_value(check).expect("an update check has a JSON form")
    }

    /// `value` with the key `pointer` names set to `with`, or taken out given `None`.
    fn edited(value: &Value, pointer: &str, with: Option<Value>) -> Value {
        let mut value = value.clone();
        let (parent, key) = pointer.rsplit_once('/').expect(pointer);
        let parent = value.pointer_mut(parent).and_then(Value::as_object_mut);
        let parent = parent.expect(pointer);
        match with {
            Some(with) => {
                parent.insert(key.to_owned(), with);
            }
            None => {
                parent.remove(key).expect(pointer);
            }
        }
        value
    }

    #[test]
    fn a_release_round_trips_through_its_json() {
        for whole in [true, false] {
            let made = check(available(whole));
            let value = json_of(&made);
            assert_eq!(UpdateCheck::from_value(&value).as_ref(), Some(&made));
            let release = &value["answer"]["release"];
            assert_eq!(value["answer"]["kind"], "available");
            assert_eq!(release["version"], NEWER);
            assert_eq!(release["tag"], format!("v{NEWER}"));
            assert_eq!(release["platform"], "windows-x64");
            assert_eq!(release["launcher"]["sha256"], "a1".repeat(32));
            assert_eq!(release["launcher"]["size"], LAUNCHER_FILE.0);
            // What a release does not carry is left out, and an absent value reads as none.
            for key in ["signature", "notes"] {
                assert_eq!(release.get(key).is_some(), whole, "{key} of {release}");
            }
            let nulls = edited(&value, "/answer/release/signature", Some(Value::Null));
            let nulls = edited(&nulls, "/answer/release/notes", Some(Value::Null));
            assert_eq!(
                UpdateCheck::from_value(&nulls),
                Some(check(available(false)))
            );
        }
        let value = json_of(&check(available(true)));
        let digest = "a1".repeat(32);
        for (pointer, with) in [
            (
                "/answer/release/launcher/sha256",
                json!(digest.to_uppercase()),
            ),
            (
                "/answer/release/checksums/sha256",
                json!(format!("sha256:{}", "b2".repeat(32))),
            ),
            ("/answer/release/signature/sha256", json!(&digest[1..])),
            ("/answer/release/launcher/sha256", json!(null)),
            ("/answer/release/version", json!("1.0")),
            ("/answer/release/version", json!(format!("v{NEWER}"))),
            ("/answer/release/version", json!(1)),
            ("/answer/release/platform", json!("macos-x64")),
            ("/answer/release/platform", json!("Windows-x64")),
            ("/answer/release/launcher/size", json!(-1)),
            ("/answer/release/launcher/size", json!(1.5)),
            ("/answer/release/launcher/size", json!("241")),
            ("/answer/release/signature", json!({})),
            ("/answer/release/notes", json!(["## Fixed"])),
            ("/answer/release", json!(null)),
            ("/answer/kind", json!("downgrade")),
            ("/answer/kind", json!("UpToDate")),
            ("/answer", json!("upToDate")),
            ("/checkedAt", json!(-1)),
            ("/checkedAt", json!(f64::from(u32::MAX))),
            ("/launcher", json!(1)),
            ("/platform", json!(null)),
        ] {
            let edited = edited(&value, pointer, Some(with.clone()));
            assert_eq!(
                UpdateCheck::from_value(&edited),
                None,
                "{pointer} as {with}"
            );
        }
        for pointer in [
            "/answer/release/launcher/url",
            "/answer/release/checksums",
            "/answer/release/tag",
            "/answer/kind",
            "/answer",
            "/checkedAt",
            "/launcher",
            "/platform",
        ] {
            assert_eq!(
                UpdateCheck::from_value(&edited(&value, pointer, None)),
                None,
                "without {pointer}"
            );
        }
        for value in [json!(null), json!([]), json!("upToDate"), json!({})] {
            assert_eq!(UpdateCheck::from_value(&value), None, "{value}");
        }
    }

    #[test]
    fn an_answer_is_kept_an_hour() {
        assert_eq!(UPDATE_CHECK_LIFETIME, 3600);
        for answer in [Answer::UpToDate, available(true)] {
            let made = check(answer.clone());
            assert_eq!(made.checked_at, CHECKED_AT);
            for now in [
                CHECKED_AT,
                CHECKED_AT + 1,
                CHECKED_AT + UPDATE_CHECK_LIFETIME - 1,
            ] {
                assert_eq!(kept(&made, now), Some(&answer), "at {now}");
            }
            for now in [CHECKED_AT + UPDATE_CHECK_LIFETIME, u64::MAX] {
                assert_eq!(kept(&made, now), None, "at {now}");
            }
        }
    }

    #[test]
    fn a_clock_moved_back_past_the_skew_makes_it_stale() {
        assert_eq!(MAX_CLOCK_SKEW, 300);
        let made = check(Answer::UpToDate);
        assert_eq!(
            kept(&made, CHECKED_AT - MAX_CLOCK_SKEW),
            Some(&Answer::UpToDate)
        );
        for now in [
            CHECKED_AT - MAX_CLOCK_SKEW - 1,
            CHECKED_AT - UPDATE_CHECK_LIFETIME,
            0,
        ] {
            assert_eq!(kept(&made, now), None, "at {now}");
        }
        // A check said to be made at the clock's end is held to the same rules, without
        // overflowing them.
        let mut last = made;
        last.checked_at = u64::MAX;
        assert_eq!(
            kept(&last, u64::MAX - MAX_CLOCK_SKEW),
            Some(&Answer::UpToDate)
        );
        assert_eq!(kept(&last, u64::MAX - MAX_CLOCK_SKEW - 1), None);
    }

    #[test]
    fn another_launcher_or_platform_reads_none() {
        let made = check(available(false));
        assert!(kept(&made, CHECKED_AT).is_some());
        // The version is compared as text: another build of the same version asks afresh.
        for other in ["1.0.0-beta.1", "1.0.0-beta+build.1", "1.0.0-alpha", "0.9.0"] {
            let other = version(other);
            assert_eq!(
                made.fresh(CHECKED_AT, &other, Platform::WindowsX64),
                None,
                "{other}"
            );
        }
        assert_eq!(
            made.fresh(CHECKED_AT, &version(RUNNING), Platform::LinuxX64),
            None
        );
        let linux = UpdateCheck::new(
            CHECKED_AT,
            &version(RUNNING),
            Platform::LinuxX64,
            Answer::UpToDate,
        );
        assert_eq!(linux.platform, "linux-x64");
        assert_eq!(
            linux.fresh(CHECKED_AT, &version(RUNNING), Platform::LinuxX64),
            Some(&Answer::UpToDate)
        );
        assert_eq!(kept(&linux, CHECKED_AT), None);
        for (field, text) in [("launcher", "v1.0.0-beta"), ("platform", "Windows-x64")] {
            let edited = edited(&json_of(&made), &format!("/{field}"), Some(json!(text)));
            let read = UpdateCheck::from_value(&edited).expect("any text reads");
            assert_eq!(kept(&read, CHECKED_AT), None, "{field} {text}");
        }
    }

    #[test]
    fn a_rate_limit_is_kept_until_its_reset_or_the_hour() {
        let limited = |until| check(Answer::RateLimited { until });
        let soon = limited(CHECKED_AT + 60);
        assert_eq!(
            kept(&soon, CHECKED_AT + 59),
            Some(&Answer::RateLimited {
                until: CHECKED_AT + 60
            })
        );
        assert_eq!(kept(&soon, CHECKED_AT + 60), None);
        let late = limited(CHECKED_AT + 2 * UPDATE_CHECK_LIFETIME);
        assert!(kept(&late, CHECKED_AT + UPDATE_CHECK_LIFETIME - 1).is_some());
        assert_eq!(kept(&late, CHECKED_AT + UPDATE_CHECK_LIFETIME), None);
        for until in [CHECKED_AT, CHECKED_AT - 1, 0] {
            assert_eq!(kept(&limited(until), CHECKED_AT), None, "until {until}");
        }
    }

    #[test]
    fn a_kept_release_must_answer_the_launcher_that_asked() {
        let offered = |release: Release| {
            let made = check(Answer::Available {
                release: Box::new(release),
            });
            kept(&made, CHECKED_AT).is_some()
        };
        assert!(offered(release(true)));
        assert!(offered(release(false)));
        // A release for another platform, or none above the launcher that asked, is an answer to
        // another question.
        assert!(!offered(release_of(NEWER, Platform::LinuxX64, true)));
        for text in [RUNNING, "1.0.0-beta+build.1", "1.0.0-alpha.3", "0.9.0"] {
            let below = release_of(text, Platform::WindowsX64, true);
            assert!(!offered(below), "{text}");
        }

        // Nor is a release whose files are not this repository's own, as GitHub's list names
        // them.
        type Edit = fn(&mut Release);
        let edits: [(&str, Edit); 11] = [
            ("a launcher file of another repository", |release| {
                release.launcher.url = release.launcher.url.replace(REPOSITORY, "a/b");
            }),
            ("a list at another tag", |release| {
                let url = release
                    .checksums
                    .url
                    .replace("/v1.0.0-beta.1/", "/v1.0.0-beta.2/");
                release.checksums.url = url;
            }),
            ("a signature on a storage host", |release| {
                let signature = release.signature.as_mut().unwrap();
                signature.url = signature.url.replace(
                    "https://github.com/",
                    "https://objects.githubusercontent.com/",
                );
            }),
            ("a launcher file with a query", |release| {
                release.launcher.url.push_str("?x=1");
            }),
            ("a launcher file over plain HTTP", |release| {
                release.launcher.url = release.launcher.url.replacen("https", "http", 1);
            }),
            ("the other platform's launcher file", |release| {
                release.launcher.name = launcher_asset(&release.version, Platform::LinuxX64);
            }),
            ("a launcher file that leaves its folder", |release| {
                release.launcher.name = format!("../{}", release.launcher.name);
            }),
            ("a launcher file past its bound", |release| {
                release.launcher.size = MAX_ASSET_LEN + 1;
            }),
            ("an empty list", |release| release.checksums.size = 0),
            ("a signature past the list's bound", |release| {
                release.signature.as_mut().unwrap().size = MAX_LIST_LEN + 1;
            }),
            ("another tag", |release| {
                release.tag = "v1.0.0-beta.2".to_owned();
            }),
        ];
        for (what, edit) in edits {
            let mut edited = release(true);
            edit(&mut edited);
            assert!(!offered(edited), "{what}");
        }

        // Read back from its JSON form, a release changed there is read, and not offered.
        let value = json_of(&check(available(true)));
        let elsewhere = format!(
            "https://github.com/a/b/releases/download/v{NEWER}/{}",
            release(true).launcher.name
        );
        let moved = edited(
            &value,
            "/answer/release/launcher/url",
            Some(json!(elsewhere)),
        );
        let read = UpdateCheck::from_value(&moved).expect("its form is sound");
        assert_eq!(kept(&read, CHECKED_AT), None);
    }

    #[test]
    fn unknown_keys_are_kept() {
        let mut value = json_of(&check(Answer::UpToDate));
        value["offeredAt"] = json!(1_764_000_100);
        value["dismissed"] = json!({"version": NEWER, "times": [1, 2]});
        let read = UpdateCheck::from_value(&value).expect("a check with keys of a later launcher");
        assert_eq!(read.other.len(), 2);
        assert_eq!(read.other["dismissed"]["version"], NEWER);
        assert_eq!(kept(&read, CHECKED_AT), Some(&Answer::UpToDate));
        assert_eq!(json_of(&read), value);
        // They are written after the check's own keys.
        let text = serde_json::to_string(&read).expect("an update check has a JSON form");
        assert!(
            text.starts_with(r#"{"schema":1,"#)
                && text.contains(r#""answer":{"kind":"upToDate"},"#),
            "{text}"
        );
    }

    #[test]
    fn a_later_schema_is_not_read() {
        let value = json_of(&check(Answer::UpToDate));
        assert_eq!(value["schema"], UPDATE_CHECK_SCHEMA);
        assert_eq!(value["minLauncher"], MIN_LAUNCHER);
        let later = json!(UPDATE_CHECK_SCHEMA + 1);
        for (pointer, with) in [
            ("/schema", later),
            ("/schema", json!(0)),
            ("/schema", json!(-1)),
            ("/schema", json!(1.0)),
            ("/schema", json!("1")),
            ("/minLauncher", json!("1.0")),
            ("/minLauncher", json!("v1.0.0-beta")),
            ("/minLauncher", json!(null)),
            ("/minLauncher", json!(1)),
        ] {
            let edited = edited(&value, pointer, Some(with.clone()));
            assert_eq!(
                UpdateCheck::from_value(&edited),
                None,
                "{pointer} as {with}"
            );
        }
        for pointer in ["/schema", "/minLauncher"] {
            assert_eq!(
                UpdateCheck::from_value(&edited(&value, pointer, None)),
                None
            );
        }
        // One that asks for a later launcher is read, and kept only for such a launcher.
        let asks_later = edited(&value, "/minLauncher", Some(json!(NEWER)));
        let read = UpdateCheck::from_value(&asks_later).expect("a version reads");
        assert_eq!(kept(&read, CHECKED_AT), None);
        let mut newer = read;
        newer.launcher = NEWER.to_owned();
        assert_eq!(
            newer.fresh(CHECKED_AT, &version(NEWER), Platform::WindowsX64),
            Some(&Answer::UpToDate)
        );
        // A check made in code is held to the same stamp.
        for (schema, min_launcher) in [(UPDATE_CHECK_SCHEMA + 1, MIN_LAUNCHER), (1, "1.0")] {
            let mut made = check(Answer::UpToDate);
            made.schema = schema;
            made.min_launcher = min_launcher.to_owned();
            assert_eq!(kept(&made, CHECKED_AT), None, "{schema} {min_launcher}");
        }
    }

    #[test]
    fn the_json_is_byte_for_byte() {
        let folder = tempfile::tempdir().expect("a test folder");
        let path = folder.path().join("update-check.json");
        for (answer, expected) in [
            (Answer::UpToDate, UP_TO_DATE_JSON),
            (
                Answer::RateLimited {
                    until: CHECKED_AT + UPDATE_CHECK_LIFETIME,
                },
                RATE_LIMITED_JSON,
            ),
            (available(false), AVAILABLE_JSON),
        ] {
            let made = check(answer);
            write_json_atomic(&path, &made, MAX_JSON_LEN).expect("the check is written");
            let written = fs::read_to_string(&path).expect("the check reads back");
            assert_eq!(written, expected);
            let value: Value = serde_json::from_str(&written).expect("the check is JSON");
            assert_eq!(UpdateCheck::from_value(&value), Some(made));
        }
    }
}
