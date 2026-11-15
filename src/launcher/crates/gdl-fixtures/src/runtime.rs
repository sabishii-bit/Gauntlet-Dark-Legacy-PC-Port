//! The game runtimes the launcher installs, as zips laid out the way scripts/release/release.py
//! lays them out: the runtime's files deflated in name order, then `build-info.json`, the record
//! of the release that lists each file by its SHA-256 digest.

use serde_json::{Map, Value};
use sha2::{Digest, Sha256};

use crate::zip::{RawEntry, RawZip};

/// The release record's name, at the top of the runtime.
const BUILD_INFO: &str = "build-info.json";

/// The platform whose executable takes Windows' suffix; every other platform's goes without.
const WINDOWS_PLATFORM: &str = "windows-x64";

/// The game's executable on Windows.
const WINDOWS_EXECUTABLE: &str = "gauntlet.exe";

/// The game's executable on every other platform.
const EXECUTABLE: &str = "gauntlet";

/// The commit a standard runtime says it was built from.
const STANDARD_COMMIT: &str = "test-commit";

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
            ("VERSION", format!("{version}\n").into_bytes()),
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
        let mut bytes = serde_json::to_vec_pretty(&self.build_info())
            .expect("a JSON value always has a JSON form");
        bytes.push(b'\n');
        bytes
    }

    /// The runtime's archive, field by field: each file deflated, in byte order of the names, then
    /// `build-info.json`, deflated too.
    pub fn raw(&self) -> RawZip {
        let mut files: Vec<_> = self.files.iter().collect();
        files.sort_by(|(one, _), (other, _)| one.cmp(other));
        let mut entries: Vec<RawEntry> = files
            .into_iter()
            .map(|(name, data)| RawEntry::deflated(name, data))
            .collect();
        entries.push(RawEntry::deflated(BUILD_INFO, &self.build_info_bytes()));
        RawZip {
            entries,
            ..RawZip::default()
        }
    }

    /// The runtime's archive, as the bytes of a zip file.
    pub fn zip(&self) -> Vec<u8> {
        self.raw().to_bytes()
    }
}

/// The SHA-256 digest of `data`, in lower-case hexadecimal.
fn sha256_hex(data: &[u8]) -> String {
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

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::io::{Cursor, Read};

    use ::zip::ZipArchive;
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

    /// The platforms a release is built for, and each one's executable.
    const PLATFORMS: [(&str, &str); 2] =
        [("windows-x64", "gauntlet.exe"), ("linux-x64", "gauntlet")];

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
}
