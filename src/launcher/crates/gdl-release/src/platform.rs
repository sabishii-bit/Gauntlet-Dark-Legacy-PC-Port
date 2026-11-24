//! The platforms a release is built for, under the names the release scripts give them, so that
//! the launcher and the publisher agree on which runtime and which executable serve which system.

use serde::{Deserialize, Deserializer, Serialize, Serializer, de};

/// A system and processor that a release carries a runtime for.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Platform {
    /// Windows on x86-64 processors.
    WindowsX64,
    /// Linux on x86-64 processors.
    LinuxX64,
}

impl Platform {
    /// Every platform a release is built for.
    pub const ALL: [Platform; 2] = [Platform::WindowsX64, Platform::LinuxX64];

    /// Reads a platform as a release's metadata names it, in exactly that spelling and case; any
    /// other text names no platform a release is built for.
    pub fn parse(text: &str) -> Option<Platform> {
        Platform::ALL
            .into_iter()
            .find(|platform| platform.as_str() == text)
    }

    /// The platform's name in a release's metadata, in its file names and in the release feed.
    pub fn as_str(self) -> &'static str {
        match self {
            Platform::WindowsX64 => "windows-x64",
            Platform::LinuxX64 => "linux-x64",
        }
    }

    /// The game's executable, at the top of a runtime built for the platform.
    pub fn executable(self) -> &'static str {
        match self {
            Platform::WindowsX64 => "gauntlet.exe",
            Platform::LinuxX64 => "gauntlet",
        }
    }

    /// The platform this program was built for: the only one whose runtime and installations it
    /// takes. `None` on a system or processor that no release is built for.
    pub fn of_this_build() -> Option<Platform> {
        if cfg!(all(windows, target_arch = "x86_64")) {
            Some(Platform::WindowsX64)
        } else if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
            Some(Platform::LinuxX64)
        } else {
            None
        }
    }
}

/// Written as its name in a release's metadata.
impl Serialize for Platform {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(self.as_str())
    }
}

/// Read from its name as [`Platform::parse`] reads one: any other text names no platform.
impl<'de> Deserialize<'de> for Platform {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Platform, D::Error> {
        let text = String::deserialize(deserializer)?;
        Platform::parse(&text)
            .ok_or_else(|| de::Error::custom(format_args!("No release is built for {text:?}")))
    }
}

#[cfg(test)]
mod tests {
    use std::env::consts::{ARCH, EXE_SUFFIX, OS};

    use serde_json::json;

    use super::Platform;

    /// The names the release scripts give the platforms, which every release's metadata and file
    /// names carry.
    const NAMES: [(&str, Platform); 2] = [
        ("windows-x64", Platform::WindowsX64),
        ("linux-x64", Platform::LinuxX64),
    ];

    #[test]
    fn names_round_trip() {
        assert_eq!(Platform::ALL.len(), NAMES.len());
        for (name, platform) in NAMES {
            assert!(Platform::ALL.contains(&platform), "ALL lists {name}");
            assert_eq!(platform.as_str(), name);
            assert_eq!(Platform::parse(name), Some(platform), "{name:?}");
        }
        for platform in Platform::ALL {
            assert_eq!(Platform::parse(platform.as_str()), Some(platform));
        }
        // The release scripts compare the names exactly, so no near miss names a platform.
        for text in [
            "",
            "another-os",
            "Windows-x64",
            "LINUX-X64",
            " linux-x64",
            "windows-x64 ",
            "linux-x64\n",
            "windows",
            "linux",
            "x64",
            "windows_x64",
            "windows-x86",
            "windows-arm64",
            "linux-x86_64",
            "linux-amd64",
            "linux-aarch64",
            "macos-x64",
        ] {
            assert_eq!(Platform::parse(text), None, "{text:?}");
        }
    }

    #[test]
    fn platforms_are_written_and_read_by_name() {
        for (name, platform) in NAMES {
            let written = serde_json::to_value(platform).expect("a platform has a JSON form");
            assert_eq!(written, json!(name));
            let read: Platform = serde_json::from_value(written).expect("its own name reads");
            assert_eq!(read, platform);
        }
        for given in [
            json!("Windows-x64"),
            json!("linux-x64 "),
            json!("macos-x64"),
            json!(""),
            json!(0),
            json!(null),
            json!(["windows-x64"]),
        ] {
            assert!(
                serde_json::from_value::<Platform>(given.clone()).is_err(),
                "{given} names no platform"
            );
        }
    }

    #[test]
    fn executables_pair_with_platforms() {
        assert_eq!(Platform::WindowsX64.executable(), "gauntlet.exe");
        assert_eq!(Platform::LinuxX64.executable(), "gauntlet");
    }

    #[test]
    fn this_build_has_a_platform() {
        let expected = match (OS, ARCH) {
            ("windows", "x86_64") => Some(Platform::WindowsX64),
            ("linux", "x86_64") => Some(Platform::LinuxX64),
            _ => None,
        };
        assert_eq!(Platform::of_this_build(), expected, "{OS} on {ARCH}");
        let Some(platform) = Platform::of_this_build() else {
            eprintln!("skipped: no release is built for {OS} on {ARCH}");
            return;
        };
        // The game's executable carries the suffix this system gives its programs.
        assert_eq!(platform.executable(), format!("gauntlet{EXE_SUFFIX}"));
    }
}
