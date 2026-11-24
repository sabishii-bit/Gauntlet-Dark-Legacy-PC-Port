//! The game runtimes the launcher installs, as zips laid out the way scripts/release/release.py
//! lays them out: the runtime's files deflated in name order, then `build-info.json`, the record
//! of the release that lists each file by its SHA-256 digest.

use serde_json::{Map, Value, json};
use sha2::{Digest, Sha256};

use crate::zip::{RawEntry, RawZip};

/// The release record's name, at the top of the runtime.
pub(crate) const BUILD_INFO: &str = "build-info.json";

/// The platform whose executable, and whose installations' launcher copy, take Windows' suffix;
/// every other platform's go without.
pub(crate) const WINDOWS_PLATFORM: &str = "windows-x64";

/// The game's executable on Windows.
const WINDOWS_EXECUTABLE: &str = "gauntlet.exe";

/// The game's executable on every other platform.
const EXECUTABLE: &str = "gauntlet";

/// The commit a standard runtime says it was built from.
const STANDARD_COMMIT: &str = "test-commit";

/// The commit the runtime [`RuntimeFiles::next`] gives says it was built from.
const NEXT_COMMIT: &str = "next-commit";

/// The commit the runtime [`RuntimeFiles::newer`] gives says it was built from.
const NEWER_COMMIT: &str = "new-commit";

/// The file at a runtime's top that names its version, followed by a line end.
const VERSION_FILE: &str = "VERSION";

/// The game's English text table, which [`RuntimeFiles::next`] changes.
const TEXT_FILE: &str = "data/text/en.json";

/// The shader a standard runtime holds, which the runtimes after it no longer ship.
const OLD_SHADER: &str = "shaders/test.spv";

/// The shader [`RuntimeFiles::next`] adds.
const NEXT_SHADER: &str = "shaders/next.spv";

/// What [`RuntimeFiles::next`]'s shader holds.
const NEXT_SHADER_BYTES: &[u8] = b"next-shader";

/// The shader [`RuntimeFiles::newer`] adds.
const NEWER_SHADER: &str = "shaders/new.spv";

/// What [`RuntimeFiles::newer`]'s shader holds.
const NEWER_SHADER_BYTES: &[u8] = b"new-shader";

/// What every file but `VERSION` and the new shader holds in [`RuntimeFiles::newer`]'s runtime.
const NEWER_BYTES: &[u8] = b"new-runtime";

/// The digits a SHA-256 digest is written in, in lower case, as Python's `hexdigest` writes it.
const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";

/// A runtime's files and what its `build-info.json` says of them. Every field may be changed to
/// make the runtime a test needs, a damaged one included.
#[derive(Clone, Debug)]
pub struct RuntimeFiles {
    /// Each file's name in the runtime and its contents, in any order.
    pub files: Vec<(String, Vec<u8>)>,
    /// The release's version, as `build-info.json` records it.
    pub version: String,
    /// The commit the release was built from.
    pub commit: String,
    /// The platform the release was built for, such as `linux-x64`.
    pub platform: String,
    /// The name of the game's executable.
    pub executable: String,
    /// Keys `build-info.json` holds after the record's own, which take the place of any of the
    /// same name, so that a field can be given a value of another type.
    pub extra: Map<String, Value>,
}

impl RuntimeFiles {
    /// The runtime of the installer's own tests, for `platform` and `version`: the platform's
    /// executable, which is no real program, `VERSION`, `portable.flag`, `data/config.json`,
    /// `data/text/en.json` and one shader, built from `test-commit`.
    pub fn standard(platform: &str, version: &str) -> Self {
        let executable = if platform == WINDOWS_PLATFORM {
            WINDOWS_EXECUTABLE
        } else {
            EXECUTABLE
        };
        let files = [
            (executable, b"not a real executable".to_vec()),
            (VERSION_FILE, version_file(version)),
            ("portable.flag", Vec::new()),
            ("data/config.json", b"{}".to_vec()),
            ("data/text/en.json", b"{}".to_vec()),
            ("shaders/test.spv", b"shader".to_vec()),
        ];
        RuntimeFiles {
            files: files
                .into_iter()
                .map(|(name, data)| (name.to_owned(), data))
                .collect(),
            version: version.to_owned(),
            commit: STANDARD_COMMIT.to_owned(),
            platform: platform.to_owned(),
            executable: executable.to_owned(),
            extra: Map::new(),
        }
    }

    /// The runtime after `self`, of `version`, as a release changes one: `VERSION` naming
    /// `version`, `data/text/en.json` naming it too, `shaders/test.spv` gone and
    /// `shaders/next.spv` new, every other file as it was; built from `next-commit` for the same
    /// platform and executable, its record holding the record's own keys alone, as release.py
    /// writes one. After a standard runtime, the executable, `portable.flag` and
    /// `data/config.json` are what stays the same.
    pub fn next(&self, version: &str) -> RuntimeFiles {
        let text = json!({ "version": version }).to_string().into_bytes();
        let files = self
            .files
            .iter()
            .filter(|(name, _)| name != OLD_SHADER && name != NEXT_SHADER)
            .map(|(name, data)| {
                let data = match name.as_str() {
                    VERSION_FILE => version_file(version),
                    TEXT_FILE => text.clone(),
                    _ => data.clone(),
                };
                (name.clone(), data)
            })
            .chain([(NEXT_SHADER.to_owned(), NEXT_SHADER_BYTES.to_vec())])
            .collect();
        self.followed_by(version, NEXT_COMMIT, files)
    }

    /// The runtime the alpha installer's update tests install over `self`, of `version`: every
    /// file of `self` holding `new-runtime` but `VERSION`, which names `version`, with
    /// `shaders/test.spv` gone and `shaders/new.spv` new, holding `new-shader`; built from
    /// `new-commit` for the same platform and executable, its record holding the record's own keys
    /// alone, as release.py writes one.
    pub fn newer(&self, version: &str) -> RuntimeFiles {
        let files = self
            .files
            .iter()
            .filter(|(name, _)| name != OLD_SHADER && name != NEWER_SHADER)
            .map(|(name, _)| {
                let data = if name == VERSION_FILE {
                    version_file(version)
                } else {
                    NEWER_BYTES.to_vec()
                };
                (name.clone(), data)
            })
            .chain([(NEWER_SHADER.to_owned(), NEWER_SHADER_BYTES.to_vec())])
            .collect();
        self.followed_by(version, NEWER_COMMIT, files)
    }

    /// A release after `self` of `version`, built from `commit` for `self`'s platform and
    /// executable, holding `files`.
    fn followed_by(&self, version: &str, commit: &str, files: Vec<(String, Vec<u8>)>) -> Self {
        RuntimeFiles {
            files,
            version: version.to_owned(),
            commit: commit.to_owned(),
            platform: self.platform.clone(),
            executable: self.executable.clone(),
            extra: Map::new(),
        }
    }

    /// What `build-info.json` holds: the version, commit, platform and executable, the files'
    /// SHA-256 digests by name, then [`extra`](Self::extra).
    pub fn build_info(&self) -> Value {
        let files = self
            .files
            .iter()
            .map(|(name, data)| (name.clone(), Value::String(sha256_hex(data))))
            .collect();
        let mut info = Map::new();
        info.insert("version".to_owned(), Value::String(self.version.clone()));
        info.insert("commit".to_owned(), Value::String(self.commit.clone()));
        info.insert("platform".to_owned(), Value::String(self.platform.clone()));
        info.insert(
            "executable".to_owned(),
            Value::String(self.executable.clone()),
        );
        info.insert("files".to_owned(), Value::Object(files));
        info.extend(self.extra.clone());
        Value::Object(info)
    }

    /// `build-info.json` as release.py writes it: JSON indented by two spaces, then a line end.
    pub fn build_info_bytes(&self) -> Vec<u8> {
        pretty_json(&self.build_info())
    }

    /// The runtime's archive, field by field: each file deflated, in byte order of the names, then
    /// `build-info.json`, deflated too.
    pub fn raw(&self) -> RawZip {
        self.archive(None)
    }

    /// The runtime's archive, as the bytes of a zip file.
    pub fn zip(&self) -> Vec<u8> {
        self.raw().to_bytes()
    }

    /// Makes `program`, a real program of some megabytes, the game's executable, and gives the
    /// runtime's archive as [`zip`](Self::zip) lays it out, but with the executable held as it is
    /// rather than deflated, so that the archive builds fast; `build-info.json` names the
    /// program's bytes.
    pub fn zip_with_program(&mut self, program: &[u8]) -> Vec<u8> {
        let executable = self.executable.clone();
        let shipped = &mut self
            .files
            .iter_mut()
            .find(|(name, _)| *name == executable)
            .expect("the runtime holds its executable")
            .1;
        shipped.clear();
        shipped.extend_from_slice(program);
        self.archive(Some(&executable)).to_bytes()
    }

    /// The runtime's archive as [`raw`](Self::raw) lays it out, with the file named `stored`, when
    /// one is, held as it is rather than deflated.
    fn archive(&self, stored: Option<&str>) -> RawZip {
        let mut files: Vec<_> = self.files.iter().collect();
        files.sort_by(|(one, _), (other, _)| one.cmp(other));
        let mut entries: Vec<RawEntry> = files
            .into_iter()
            .map(|(name, data)| {
                if stored == Some(name.as_str()) {
                    RawEntry::stored(name, data)
                } else {
                    RawEntry::deflated(name, data)
                }
            })
            .collect();
        entries.push(RawEntry::deflated(BUILD_INFO, &self.build_info_bytes()));
        RawZip {
            entries,
            ..RawZip::default()
        }
    }
}

/// What a runtime's `VERSION` holds for `version`: the version, then a line end.
fn version_file(version: &str) -> Vec<u8> {
    format!("{version}\n").into_bytes()
}

/// `value` as the release scripts and the launcher write JSON files: indented by two spaces, with
/// LF line ends and a line end after it.
pub fn pretty_json(value: &Value) -> Vec<u8> {
    let mut bytes = serde_json::to_vec_pretty(value).expect("a JSON value always has a JSON form");
    bytes.push(b'\n');
    bytes
}

/// The SHA-256 digest of `data`, in lower-case hexadecimal.
pub fn sha256_hex(data: &[u8]) -> String {
    hex(&Sha256::digest(data))
}

/// `bytes` in lower-case hexadecimal, two digits a byte, as release records and section tables
/// write digests.
pub(crate) fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(2 * bytes.len());
    for &byte in bytes {
        text.push(char::from(HEX_DIGITS[usize::from(byte >> 4)]));
        text.push(char::from(HEX_DIGITS[usize::from(byte & 0x0F)]));
    }
    text
}

/// The bytes a shared vector's hexadecimal text spells, two digits a byte, for the tests that
/// hold the launcher's builders and readers to the vectors. Panics when `text` is no such text.
pub fn unhex(text: &Value) -> Vec<u8> {
    let text = text.as_str().expect("hexadecimal text");
    assert_eq!(text.len() % 2, 0, "{text}");
    (0..text.len())
        .step_by(2)
        .map(|at| u8::from_str_radix(&text[at..at + 2], 16).expect("hexadecimal digits"))
        .collect()
}

#[cfg(test)]
mod tests {
    use std::collections::{BTreeMap, BTreeSet};
    use std::io::{Cursor, Read};

    use ::zip::{CompressionMethod, ZipArchive};
    use flate2::read::DeflateDecoder;
    use serde_json::{Value, json};

    use super::{RuntimeFiles, hex, sha256_hex};
    use crate::zip::crc32;

    // The SHA-256 digests of the standard runtime's contents, as Python's hashlib gives them.
    const EXECUTABLE_SHA256: &str =
        "16aefdb711b87537abb9808387cfed74a92a18e0eb8ccc12158714b5019f0232";
    const VERSION_SHA256: &str = "74c371dc05553824203457c4dd54454c685d3ba0730fa540f66ee5b05023daba";
    const EMPTY_SHA256: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const BRACES_SHA256: &str = "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";
    const SHADER_SHA256: &str = "e137e75a5e0e7a623ca39de480667a55b43e0eedec58767634ebaef07c33383a";

    // The digests of what the runtimes after it change or add, as hashlib gives them too: `VERSION`
    // naming 1.0.0-beta.1, the next runtime's text table and shader, and the newer runtime's files
    // and shader.
    const NEXT_VERSION_SHA256: &str =
        "5703a2f830f2a6e2ce9be19e37c5309e39b6566d071f641cf11c5dc2c1c6331d";
    const NEXT_TEXT_SHA256: &str =
        "dbdc022278a4f9283aa4fb24a33ed531f8e37fff661747b36d144e7dab35ee36";
    const NEXT_SHADER_SHA256: &str =
        "f474a5d956baa0e921e3b59359f90e1411ada9beaf1c143e130efada52b0e761";
    const NEW_RUNTIME_SHA256: &str =
        "3aefc37377c5a7743ed0c1fa1d6fe9bb511fb1732466091be46d64ef17193787";
    const NEW_SHADER_SHA256: &str =
        "617d64c50324e303d00208a9d2f129990cfd9f17b1f5d7b93027e77c5a5ef70b";

    /// The platforms a release is built for, and each one's executable.
    const PLATFORMS: [(&str, &str); 2] =
        [("windows-x64", "gauntlet.exe"), ("linux-x64", "gauntlet")];

    /// The names an update from one runtime to another moves, told by the digests their records
    /// list, `build-info.json` aside.
    #[derive(Debug, Default, PartialEq, Eq)]
    struct Moved {
        replaced: BTreeSet<String>,
        added: BTreeSet<String>,
        removed: BTreeSet<String>,
        unchanged: BTreeSet<String>,
    }

    fn moved(before: &RuntimeFiles, after: &RuntimeFiles) -> Moved {
        let listed = |runtime: &RuntimeFiles| -> BTreeMap<String, Value> {
            let info = runtime.build_info();
            info["files"]
                .as_object()
                .unwrap()
                .clone()
                .into_iter()
                .collect()
        };
        let (old, new) = (listed(before), listed(after));
        let mut changes = Moved::default();
        for (name, digest) in &new {
            let kind = match old.get(name) {
                None => &mut changes.added,
                Some(old) if old == digest => &mut changes.unchanged,
                Some(_) => &mut changes.replaced,
            };
            kind.insert(name.clone());
        }
        changes.removed = old
            .into_keys()
            .filter(|name| !new.contains_key(name))
            .collect();
        changes
    }

    fn names(names: &[&str]) -> BTreeSet<String> {
        names.iter().map(|name| (*name).to_owned()).collect()
    }

    fn contents(runtime: &RuntimeFiles) -> BTreeMap<&str, &[u8]> {
        runtime
            .files
            .iter()
            .map(|(name, data)| (name.as_str(), data.as_slice()))
            .collect()
    }

    #[test]
    fn the_standard_runtime_is_the_installer_tests_own() {
        for (platform, executable) in PLATFORMS {
            let runtime = RuntimeFiles::standard(platform, "1.0.0-beta");
            assert_eq!(runtime.version, "1.0.0-beta");
            assert_eq!(runtime.commit, "test-commit");
            assert_eq!(runtime.platform, platform);
            assert_eq!(runtime.executable, executable);
            assert!(runtime.extra.is_empty());
            let files: BTreeMap<&str, &[u8]> = runtime
                .files
                .iter()
                .map(|(name, data)| (name.as_str(), data.as_slice()))
                .collect();
            let expected: BTreeMap<&str, &[u8]> = [
                (executable, &b"not a real executable"[..]),
                ("VERSION", &b"1.0.0-beta\n"[..]),
                ("portable.flag", &b""[..]),
                ("data/config.json", &b"{}"[..]),
                ("data/text/en.json", &b"{}"[..]),
                ("shaders/test.spv", &b"shader"[..]),
            ]
            .into_iter()
            .collect();
            assert_eq!(files, expected, "{platform}");
            assert_eq!(runtime.files.len(), expected.len(), "{platform}");
        }
        // A platform no release is built for keeps the executable's plain name.
        let other = RuntimeFiles::standard("another-os", "1.0.0-beta");
        assert_eq!(other.platform, "another-os");
        assert_eq!(other.executable, "gauntlet");
        assert!(other.files.iter().any(|(name, _)| name == "gauntlet"));
    }

    #[test]
    fn the_next_runtime_changes_two_files_and_swaps_a_shader() {
        for (platform, executable) in PLATFORMS {
            let standard = RuntimeFiles::standard(platform, "1.0.0-beta");
            let next = standard.next("1.0.0-beta.1");
            assert_eq!(next.version, "1.0.0-beta.1");
            assert_eq!(next.commit, "next-commit");
            assert_eq!(next.platform, platform);
            assert_eq!(next.executable, executable);
            assert!(next.extra.is_empty());
            let expected: BTreeMap<&str, &[u8]> = [
                (executable, &b"not a real executable"[..]),
                ("VERSION", &b"1.0.0-beta.1\n"[..]),
                ("portable.flag", &b""[..]),
                ("data/config.json", &b"{}"[..]),
                ("data/text/en.json", &br#"{"version":"1.0.0-beta.1"}"#[..]),
                ("shaders/next.spv", &b"next-shader"[..]),
            ]
            .into_iter()
            .collect();
            assert_eq!(contents(&next), expected, "{platform}");
            assert_eq!(next.files.len(), expected.len(), "{platform}");

            // An update to it replaces two files, adds one shader and removes the other.
            assert_eq!(
                moved(&standard, &next),
                Moved {
                    replaced: names(&["VERSION", "data/text/en.json"]),
                    added: names(&["shaders/next.spv"]),
                    removed: names(&["shaders/test.spv"]),
                    unchanged: names(&[executable, "portable.flag", "data/config.json"]),
                },
                "{platform}"
            );
            // Its record lists the new files by the digests Python's hashlib gives them.
            let info = next.build_info();
            assert_eq!(info["files"]["VERSION"], NEXT_VERSION_SHA256);
            assert_eq!(info["files"]["data/text/en.json"], NEXT_TEXT_SHA256);
            assert_eq!(info["files"]["shaders/next.spv"], NEXT_SHADER_SHA256);
            assert_eq!(info["files"][executable], EXECUTABLE_SHA256);
            assert_eq!(info["commit"], "next-commit");
            // The runtime it follows is as it was.
            assert_eq!(
                contents(&standard),
                contents(&RuntimeFiles::standard(platform, "1.0.0-beta"))
            );

            // Followed again, it names each file once and changes the version's two files alone.
            let again = next.next("1.0.0-beta.2");
            assert_eq!(again.files.len(), expected.len(), "{platform}");
            assert_eq!(
                moved(&next, &again),
                Moved {
                    replaced: names(&["VERSION", "data/text/en.json"]),
                    unchanged: names(&[
                        executable,
                        "portable.flag",
                        "data/config.json",
                        "shaders/next.spv",
                    ]),
                    ..Moved::default()
                },
                "{platform}"
            );
        }

        // Its record holds the release's own keys alone, whatever the runtime before it held.
        let mut changed = RuntimeFiles::standard("linux-x64", "1.0.0-beta");
        changed.extra.insert("commit".to_owned(), json!(5));
        let next = changed.next("1.0.0-beta.1");
        assert!(next.extra.is_empty());
        assert_eq!(next.build_info()["commit"], "next-commit");
    }

    /// The newer runtime is the one the alpha installer's update tests install over theirs.
    #[test]
    fn the_newer_runtime_is_the_updater_tests_own() {
        for (platform, executable) in PLATFORMS {
            let standard = RuntimeFiles::standard(platform, "1.0.0-beta");
            let newer = standard.newer("1.0.0-beta.1");
            assert_eq!(newer.version, "1.0.0-beta.1");
            assert_eq!(newer.commit, "new-commit");
            assert_eq!(newer.platform, platform);
            assert_eq!(newer.executable, executable);
            assert!(newer.extra.is_empty());
            let expected: BTreeMap<&str, &[u8]> = [
                (executable, &b"new-runtime"[..]),
                ("VERSION", &b"1.0.0-beta.1\n"[..]),
                ("portable.flag", &b"new-runtime"[..]),
                ("data/config.json", &b"new-runtime"[..]),
                ("data/text/en.json", &b"new-runtime"[..]),
                ("shaders/new.spv", &b"new-shader"[..]),
            ]
            .into_iter()
            .collect();
            assert_eq!(contents(&newer), expected, "{platform}");
            assert_eq!(newer.files.len(), expected.len(), "{platform}");
            assert_eq!(
                newer.build_info()["files"],
                json!({
                    executable: NEW_RUNTIME_SHA256,
                    "VERSION": NEXT_VERSION_SHA256,
                    "portable.flag": NEW_RUNTIME_SHA256,
                    "data/config.json": NEW_RUNTIME_SHA256,
                    "data/text/en.json": NEW_RUNTIME_SHA256,
                    "shaders/new.spv": NEW_SHADER_SHA256,
                }),
                "{platform}"
            );

            // An update to it replaces every file, adds one shader and removes the other, so with
            // `build-info.json` it backs up seven names and puts seven in place.
            let changes = moved(&standard, &newer);
            assert_eq!(
                changes,
                Moved {
                    replaced: names(&[
                        executable,
                        "VERSION",
                        "portable.flag",
                        "data/config.json",
                        "data/text/en.json",
                    ]),
                    added: names(&["shaders/new.spv"]),
                    removed: names(&["shaders/test.spv"]),
                    unchanged: BTreeSet::new(),
                },
                "{platform}"
            );
            let backup = changes.replaced.len() + changes.removed.len() + 1;
            let publish = changes.replaced.len() + changes.added.len() + 1;
            assert_eq!((backup, publish), (7, 7), "{platform}");

            // Taken further, it names each file once, the new shader as it was.
            let again = newer.newer("1.0.0-beta.2");
            assert_eq!(contents(&again).len(), expected.len(), "{platform}");
            assert_eq!(again.files.len(), expected.len(), "{platform}");
            assert_eq!(contents(&again)["shaders/new.spv"], b"new-shader");
            assert_eq!(contents(&again)["VERSION"], b"1.0.0-beta.2\n");
        }
    }

    #[test]
    fn build_info_lists_each_file_by_its_digest() {
        assert_eq!(hex(b"\x01\x23\x45\x67\x89\xab\xcd\xef"), "0123456789abcdef");
        assert_eq!(sha256_hex(b""), EMPTY_SHA256);
        let mut runtime = RuntimeFiles::standard("windows-x64", "1.0.0-beta");
        assert_eq!(
            runtime.build_info(),
            json!({
                "version": "1.0.0-beta",
                "commit": "test-commit",
                "platform": "windows-x64",
                "executable": "gauntlet.exe",
                "files": {
                    "gauntlet.exe": EXECUTABLE_SHA256,
                    "VERSION": VERSION_SHA256,
                    "portable.flag": EMPTY_SHA256,
                    "data/config.json": BRACES_SHA256,
                    "data/text/en.json": BRACES_SHA256,
                    "shaders/test.spv": SHADER_SHA256,
                },
            })
        );

        // The fields follow the runtime as it is changed.
        runtime.version = "0.1.0-alpha.1+abc".to_owned();
        runtime.platform = "another-os".to_owned();
        runtime.files.retain(|(name, _)| name != "VERSION");
        runtime
            .files
            .push(("saves/hero.json".to_owned(), b"{}".to_vec()));
        let info = runtime.build_info();
        assert_eq!(info["version"], "0.1.0-alpha.1+abc");
        assert_eq!(info["platform"], "another-os");
        let files = info["files"].as_object().unwrap();
        assert_eq!(files.len(), 6);
        assert!(!files.contains_key("VERSION"));
        assert_eq!(files["saves/hero.json"], BRACES_SHA256);

        // Extra keys are kept, and take the place of the record's own.
        runtime.extra.insert("future".to_owned(), json!({"x": 1}));
        runtime.extra.insert("commit".to_owned(), json!(5));
        runtime.extra.insert("files".to_owned(), json!([]));
        let info = runtime.build_info();
        let keys: Vec<&str> = info
            .as_object()
            .unwrap()
            .keys()
            .map(String::as_str)
            .collect();
        assert_eq!(
            keys.len(),
            6,
            "the five fields and one new key, not {keys:?}"
        );
        assert_eq!(info["future"], json!({"x": 1}));
        assert_eq!(info["commit"], json!(5));
        assert_eq!(info["files"], json!([]));
        assert_eq!(info["executable"], "gauntlet.exe");
    }

    #[test]
    fn build_info_is_written_indented_with_a_line_end() {
        for (platform, _) in PLATFORMS {
            let runtime = RuntimeFiles::standard(platform, "1.0.0-beta");
            let bytes = runtime.build_info_bytes();
            let text = std::str::from_utf8(&bytes).unwrap();
            assert!(text.starts_with("{\n  \""), "{text}");
            assert!(text.ends_with("\n}\n"), "{text}");
            assert!(!text.contains('\r'), "{text}");
            assert_eq!(
                serde_json::from_slice::<Value>(&bytes).unwrap(),
                runtime.build_info()
            );
            // Each field on a line of its own two spaces in, and each file four.
            let indented = |depth: usize| {
                text.lines()
                    .filter(|line| {
                        line.strip_prefix(&" ".repeat(depth))
                            .is_some_and(|rest| rest.starts_with('"'))
                    })
                    .count()
            };
            assert_eq!(indented(2), 5, "{text}");
            assert_eq!(indented(4), 6, "{text}");
            assert_eq!(text.lines().count(), 5 + 6 + 3, "{text}");
        }
    }

    #[test]
    fn runtime_archives_hold_the_files_in_name_order_then_build_info() {
        let mut runtime = RuntimeFiles::standard("windows-x64", "1.0.0-beta");
        runtime
            .files
            .push(("lib/a.so".to_owned(), b"library".to_vec()));
        let raw = runtime.raw();
        let names: Vec<&str> = raw
            .entries
            .iter()
            .map(|entry| std::str::from_utf8(&entry.name).unwrap())
            .collect();
        assert_eq!(
            names,
            [
                "VERSION",
                "data/config.json",
                "data/text/en.json",
                "gauntlet.exe",
                "lib/a.so",
                "portable.flag",
                "shaders/test.spv",
                "build-info.json",
            ]
        );
        let contents: BTreeMap<&str, &[u8]> = runtime
            .files
            .iter()
            .map(|(name, data)| (name.as_str(), data.as_slice()))
            .collect();
        let build_info = runtime.build_info_bytes();
        for entry in &raw.entries {
            let name = std::str::from_utf8(&entry.name).unwrap();
            let data = contents.get(name).copied().unwrap_or(&build_info);
            assert_eq!(entry.method, 8, "{name}");
            assert_eq!(entry.declared_size, Some(data.len() as u32), "{name}");
            assert_eq!(entry.crc, Some(crc32(data)), "{name}");
            let mut inflated = Vec::new();
            DeflateDecoder::new(&entry.data[..])
                .read_to_end(&mut inflated)
                .unwrap();
            assert_eq!(inflated, data, "{name}");
            assert_eq!(entry.local_name, None, "{name}");
            assert_eq!(entry.flags, 0, "{name}");
            assert!(entry.extra.is_empty() && entry.comment.is_empty(), "{name}");
        }
        assert!(raw.prefix.is_empty() && raw.comment.is_empty());
        assert_eq!(raw.eocd_count, None);
        assert!(!raw.zip64_locator);
        assert_eq!(runtime.zip(), raw.to_bytes());
    }

    #[test]
    fn a_runtime_archive_reads_back_with_zip() {
        for (platform, executable) in PLATFORMS {
            let runtime = RuntimeFiles::standard(platform, "1.0.0-beta");
            let mut archive = ZipArchive::new(Cursor::new(runtime.zip())).unwrap();
            archive.validate_header_consistency().unwrap();
            let names: Vec<String> = archive
                .file_names()
                .map(|name| name.unwrap().into_owned())
                .collect();
            let mut expected: Vec<String> =
                runtime.files.iter().map(|(name, _)| name.clone()).collect();
            expected.sort();
            expected.push("build-info.json".to_owned());
            assert_eq!(names, expected, "{platform}");

            let mut read = BTreeMap::new();
            for index in 0..archive.len() {
                let mut file = archive.by_index(index).unwrap();
                let mut data = Vec::new();
                file.read_to_end(&mut data).unwrap();
                read.insert(file.name().unwrap().into_owned(), data);
            }
            let info: Value = serde_json::from_slice(&read["build-info.json"]).unwrap();
            assert_eq!(info, runtime.build_info(), "{platform}");
            assert_eq!(info["executable"], executable, "{platform}");
            let files = info["files"].as_object().unwrap();
            assert_eq!(files.len(), read.len() - 1, "{platform}");
            for (name, digest) in files {
                assert_eq!(*digest, sha256_hex(&read[name]), "{platform} {name}");
            }
        }
    }

    #[test]
    fn a_runtime_carrying_a_program_holds_it_as_it_is() {
        let program = b"a real program's bytes ".repeat(1024);
        for (platform, executable) in PLATFORMS {
            let mut runtime = RuntimeFiles::standard(platform, "1.0.0-beta");
            let laid_out: Vec<String> = runtime
                .raw()
                .entries
                .iter()
                .map(|entry| String::from_utf8(entry.name.clone()).unwrap())
                .collect();
            let zip = runtime.zip_with_program(&program);

            // The runtime's executable is the program now, and its record names the program.
            let (_, shipped) = runtime
                .files
                .iter()
                .find(|(name, _)| name == executable)
                .unwrap();
            assert!(*shipped == program, "{platform}");
            assert_eq!(
                runtime.build_info()["files"][executable],
                sha256_hex(&program),
                "{platform}"
            );

            // The archive lays its files out as every runtime's, the record last; the executable
            // is held as it is, and the rest deflated.
            let mut archive = ZipArchive::new(Cursor::new(zip)).unwrap();
            archive.validate_header_consistency().unwrap();
            let names: Vec<String> = archive
                .file_names()
                .map(|name| name.unwrap().into_owned())
                .collect();
            assert_eq!(names, laid_out, "{platform}");
            let contents: BTreeMap<&str, &[u8]> = runtime
                .files
                .iter()
                .map(|(name, data)| (name.as_str(), data.as_slice()))
                .collect();
            let build_info = runtime.build_info_bytes();
            for index in 0..archive.len() {
                let mut file = archive.by_index(index).unwrap();
                let name = file.name().unwrap().into_owned();
                let method = if name == executable {
                    CompressionMethod::Stored
                } else {
                    CompressionMethod::Deflated
                };
                assert_eq!(file.compression(), method, "{platform} {name}");
                let mut data = Vec::new();
                file.read_to_end(&mut data).unwrap();
                let held = contents.get(name.as_str()).copied().unwrap_or(&build_info);
                assert!(data == held, "{platform} {name}");
            }
        }
    }
}
