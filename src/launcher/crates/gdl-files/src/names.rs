//! Names the launcher takes from outside, such as a runtime zip's entries, checked before anything
//! is made from them: no name may leave the folder it belongs in, name a Windows device, clash
//! with another on a case-insensitive file system, or carry the alpha installer's names into an
//! installation.

use std::collections::BTreeMap;
use std::collections::btree_map::Entry;
use std::fmt;
use std::path::{Path, PathBuf};

/// The most characters a component of a name may have.
pub const MAX_COMPONENT_CHARS: usize = 200;

/// The most characters a runtime zip's entry name may have, separators included.
pub const MAX_PAYLOAD_PATH_CHARS: usize = 1024;

/// Characters no component may hold: the separators of both systems, a drive's colon, and the
/// characters Windows keeps for wildcards, redirection and quoting.
pub const FORBIDDEN_CHARS: &str = "/\\:<>\"|?*";

/// The names the alpha installer leaves in a folder it installed or updated. No name from outside
/// may be one, at any depth and in any letter case, so that the launcher never writes one.
pub const ALPHA_NAMES: [&str; 7] = [
    "installation.json",
    ".gdl-update.lock",
    ".gdl-update",
    ".gdl-installer-update",
    ".gdl-install.lock",
    "GauntletDarkLegacy-Update",
    "GauntletDarkLegacy-Update.exe",
];

/// The names Windows reserves for its devices, in upper case: a file of one of these names,
/// whatever its extension, may open the device instead. The ports' superscript digits one to
/// three count too.
pub const DEVICE_NAMES: [&str; 32] = [
    "CON",
    "PRN",
    "AUX",
    "NUL",
    "CONIN$",
    "CONOUT$",
    "COM0",
    "COM1",
    "COM2",
    "COM3",
    "COM4",
    "COM5",
    "COM6",
    "COM7",
    "COM8",
    "COM9",
    "LPT0",
    "LPT1",
    "LPT2",
    "LPT3",
    "LPT4",
    "LPT5",
    "LPT6",
    "LPT7",
    "LPT8",
    "LPT9",
    "COM\u{b9}",
    "COM\u{b2}",
    "COM\u{b3}",
    "LPT\u{b9}",
    "LPT\u{b2}",
    "LPT\u{b3}",
];

/// What a kind of name is held to, beyond the rules every component follows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NameRules {
    /// The most characters a whole name may have, separators included.
    pub max_path_chars: usize,
    /// Whether every character must be ASCII.
    pub ascii_only: bool,
    /// Names a first component may not take, in any ASCII letter case: names the launcher keeps
    /// for files of its own beside those it takes.
    pub reserved_first: &'static [&'static str],
}

/// The rules for a runtime zip's entry names, which the release scripts write in ASCII alone.
pub const PAYLOAD_NAMES: NameRules = NameRules {
    max_path_chars: MAX_PAYLOAD_PATH_CHARS,
    ascii_only: true,
    reserved_first: &[],
};

/// The separator between a name's components, on every system.
const SEPARATOR: char = '/';

/// The rule a name breaks.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum NameRule {
    /// A component is empty: the name is empty, or has a separator at either end or two in a row.
    Empty,
    /// A component is `.` or `..`, which stand for a folder itself and for its parent.
    Dot,
    /// A component holds a control character: one below a space, or delete.
    Control,
    /// A component holds one of [`FORBIDDEN_CHARS`].
    Forbidden,
    /// A component holds a character outside ASCII where only ASCII is allowed.
    NonAscii,
    /// A component has more than [`MAX_COMPONENT_CHARS`] characters.
    ComponentTooLong,
    /// The name has more characters than its kind allows.
    PathTooLong,
    /// A component ends in a dot or a space, which Windows drops from a name.
    TrailingDotOrSpace,
    /// A component names a Windows device.
    DeviceName,
    /// A component is one of the alpha installer's names.
    AlphaName,
    /// The first component is a name its kind keeps for the launcher's own files.
    Reserved,
    /// The name is one a set holds already, in this or another ASCII letter case.
    Duplicate,
    /// The name is also the folder above another name of its set.
    Conflict,
}

impl NameRule {
    /// The rule's stable machine name, in camelCase, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            NameRule::Empty => "empty",
            NameRule::Dot => "dot",
            NameRule::Control => "control",
            NameRule::Forbidden => "forbidden",
            NameRule::NonAscii => "nonAscii",
            NameRule::ComponentTooLong => "componentTooLong",
            NameRule::PathTooLong => "pathTooLong",
            NameRule::TrailingDotOrSpace => "trailingDotOrSpace",
            NameRule::DeviceName => "deviceName",
            NameRule::AlphaName => "alphaName",
            NameRule::Reserved => "reserved",
            NameRule::Duplicate => "duplicate",
            NameRule::Conflict => "conflict",
        }
    }
}

/// The rule's machine name.
impl fmt::Display for NameRule {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// A name refused, and the rule it breaks.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("unsafe name {name:?}: {rule}")]
pub struct NameError {
    /// The name as it was given: the component that [`check_component`] was given, and otherwise
    /// the whole name.
    pub name: Box<str>,
    /// The first rule it breaks.
    pub rule: NameRule,
}

impl NameError {
    fn new(name: &str, rule: NameRule) -> NameError {
        NameError {
            name: name.into(),
            rule,
        }
    }
}

/// Checks one component of a name, what lies between two separators, and reports the first rule
/// it breaks.
pub fn check_component(name: &str, rules: &NameRules) -> Result<(), NameError> {
    match broken_rule(name, rules) {
        Some(rule) => Err(NameError::new(name, rule)),
        None => Ok(()),
    }
}

/// The first rule a component breaks, checked in a fixed order so that a name is always refused
/// for the same reason.
fn broken_rule(name: &str, rules: &NameRules) -> Option<NameRule> {
    let rule = if name.is_empty() {
        NameRule::Empty
    } else if name == "." || name == ".." {
        NameRule::Dot
    } else if name.chars().any(|c| c.is_ascii_control()) {
        NameRule::Control
    } else if name.contains(|c: char| FORBIDDEN_CHARS.contains(c)) {
        NameRule::Forbidden
    } else if rules.ascii_only && !name.is_ascii() {
        NameRule::NonAscii
    } else if longer_than(name, MAX_COMPONENT_CHARS) {
        NameRule::ComponentTooLong
    } else if name.ends_with(['.', ' ']) {
        NameRule::TrailingDotOrSpace
    } else if is_device_name(name) {
        NameRule::DeviceName
    } else if ALPHA_NAMES
        .iter()
        .any(|alpha| alpha.eq_ignore_ascii_case(name))
    {
        NameRule::AlphaName
    } else {
        return None;
    };
    Some(rule)
}

/// Whether a component names a Windows device: its part before the first dot, without the spaces
/// Windows drops after it, is one of [`DEVICE_NAMES`] in any letter case.
fn is_device_name(name: &str) -> bool {
    let stem = name.split_once('.').map_or(name, |(stem, _)| stem);
    let stem = stem.trim_end_matches(' ').to_uppercase();
    DEVICE_NAMES.contains(&stem.as_str())
}

/// Whether `text` has more than `chars` characters.
fn longer_than(text: &str, chars: usize) -> bool {
    text.chars().nth(chars).is_some()
}

/// A name from outside that has passed its kind's rules: components joined by `/`, none of which
/// can leave the folder the name is meant for. It is the only way such a name becomes a path.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RelPath(String);

impl RelPath {
    /// Reads a name under `rules`: no more characters than they allow, every component safe, and
    /// a first component they do not reserve. The first rule broken is reported.
    pub fn parse(text: &str, rules: &NameRules) -> Result<RelPath, NameError> {
        let refused = |rule| Err(NameError::new(text, rule));
        if longer_than(text, rules.max_path_chars) {
            return refused(NameRule::PathTooLong);
        }
        if let Some(rule) = text
            .split(SEPARATOR)
            .find_map(|component| broken_rule(component, rules))
        {
            return refused(rule);
        }
        let first = first_component(text);
        if rules
            .reserved_first
            .iter()
            .any(|reserved| reserved.eq_ignore_ascii_case(first))
        {
            return refused(NameRule::Reserved);
        }
        Ok(RelPath(text.to_owned()))
    }

    /// The name as it was read.
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// The name's components, first to last.
    pub fn components(&self) -> std::str::Split<'_, char> {
        self.0.split(SEPARATOR)
    }

    /// The first component: the root the name lies under, or the whole name when it has only one.
    pub fn first(&self) -> &str {
        first_component(&self.0)
    }

    /// How many components the name has.
    pub fn depth(&self) -> usize {
        self.0.matches(SEPARATOR).count() + 1
    }

    /// The name in ASCII lower case: the key that names differing only in letter case share, as
    /// they would share one file on a case-insensitive file system.
    pub fn folded(&self) -> String {
        self.0.to_ascii_lowercase()
    }

    /// The path the name stands for under `root`, joined component by component.
    pub fn path_under(&self, root: &Path) -> PathBuf {
        let mut path = root.to_path_buf();
        path.extend(self.components());
        path
    }
}

/// A name's first component, or the whole name when it has only one.
fn first_component(text: &str) -> &str {
    text.split_once(SEPARATOR).map_or(text, |(first, _)| first)
}

/// Names that will lie in one folder together, kept by their folded form, so that no two of them
/// differ only in ASCII letter case and none of them is also the folder above another.
#[derive(Clone, Debug, Default)]
pub struct NameSet {
    /// Each name, under its folded form.
    names: BTreeMap<String, RelPath>,
}

impl NameSet {
    /// Adds a name, refusing it as a duplicate when the set holds it already in any ASCII letter
    /// case.
    pub fn insert(&mut self, path: RelPath) -> Result<(), NameError> {
        match self.names.entry(path.folded()) {
            Entry::Occupied(_) => Err(NameError::new(path.as_str(), NameRule::Duplicate)),
            Entry::Vacant(slot) => {
                slot.insert(path);
                Ok(())
            }
        }
    }

    /// Refuses the set when one of its names is also the folder above another, in any ASCII
    /// letter case, since a file and a folder cannot share a name. The error names the one that
    /// would have to be both.
    pub fn conflicts(&self) -> Result<(), NameError> {
        for folded in self.names.keys() {
            for (end, _) in folded.match_indices(SEPARATOR) {
                if let Some(ancestor) = self.names.get(&folded[..end]) {
                    return Err(NameError::new(ancestor.as_str(), NameRule::Conflict));
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::path::Path;

    use super::{
        ALPHA_NAMES, DEVICE_NAMES, FORBIDDEN_CHARS, MAX_COMPONENT_CHARS, MAX_PAYLOAD_PATH_CHARS,
        NameError, NameRule, NameRules, NameSet, PAYLOAD_NAMES, RelPath, check_component,
    };

    /// Rules like a payload's that take any character, for a kind of name that may reach outside
    /// ASCII.
    const UNICODE_NAMES: NameRules = NameRules {
        ascii_only: false,
        ..PAYLOAD_NAMES
    };

    /// Rules like a payload's that keep two first components for files of the launcher's own, as
    /// an extracted dump's tree keeps `_program` and `manifest.json` for the extractor's.
    const RESERVING_NAMES: NameRules = NameRules {
        reserved_first: &["_program", "manifest.json"],
        ..PAYLOAD_NAMES
    };

    /// The rule a component breaks, checking that a refusal names the component it was given.
    fn component_rule(name: &str, rules: &NameRules) -> Option<NameRule> {
        let error = check_component(name, rules).err()?;
        assert_eq!(&*error.name, name, "the refusal names the component");
        Some(error.rule)
    }

    /// The rule a name breaks, checking that a refusal names the whole name and that a name read
    /// keeps its text.
    fn path_rule(text: &str, rules: &NameRules) -> Option<NameRule> {
        match RelPath::parse(text, rules) {
            Ok(path) => {
                assert_eq!(path.as_str(), text);
                None
            }
            Err(error) => {
                assert_eq!(&*error.name, text, "the refusal names the whole name");
                Some(error.rule)
            }
        }
    }

    fn rel(text: &str) -> RelPath {
        RelPath::parse(text, &PAYLOAD_NAMES).unwrap_or_else(|error| panic!("{error}"))
    }

    #[test]
    fn refuses_the_unsafe_components_python_refuses() {
        // The release scripts' own refusals, each for the rule it breaks first here.
        for (name, rule) in [
            ("..", NameRule::Dot),
            (".", NameRule::Dot),
            ("", NameRule::Empty),
            ("a/b", NameRule::Forbidden),
            ("a\\b", NameRule::Forbidden),
            ("C:", NameRule::Forbidden),
            ("CON.txt", NameRule::DeviceName),
            ("LPT1", NameRule::DeviceName),
            ("foo.", NameRule::TrailingDotOrSpace),
            ("foo ", NameRule::TrailingDotOrSpace),
            ("x\0y", NameRule::Control),
        ] {
            assert_eq!(component_rule(name, &PAYLOAD_NAMES), Some(rule), "{name:?}");
        }
        // The release scripts' characters, each refused wherever it stands.
        assert_eq!(FORBIDDEN_CHARS, "/\\:<>\"|?*");
        for forbidden in FORBIDDEN_CHARS.chars() {
            for name in [
                format!("a{forbidden}b"),
                forbidden.to_string(),
                format!("x{forbidden}"),
            ] {
                let rule = component_rule(&name, &PAYLOAD_NAMES);
                assert_eq!(rule, Some(NameRule::Forbidden), "{name:?}");
            }
        }
        for control in ('\0'..' ').chain(['\u{7f}']) {
            let name = format!("a{control}b");
            let rule = component_rule(&name, &PAYLOAD_NAMES);
            assert_eq!(rule, Some(NameRule::Control), "{name:?}");
        }
        // The first rule broken wins, whatever the order of the characters breaking them.
        for (name, rule) in [
            ("a/\0", NameRule::Control),
            ("\u{e9}\0", NameRule::Control),
            ("\u{e9}:", NameRule::Forbidden),
            ("\u{e9}.", NameRule::NonAscii),
            ("CON.", NameRule::TrailingDotOrSpace),
            ("NUL ", NameRule::TrailingDotOrSpace),
            ("...", NameRule::TrailingDotOrSpace),
        ] {
            assert_eq!(component_rule(name, &PAYLOAD_NAMES), Some(rule), "{name:?}");
        }
        // Names that only look like the refused ones are fine.
        for name in [
            "gauntlet.exe",
            "build-info.json",
            "portable.flag",
            ".hidden",
            "a..b",
            ".a",
            "a b",
            "CONSOLE",
            "CONIN",
            "xCON",
            "CON-x.txt",
            "COM10",
            "LPT",
            "COMx",
            "nul_file",
            "$",
            "~a",
        ] {
            assert_eq!(component_rule(name, &PAYLOAD_NAMES), None, "{name:?}");
        }
    }

    #[test]
    fn device_names_are_refused_in_every_spelling() {
        // The release scripts' list, then the ports' superscript digits one to three.
        let mut expected: Vec<String> = ["CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"]
            .into_iter()
            .map(String::from)
            .collect();
        for port in ["COM", "LPT"] {
            expected.extend((0..10).map(|digit| format!("{port}{digit}")));
        }
        for port in ["COM", "LPT"] {
            expected.extend(['\u{b9}', '\u{b2}', '\u{b3}'].map(|digit| format!("{port}{digit}")));
        }
        assert_eq!(DEVICE_NAMES.to_vec(), expected);
        for device in DEVICE_NAMES {
            // An upper-case stem is what each is compared with, so each must be one.
            assert_eq!(device.to_uppercase(), device);
            let rules = if device.is_ascii() {
                PAYLOAD_NAMES
            } else {
                UNICODE_NAMES
            };
            let lower = device.to_lowercase();
            let mixed: String = device
                .chars()
                .enumerate()
                .map(|(index, c)| {
                    if index % 2 == 0 {
                        c.to_ascii_lowercase()
                    } else {
                        c
                    }
                })
                .collect();
            for stem in [device, lower.as_str(), mixed.as_str()] {
                for name in [
                    stem.to_owned(),
                    format!("{stem}.txt"),
                    format!("{stem}.tar.gz"),
                    format!("{stem}..x"),
                ] {
                    let rule = component_rule(&name, &rules);
                    assert_eq!(rule, Some(NameRule::DeviceName), "{name:?}");
                }
            }
        }
    }

    #[test]
    fn refuses_superscript_device_names_where_names_may_be_unicode() {
        for name in ["COM\u{b9}", "LPT\u{b3}", "com\u{b2}.txt"] {
            let rule = component_rule(name, &UNICODE_NAMES);
            assert_eq!(rule, Some(NameRule::DeviceName), "{name:?}");
        }
        // A payload's names are ASCII, so it refuses them sooner.
        let rule = component_rule("COM\u{b9}", &PAYLOAD_NAMES);
        assert_eq!(rule, Some(NameRule::NonAscii));
        // Other digits and other scripts name no device.
        for name in ["COM\u{2074}", "LPT\u{2079}", "COM\u{2081}", "\u{e9}.json"] {
            assert_eq!(component_rule(name, &UNICODE_NAMES), None, "{name:?}");
        }
    }

    #[test]
    fn refuses_a_device_name_before_spaces() {
        for name in [
            "CON .txt",
            "aux.tar.gz",
            "NUL  .log",
            "conout$ .x",
            "Com1 .a.b",
        ] {
            let rule = component_rule(name, &PAYLOAD_NAMES);
            assert_eq!(rule, Some(NameRule::DeviceName), "{name:?}");
        }
        // Only the spaces after the stem are dropped from it.
        assert_eq!(component_rule("CON x.txt", &PAYLOAD_NAMES), None);
    }

    #[test]
    fn refuses_reserved_first_components() {
        for text in [
            "_program/x",
            "MANIFEST.JSON",
            "manifest.json",
            "_PROGRAM/sub/x",
            "Manifest.Json/x",
        ] {
            let rule = path_rule(text, &RESERVING_NAMES);
            assert_eq!(rule, Some(NameRule::Reserved), "{text:?}");
        }
        // Only the first component is reserved, and only by its whole name.
        for text in [
            "data/_program",
            "data/manifest.json",
            "_programs/x",
            "manifest.json.bak",
            "x_program",
        ] {
            assert_eq!(path_rule(text, &RESERVING_NAMES), None, "{text:?}");
        }
        // A payload reserves nothing.
        for text in ["_program/x", "manifest.json"] {
            assert_eq!(path_rule(text, &PAYLOAD_NAMES), None, "{text:?}");
        }
        // A component's own rules come first.
        assert_eq!(
            path_rule("_program/CON", &RESERVING_NAMES),
            Some(NameRule::DeviceName)
        );
    }

    #[test]
    fn refuses_alpha_names_at_any_depth_in_any_case() {
        // The alpha installer's names as its scripts write them: its receipt, the locks of its
        // game updates and of its installs, the transaction folders of its game updates and of
        // its own, and its updater under each system's name.
        assert_eq!(
            ALPHA_NAMES,
            [
                "installation.json",
                ".gdl-update.lock",
                ".gdl-update",
                ".gdl-installer-update",
                ".gdl-install.lock",
                "GauntletDarkLegacy-Update",
                "GauntletDarkLegacy-Update.exe",
            ]
        );
        for text in [
            "data/installation.json",
            "licenses/GauntletDarkLegacy-Update.exe",
            "shaders/.gdl-update/x",
            "lib/.GDL-INSTALL.LOCK",
        ] {
            let rule = path_rule(text, &PAYLOAD_NAMES);
            assert_eq!(rule, Some(NameRule::AlphaName), "{text:?}");
        }
        for alpha in ALPHA_NAMES {
            for name in [
                alpha.to_owned(),
                alpha.to_ascii_uppercase(),
                alpha.to_ascii_lowercase(),
            ] {
                let rule = component_rule(&name, &PAYLOAD_NAMES);
                assert_eq!(rule, Some(NameRule::AlphaName), "{name:?}");
                for text in [format!("data/{name}"), format!("a/b/{name}/c"), name] {
                    let rule = path_rule(&text, &PAYLOAD_NAMES);
                    assert_eq!(rule, Some(NameRule::AlphaName), "{text:?}");
                }
            }
        }
        // Names that only contain or resemble them are fine.
        for name in [
            "installation.json.bak",
            "my-installation.json",
            ".gdl-updates",
            ".gdl-update.locks",
            "GauntletDarkLegacy-Updater.exe",
            "GauntletDarkLegacy-Launcher.exe",
            "GauntletDarkLegacy-Update.dll",
        ] {
            assert_eq!(component_rule(name, &PAYLOAD_NAMES), None, "{name:?}");
        }
    }

    #[test]
    fn refuses_non_ascii_and_del_in_payload_names() {
        assert_eq!(
            path_rule("\u{e9}.json", &PAYLOAD_NAMES),
            Some(NameRule::NonAscii)
        );
        assert_eq!(
            path_rule("a\u{7f}b", &PAYLOAD_NAMES),
            Some(NameRule::Control)
        );
        assert_eq!(
            path_rule("data/text/\u{e9}.json", &PAYLOAD_NAMES),
            Some(NameRule::NonAscii)
        );
        assert_eq!(
            path_rule("shaders/\u{1d11e}.spv", &PAYLOAD_NAMES),
            Some(NameRule::NonAscii)
        );
        // Where names may reach outside ASCII, delete is still a control character.
        assert_eq!(path_rule("\u{e9}.json", &UNICODE_NAMES), None);
        assert_eq!(
            path_rule("a\u{7f}b", &UNICODE_NAMES),
            Some(NameRule::Control)
        );
    }

    #[test]
    fn limits_are_inclusive() {
        // The release scripts' limits: 200 characters a component and 1024 a payload's name.
        assert_eq!(MAX_COMPONENT_CHARS, 200);
        assert_eq!(MAX_PAYLOAD_PATH_CHARS, 1024);
        assert_eq!(PAYLOAD_NAMES.max_path_chars, MAX_PAYLOAD_PATH_CHARS);
        let longest = "a".repeat(MAX_COMPONENT_CHARS);
        assert_eq!(component_rule(&longest, &PAYLOAD_NAMES), None);
        let over = "a".repeat(MAX_COMPONENT_CHARS + 1);
        let rule = component_rule(&over, &PAYLOAD_NAMES);
        assert_eq!(rule, Some(NameRule::ComponentTooLong));
        assert_eq!(path_rule(&format!("data/{over}"), &PAYLOAD_NAMES), rule);
        // A component's length is checked after its characters and before how it ends.
        let wide_over = "\u{e9}".repeat(MAX_COMPONENT_CHARS + 1);
        let rule = component_rule(&wide_over, &PAYLOAD_NAMES);
        assert_eq!(rule, Some(NameRule::NonAscii));
        let rule = component_rule(&format!("{longest}."), &PAYLOAD_NAMES);
        assert_eq!(rule, Some(NameRule::ComponentTooLong));

        // Five full components, then one that brings the name to exactly its limit.
        let mut text = [longest.as_str(); 5].join("/");
        text.push('/');
        text.push_str(&"b".repeat(MAX_PAYLOAD_PATH_CHARS - text.len()));
        assert_eq!(text.chars().count(), MAX_PAYLOAD_PATH_CHARS);
        assert_eq!(path_rule(&text, &PAYLOAD_NAMES), None);
        text.push('b');
        assert_eq!(
            path_rule(&text, &PAYLOAD_NAMES),
            Some(NameRule::PathTooLong)
        );
        // The whole name's length is checked before its components'.
        let overlong = [over.as_str(); 6].join("/");
        assert_eq!(
            path_rule(&overlong, &PAYLOAD_NAMES),
            Some(NameRule::PathTooLong)
        );

        // Both limits count characters, not bytes.
        let wide = "\u{e9}".repeat(MAX_COMPONENT_CHARS);
        assert_eq!(component_rule(&wide, &UNICODE_NAMES), None);
        let rule = component_rule(&format!("{wide}\u{e9}"), &UNICODE_NAMES);
        assert_eq!(rule, Some(NameRule::ComponentTooLong));
        let short = NameRules {
            max_path_chars: 5,
            ..UNICODE_NAMES
        };
        assert_eq!(path_rule("\u{e9}/\u{1d11e}/a", &short), None);
        assert_eq!(
            path_rule("\u{e9}/\u{1d11e}/ab", &short),
            Some(NameRule::PathTooLong)
        );
    }

    #[test]
    fn names_never_leave_their_folder() {
        for (text, rule) in [
            ("", NameRule::Empty),
            ("/etc/passwd", NameRule::Empty),
            ("data/", NameRule::Empty),
            ("data//config.json", NameRule::Empty),
            ("../escaped", NameRule::Dot),
            ("data/../escaped", NameRule::Dot),
            ("data/./config.json", NameRule::Dot),
            ("a/../../x", NameRule::Dot),
            ("..\\escaped", NameRule::Forbidden),
            ("data\\back.json", NameRule::Forbidden),
            ("C:\\Windows", NameRule::Forbidden),
            ("C:x", NameRule::Forbidden),
            ("\\\\server\\share\\x", NameRule::Forbidden),
            ("config.json:stream", NameRule::Forbidden),
            ("shaders/a.spv\0.txt", NameRule::Control),
            ("data/CON", NameRule::DeviceName),
            ("data/x./y", NameRule::TrailingDotOrSpace),
        ] {
            assert_eq!(path_rule(text, &PAYLOAD_NAMES), Some(rule), "{text:?}");
        }
    }

    #[test]
    fn rel_paths_split_fold_and_join() {
        let path = rel("Data/Text/EN.json");
        assert_eq!(path.as_str(), "Data/Text/EN.json");
        assert_eq!(
            path.components().collect::<Vec<_>>(),
            ["Data", "Text", "EN.json"]
        );
        assert_eq!(path.first(), "Data");
        assert_eq!(path.depth(), 3);
        assert_eq!(path.folded(), "data/text/en.json");
        let root = Path::new("install");
        assert_eq!(
            path.path_under(root),
            root.join("Data").join("Text").join("EN.json")
        );

        let single = rel("VERSION");
        assert_eq!(single.components().collect::<Vec<_>>(), ["VERSION"]);
        assert_eq!(single.first(), "VERSION");
        assert_eq!(single.depth(), 1);
        assert_eq!(single.path_under(root), root.join("VERSION"));

        // Names order by their bytes, so that upper case sorts first.
        let mut sorted = [
            rel("data"),
            rel("VERSION"),
            rel("Shaders"),
            rel("build-info.json"),
        ];
        sorted.sort();
        let order: Vec<&str> = sorted.iter().map(RelPath::as_str).collect();
        assert_eq!(order, ["Shaders", "VERSION", "build-info.json", "data"]);
    }

    #[test]
    fn name_sets_refuse_case_duplicates_and_conflicts() {
        let mut names = NameSet::default();
        names.insert(rel("data/config.json")).unwrap();
        let error = names.insert(rel("DATA/config.json")).unwrap_err();
        assert_eq!(
            error,
            NameError {
                name: "DATA/config.json".into(),
                rule: NameRule::Duplicate
            }
        );
        let error = names.insert(rel("data/config.json")).unwrap_err();
        assert_eq!(error.rule, NameRule::Duplicate);
        names.insert(rel("data/text/en.json")).unwrap();
        names.insert(rel("shaders/test.spv")).unwrap();
        names.insert(rel("VERSION")).unwrap();
        names.conflicts().unwrap();

        // A name that is also another's folder, whichever comes first and in any case.
        for (first, second, both) in [
            ("lib", "lib/a.so", "lib"),
            ("lib/a.so", "lib", "lib"),
            ("LIB", "lib/a.so", "LIB"),
            ("data/text", "Data/Text/en.json", "data/text"),
            ("a", "A/b/c/d", "a"),
        ] {
            let mut names = NameSet::default();
            names.insert(rel(first)).unwrap();
            names.insert(rel(second)).unwrap();
            let error = names.conflicts().unwrap_err();
            assert_eq!(error.rule, NameRule::Conflict, "{first} and {second}");
            assert_eq!(&*error.name, both, "{first} and {second}");
        }
        // Names that only begin alike are no conflict.
        let mut names = NameSet::default();
        for text in ["lib", "lib-x/a", "library/a", "lib.so", "li/b"] {
            names.insert(rel(text)).unwrap();
        }
        names.conflicts().unwrap();
        NameSet::default().conflicts().unwrap();
    }

    #[test]
    fn rules_keep_their_names() {
        for (rule, name) in [
            (NameRule::Empty, "empty"),
            (NameRule::Dot, "dot"),
            (NameRule::Control, "control"),
            (NameRule::Forbidden, "forbidden"),
            (NameRule::NonAscii, "nonAscii"),
            (NameRule::ComponentTooLong, "componentTooLong"),
            (NameRule::PathTooLong, "pathTooLong"),
            (NameRule::TrailingDotOrSpace, "trailingDotOrSpace"),
            (NameRule::DeviceName, "deviceName"),
            (NameRule::AlphaName, "alphaName"),
            (NameRule::Reserved, "reserved"),
            (NameRule::Duplicate, "duplicate"),
            (NameRule::Conflict, "conflict"),
        ] {
            assert_eq!(rule.as_str(), name);
            assert_eq!(rule.to_string(), name);
        }
        let error = check_component("x\0y", &PAYLOAD_NAMES).unwrap_err();
        assert_eq!(error.to_string(), r#"unsafe name "x\0y": control"#);
        let error = RelPath::parse("data/../escaped", &PAYLOAD_NAMES).unwrap_err();
        assert_eq!(error.to_string(), r#"unsafe name "data/../escaped": dot"#);
    }
}
