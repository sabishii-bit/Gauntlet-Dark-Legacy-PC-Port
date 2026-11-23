//! The release record at the top of a runtime, `build-info.json`, as scripts/release/release.py
//! writes it: the version, commit, platform and executable of the release, and the SHA-256 digest
//! of every other file the runtime holds, by name.

use std::collections::BTreeMap;
use std::fmt;

use gdl_files::{NameSet, PAYLOAD_NAMES, RelPath};
use gdl_media::Sha256Digest;
use gdl_release::{Platform, Version};
use serde_json::{Map, Value};

use super::{
    BUILD_INFO, DEFAULT_CONFIG, MAX_PAYLOAD_ENTRIES, PORTABLE_FLAG, PayloadError, VERSION_FILE,
    check_platform_root, check_root,
};

/// The record's key for the release's version.
const VERSION_KEY: &str = "version";

/// The record's key for the commit the release was built from.
const COMMIT_KEY: &str = "commit";

/// The record's key for the platform the release was built for.
const PLATFORM_KEY: &str = "platform";

/// The record's key for the game's executable.
const EXECUTABLE_KEY: &str = "executable";

/// The record's key for its inventory of the runtime's files.
const FILES_KEY: &str = "files";

/// What a runtime's release record says of the release and of each file the runtime holds beside
/// it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BuildInfo {
    /// The release's version, as the record writes it: a semantic version, which may carry build
    /// metadata.
    pub version: String,
    /// The commit the release was built from.
    pub commit: String,
    /// The platform the release was built for.
    pub platform: Platform,
    /// The game's executable: the platform's own.
    pub executable: String,
    /// The SHA-256 digest of every file of the runtime but the record itself, by name.
    pub files: BTreeMap<String, Sha256Digest>,
    /// The keys the record holds beyond its own, kept as they are for a later launcher.
    pub other: Map<String, Value>,
}

impl BuildInfo {
    /// Reads a release record and holds it to the rules a runtime's record keeps, refusing it for
    /// the first it breaks, as the variants of [`PayloadError`] name them. Keys the record does
    /// not know are kept in `other`.
    pub fn parse(bytes: &[u8]) -> Result<BuildInfo, PayloadError> {
        let record = serde_json::from_slice(bytes)
            .map_err(|error| invalid(format_args!("build-info.json is not JSON: {error}")))?;
        let Value::Object(mut record) = record else {
            return Err(invalid("build-info.json is not a JSON object"));
        };
        let mut text = |key: &str| match record.remove(key) {
            Some(Value::String(text)) => Ok(text),
            _ => Err(invalid(format_args!("its {key} is not a string"))),
        };
        let version = text(VERSION_KEY)?;
        let commit = text(COMMIT_KEY)?;
        let platform = text(PLATFORM_KEY)?;
        let executable = text(EXECUTABLE_KEY)?;
        if Version::parse(&version).is_err() {
            return Err(PayloadError::Version {
                text: version.into(),
            });
        }
        let Some(platform) = Platform::parse(&platform) else {
            return Err(PayloadError::Platform {
                text: platform.into(),
            });
        };
        if executable != platform.executable() {
            return Err(PayloadError::Executable {
                name: executable.into(),
            });
        }
        let files = match record.remove(FILES_KEY) {
            Some(Value::Object(files)) if files.len() <= MAX_PAYLOAD_ENTRIES => files,
            _ => {
                return Err(invalid(format_args!(
                    "its file inventory is not an object of at most {MAX_PAYLOAD_ENTRIES} names"
                )));
            }
        };
        check_names(&files, platform)?;
        let files = files
            .into_iter()
            .map(
                |(name, digest)| match digest.as_str().and_then(Sha256Digest::from_hex) {
                    Some(digest) => Ok((name, digest)),
                    None => Err(PayloadError::Digest { name: name.into() }),
                },
            )
            .collect::<Result<_, _>>()?;
        Ok(BuildInfo {
            version,
            commit,
            platform,
            executable,
            files,
            other: record,
        })
    }

    /// The release's version, read: what its order and equality go by.
    ///
    /// A record [`BuildInfo::parse`] reads always has one; this panics only when `version` has
    /// been changed since to a text that is not a semantic version.
    pub fn version(&self) -> Version {
        Version::parse(&self.version).expect("a release record's version is a semantic version")
    }
}

/// Holds the inventory's names to the payload's rules, refusing it for the first one broken: each
/// name keeps to the payload's name rules, shares a name in no ASCII letter case with another or
/// with the record itself, and lies where a runtime of `platform` keeps its files; no name stands
/// above another; and the files every runtime has are there, in their exact case.
fn check_names(files: &Map<String, Value>, platform: Platform) -> Result<(), PayloadError> {
    let mut names = NameSet::default();
    let record = RelPath::parse(BUILD_INFO, &PAYLOAD_NAMES)
        .expect("the record's own name keeps to the payload's name rules");
    names
        .insert(record)
        .expect("a set of no names takes any name");
    for name in files.keys() {
        let name =
            RelPath::parse(name, &PAYLOAD_NAMES).map_err(|error| PayloadError::Name { error })?;
        names
            .insert(name.clone())
            .map_err(|error| PayloadError::Duplicate { name: error.name })?;
        check_root(&name)?;
        check_platform_root(&name, platform)?;
    }
    names
        .conflicts()
        .map_err(|error| PayloadError::Conflict { name: error.name })?;
    let required = [
        VERSION_FILE,
        PORTABLE_FLAG,
        DEFAULT_CONFIG,
        platform.executable(),
    ];
    match required.into_iter().find(|name| !files.contains_key(*name)) {
        Some(name) => Err(PayloadError::Missing { name: name.into() }),
        None => Ok(()),
    }
}

/// The refusal of a record that is not one a runtime could have, for the reason `detail` gives.
fn invalid(detail: impl fmt::Display) -> PayloadError {
    PayloadError::BuildInfo {
        detail: detail.to_string().into(),
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use gdl_files::NameRule;
    use gdl_fixtures::RuntimeFiles;
    use gdl_media::Sha256Digest;
    use gdl_release::{Platform, Version};
    use serde_json::{Map, Value, json};

    use super::BuildInfo;
    use crate::payload::{MAX_PAYLOAD_ENTRIES, PayloadError};

    /// The version the test records carry.
    const VERSION: &str = "1.0.0-beta";

    /// The SHA-256 digest of an empty file, as a record writes it.
    const EMPTY_SHA256: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    /// The record of the installer's own test runtime for `platform`, as a JSON object.
    fn record(platform: Platform) -> Map<String, Value> {
        let runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        runtime
            .build_info()
            .as_object()
            .expect("a release record is an object")
            .clone()
    }

    /// The record's inventory.
    fn files(record: &mut Map<String, Value>) -> &mut Map<String, Value> {
        record["files"]
            .as_object_mut()
            .expect("the record has an inventory")
    }

    /// `record` as release.py writes it.
    fn bytes(record: &Map<String, Value>) -> Vec<u8> {
        let mut bytes = serde_json::to_vec_pretty(record).unwrap();
        bytes.push(b'\n');
        bytes
    }

    /// The standard record of `platform`, changed.
    fn changed(platform: Platform, change: impl FnOnce(&mut Map<String, Value>)) -> Vec<u8> {
        let mut record = record(platform);
        change(&mut record);
        bytes(&record)
    }

    /// The standard Linux record, changed.
    fn linux(change: impl FnOnce(&mut Map<String, Value>)) -> Vec<u8> {
        changed(Platform::LinuxX64, change)
    }

    /// The standard Linux record as text, a part of it replaced.
    fn replaced(from: &str, to: &str) -> Vec<u8> {
        let text = String::from_utf8(bytes(&record(Platform::LinuxX64))).unwrap();
        assert!(text.contains(from), "{from} is in {text}");
        text.replace(from, to).into_bytes()
    }

    /// The rule `bytes` are refused for, or `None` when they are read.
    fn refused_for(bytes: &[u8]) -> Option<&'static str> {
        BuildInfo::parse(bytes).err().map(|error| error.rule())
    }

    #[test]
    fn reads_the_standard_record_of_each_platform() {
        for platform in Platform::ALL {
            let runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
            let info = BuildInfo::parse(&runtime.build_info_bytes()).unwrap();
            assert_eq!(info.version, VERSION);
            assert_eq!(info.commit, "test-commit");
            assert_eq!(info.platform, platform);
            assert_eq!(info.executable, platform.executable());
            let digests: BTreeMap<String, Sha256Digest> = runtime
                .files
                .iter()
                .map(|(name, data)| (name.clone(), Sha256Digest::of(data)))
                .collect();
            assert_eq!(info.files, digests, "{platform:?}");
            assert!(info.other.is_empty(), "{platform:?}");
            assert_eq!(info.version(), Version::parse(VERSION).unwrap());
        }

        // A version is read as written, build metadata and all, which its order leaves out.
        let built = linux(|record| {
            record.insert("version".to_owned(), json!("0.1.0-alpha.1+abc"));
        });
        let info = BuildInfo::parse(&built).unwrap();
        assert_eq!(info.version, "0.1.0-alpha.1+abc");
        assert_eq!(info.version().to_string(), "0.1.0-alpha.1+abc");
        assert_eq!(info.version(), Version::parse("0.1.0-alpha.1").unwrap());

        // Without the line end release.py writes after it, or indentation, it reads the same.
        let record = record(Platform::LinuxX64);
        let compact = serde_json::to_vec(&record).unwrap();
        assert_eq!(
            BuildInfo::parse(&compact).unwrap(),
            BuildInfo::parse(&bytes(&record)).unwrap()
        );
    }

    #[test]
    fn keeps_the_keys_it_does_not_know() {
        let bytes = changed(Platform::WindowsX64, |record| {
            record.insert("future".to_owned(), json!({"x": [1, 2.5, null]}));
            record.insert("notes".to_owned(), json!("kept"));
        });
        let info = BuildInfo::parse(&bytes).unwrap();
        let mut expected = Map::new();
        expected.insert("future".to_owned(), json!({"x": [1, 2.5, null]}));
        expected.insert("notes".to_owned(), json!("kept"));
        assert_eq!(info.other, expected);
        // The record's own fields are read into theirs, and never kept twice.
        assert_eq!(info.commit, "test-commit");
        assert_eq!(info.files.len(), 6);
    }

    #[test]
    fn refuses_each_broken_record_for_its_rule() {
        let digest = || json!(EMPTY_SHA256);
        let config = "data/config.json";
        let mut wrong = Vec::new();
        for (what, bytes, rule) in [
            // Not a JSON object, as serde_json reads JSON: no byte-order mark, no NaN, no more
            // than one value.
            ("not JSON", b"not JSON\n".to_vec(), "buildInfo"),
            (
                "a byte-order mark",
                [&b"\xef\xbb\xbf"[..], &bytes(&record(Platform::LinuxX64))].concat(),
                "buildInfo",
            ),
            (
                "a commit of NaN",
                replaced("\"test-commit\"", "NaN"),
                "buildInfo",
            ),
            (
                "a second value after the record",
                replaced("\n}\n", "\n}\n{}\n"),
                "buildInfo",
            ),
            ("an array", b"[]\n".to_vec(), "buildInfo"),
            ("a string", b"\"version\"\n".to_vec(), "buildInfo"),
            ("an empty file", Vec::new(), "buildInfo"),
            // Fields that are not texts.
            (
                "no version",
                linux(|record| {
                    record.remove("version");
                }),
                "buildInfo",
            ),
            (
                "a commit of 5",
                linux(|record| {
                    record.insert("commit".to_owned(), json!(5));
                }),
                "buildInfo",
            ),
            (
                "a platform of null",
                linux(|record| {
                    record.insert("platform".to_owned(), Value::Null);
                }),
                "buildInfo",
            ),
            (
                "an executable list",
                linux(|record| {
                    record.insert("executable".to_owned(), json!(["gauntlet"]));
                }),
                "buildInfo",
            ),
            // Versions, platforms and executables a release does not have.
            (
                "the version of a tag",
                linux(|record| {
                    record.insert("version".to_owned(), json!("v1.0.0-beta"));
                }),
                "version",
            ),
            (
                "a version of two numbers",
                linux(|record| {
                    record.insert("version".to_owned(), json!("1.0"));
                }),
                "version",
            ),
            (
                "a version with a space before it",
                linux(|record| {
                    record.insert("version".to_owned(), json!(" 1.0.0-beta"));
                }),
                "version",
            ),
            (
                "another-os",
                linux(|record| {
                    record.insert("platform".to_owned(), json!("another-os"));
                }),
                "platform",
            ),
            (
                "a platform in another letter case",
                linux(|record| {
                    record.insert("platform".to_owned(), json!("Linux-x64"));
                }),
                "platform",
            ),
            (
                "an executable named game",
                linux(|record| {
                    record.insert("executable".to_owned(), json!("game"));
                }),
                "executable",
            ),
            (
                "Windows' executable on Linux",
                linux(|record| {
                    record.insert("executable".to_owned(), json!("gauntlet.exe"));
                }),
                "executable",
            ),
            (
                "Linux's executable on Windows",
                changed(Platform::WindowsX64, |record| {
                    record.insert("executable".to_owned(), json!("gauntlet"));
                }),
                "executable",
            ),
            (
                "the executable in another letter case",
                linux(|record| {
                    record.insert("executable".to_owned(), json!("Gauntlet"));
                }),
                "executable",
            ),
            // Inventories that are not one.
            (
                "no inventory",
                linux(|record| {
                    record.remove("files");
                }),
                "buildInfo",
            ),
            (
                "an inventory list",
                linux(|record| {
                    record.insert("files".to_owned(), json!([]));
                }),
                "buildInfo",
            ),
            (
                "an inventory text",
                linux(|record| {
                    record.insert("files".to_owned(), json!("VERSION"));
                }),
                "buildInfo",
            ),
            // Names a runtime cannot have.
            (
                "a name that leaves the runtime",
                linux(|record| {
                    files(record).insert("data/../escaped.json".to_owned(), digest());
                }),
                "name",
            ),
            (
                "a name outside ASCII",
                linux(|record| {
                    files(record).insert("data/t\u{e9}.json".to_owned(), digest());
                }),
                "name",
            ),
            (
                "a name of a device",
                linux(|record| {
                    files(record).insert("shaders/aux.spv".to_owned(), digest());
                }),
                "name",
            ),
            (
                "build-info.json listed",
                linux(|record| {
                    files(record).insert("build-info.json".to_owned(), digest());
                }),
                "duplicate",
            ),
            (
                "build-info.json listed in upper case",
                linux(|record| {
                    files(record).insert("BUILD-INFO.JSON".to_owned(), digest());
                }),
                "duplicate",
            ),
            (
                "data/Config.json beside data/config.json",
                linux(|record| {
                    files(record).insert("data/Config.json".to_owned(), digest());
                }),
                "duplicate",
            ),
            (
                "saves/hero.json",
                linux(|record| {
                    files(record).insert("saves/hero.json".to_owned(), digest());
                }),
                "unknownRoot",
            ),
            (
                "VERSION as a folder",
                linux(|record| {
                    files(record).insert("VERSION/x".to_owned(), digest());
                }),
                "fileRoot",
            ),
            (
                "shaders as a file",
                linux(|record| {
                    files(record).insert("shaders".to_owned(), digest());
                }),
                "folderRoot",
            ),
            (
                "data that is not JSON",
                linux(|record| {
                    files(record).insert("data/texture.ngc".to_owned(), digest());
                }),
                "dataNotJson",
            ),
            (
                "Linux's libraries on Windows",
                changed(Platform::WindowsX64, |record| {
                    files(record).insert("lib/libgcc_s.so.1".to_owned(), digest());
                }),
                "platformRoot",
            ),
            (
                "Windows' libraries on Linux",
                linux(|record| {
                    files(record).insert("SDL3.dll".to_owned(), digest());
                }),
                "platformRoot",
            ),
            (
                "Windows' executable listed on Linux",
                linux(|record| {
                    files(record).insert("gauntlet.exe".to_owned(), digest());
                }),
                "executable",
            ),
            (
                "a name that is the folder above another",
                linux(|record| {
                    files(record).insert("shaders/test.spv/x".to_owned(), digest());
                }),
                "conflict",
            ),
            // The files every runtime has.
            (
                "no VERSION",
                linux(|record| {
                    files(record).remove("VERSION");
                }),
                "incomplete",
            ),
            (
                "no portable.flag",
                linux(|record| {
                    files(record).remove("portable.flag");
                }),
                "incomplete",
            ),
            (
                "no data/config.json",
                linux(|record| {
                    files(record).remove(config);
                }),
                "incomplete",
            ),
            (
                "no executable",
                changed(Platform::WindowsX64, |record| {
                    files(record).remove("gauntlet.exe");
                }),
                "incomplete",
            ),
            (
                "VERSION in another letter case",
                linux(|record| {
                    let digest = files(record).remove("VERSION").unwrap();
                    files(record).insert("version".to_owned(), digest);
                }),
                "unknownRoot",
            ),
            // Digests as hashlib's hexdigest never writes them.
            (
                "a digest in upper case",
                linux(|record| {
                    files(record).insert(config.to_owned(), json!(EMPTY_SHA256.to_uppercase()));
                }),
                "digest",
            ),
            (
                "a digest one digit short",
                linux(|record| {
                    files(record).insert(config.to_owned(), json!(&EMPTY_SHA256[1..]));
                }),
                "digest",
            ),
            (
                "a digest one digit long",
                linux(|record| {
                    files(record).insert(config.to_owned(), json!(format!("{EMPTY_SHA256}0")));
                }),
                "digest",
            ),
            (
                "a digest with a letter past f",
                linux(|record| {
                    let digest = format!("g{}", &EMPTY_SHA256[1..]);
                    files(record).insert(config.to_owned(), json!(digest));
                }),
                "digest",
            ),
            (
                "a digest with a space",
                linux(|record| {
                    let digest = format!(" {}", &EMPTY_SHA256[1..]);
                    files(record).insert(config.to_owned(), json!(digest));
                }),
                "digest",
            ),
            (
                "a digest that is a number",
                linux(|record| {
                    files(record).insert(config.to_owned(), json!(7));
                }),
                "digest",
            ),
            (
                "a digest of null",
                linux(|record| {
                    files(record).insert(config.to_owned(), Value::Null);
                }),
                "digest",
            ),
        ] {
            match refused_for(&bytes) {
                Some(found) if found == rule => {}
                Some(found) => wrong.push(format!("{what}: refused for {found}, not for {rule}")),
                None => wrong.push(format!("{what}: read, not refused for {rule}")),
            }
        }
        assert!(wrong.is_empty(), "{}", wrong.join("\n"));
    }

    #[test]
    fn the_first_rule_broken_is_reported() {
        let digest = || json!(EMPTY_SHA256);
        for (what, bytes, rule) in [
            (
                "a commit of 5 and the version of a tag",
                linux(|record| {
                    record.insert("commit".to_owned(), json!(5));
                    record.insert("version".to_owned(), json!("v1.0.0-beta"));
                }),
                "buildInfo",
            ),
            (
                "the version of a tag and another-os",
                linux(|record| {
                    record.insert("version".to_owned(), json!("v1.0.0-beta"));
                    record.insert("platform".to_owned(), json!("another-os"));
                }),
                "version",
            ),
            (
                "another-os and an executable named game",
                linux(|record| {
                    record.insert("platform".to_owned(), json!("another-os"));
                    record.insert("executable".to_owned(), json!("game"));
                }),
                "platform",
            ),
            (
                "an executable named game and an inventory list",
                linux(|record| {
                    record.insert("executable".to_owned(), json!("game"));
                    record.insert("files".to_owned(), json!([]));
                }),
                "executable",
            ),
            (
                "a name of a device and no VERSION",
                linux(|record| {
                    files(record).insert("shaders/con.spv".to_owned(), digest());
                    files(record).remove("VERSION");
                }),
                "name",
            ),
            (
                "a name of a device and a digest in upper case",
                linux(|record| {
                    files(record).insert("shaders/con.spv".to_owned(), digest());
                    let upper = EMPTY_SHA256.to_uppercase();
                    files(record).insert("portable.flag".to_owned(), json!(upper));
                }),
                "name",
            ),
            (
                "a name above another and no VERSION",
                linux(|record| {
                    files(record).insert("shaders/test.spv/x".to_owned(), digest());
                    files(record).remove("VERSION");
                }),
                "conflict",
            ),
            (
                "no VERSION and a digest in upper case",
                linux(|record| {
                    files(record).remove("VERSION");
                    let upper = EMPTY_SHA256.to_uppercase();
                    files(record).insert("portable.flag".to_owned(), json!(upper));
                }),
                "incomplete",
            ),
        ] {
            match BuildInfo::parse(&bytes) {
                Err(error) => assert_eq!(error.rule(), rule, "{what}: {error}"),
                Ok(_) => panic!("{what}: read"),
            }
        }
    }

    #[test]
    fn the_inventory_bound_is_inclusive() {
        // The release scripts' bound, the same as a runtime's entries.
        assert_eq!(MAX_PAYLOAD_ENTRIES, 20_000);
        let mut record = record(Platform::LinuxX64);
        let more = MAX_PAYLOAD_ENTRIES - files(&mut record).len();
        for index in 0..more {
            files(&mut record).insert(format!("shaders/{index:05}.spv"), json!(EMPTY_SHA256));
        }
        let info = BuildInfo::parse(&bytes(&record)).unwrap();
        assert_eq!(info.files.len(), MAX_PAYLOAD_ENTRIES);

        files(&mut record).insert("shaders/one-more.spv".to_owned(), json!(EMPTY_SHA256));
        match BuildInfo::parse(&bytes(&record)) {
            Err(error @ PayloadError::BuildInfo { .. }) => assert_eq!(
                error.to_string(),
                "Invalid release metadata: its file inventory is not an object of at most 20000 \
                 names"
            ),
            other => panic!("{:?}", other.map(|info| info.files.len())),
        }
    }

    #[test]
    fn refusals_name_what_breaks_the_rule() {
        let refused = |bytes: &[u8]| BuildInfo::parse(bytes).unwrap_err();
        let error = refused(&linux(|record| {
            record.insert("commit".to_owned(), json!(5));
        }));
        assert_eq!(
            error.to_string(),
            "Invalid release metadata: its commit is not a string"
        );
        let error = refused(b"[]");
        assert_eq!(
            error.to_string(),
            "Invalid release metadata: build-info.json is not a JSON object"
        );
        let error = refused(b"not JSON");
        assert!(
            error
                .to_string()
                .starts_with("Invalid release metadata: build-info.json is not JSON: "),
            "{error}"
        );
        match refused(&linux(|record| {
            record.insert("version".to_owned(), json!("v1.0.0-beta"));
        })) {
            PayloadError::Version { text } => assert_eq!(&*text, "v1.0.0-beta"),
            other => panic!("{other}"),
        }
        match refused(&linux(|record| {
            record.insert("platform".to_owned(), json!("another-os"));
        })) {
            PayloadError::Platform { text } => assert_eq!(&*text, "another-os"),
            other => panic!("{other}"),
        }
        match refused(&linux(|record| {
            record.insert("executable".to_owned(), json!("game"));
        })) {
            PayloadError::Executable { name } => assert_eq!(&*name, "game"),
            other => panic!("{other}"),
        }
        match refused(&linux(|record| {
            files(record).insert("data/../escaped.json".to_owned(), json!(EMPTY_SHA256));
        })) {
            PayloadError::Name { error } => {
                assert_eq!(
                    (&*error.name, error.rule),
                    ("data/../escaped.json", NameRule::Dot)
                );
            }
            other => panic!("{other}"),
        }
        match refused(&linux(|record| {
            files(record).insert("BUILD-INFO.JSON".to_owned(), json!(EMPTY_SHA256));
        })) {
            PayloadError::Duplicate { name } => assert_eq!(&*name, "BUILD-INFO.JSON"),
            other => panic!("{other}"),
        }
        match refused(&linux(|record| {
            files(record).insert("shaders/test.spv/x".to_owned(), json!(EMPTY_SHA256));
        })) {
            PayloadError::Conflict { name } => assert_eq!(&*name, "shaders/test.spv"),
            other => panic!("{other}"),
        }
        match refused(&linux(|record| {
            files(record).remove("portable.flag");
        })) {
            PayloadError::Missing { name } => assert_eq!(&*name, "portable.flag"),
            other => panic!("{other}"),
        }
        match refused(&linux(|record| {
            files(record).insert("data/config.json".to_owned(), json!(7));
        })) {
            PayloadError::Digest { name } => assert_eq!(&*name, "data/config.json"),
            other => panic!("{other}"),
        }
    }
}
