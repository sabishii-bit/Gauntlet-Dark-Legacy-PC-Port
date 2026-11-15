//! The launcher's modes without a window, for the release scripts, CI and developers at a console:
//! `--version`, `--check-payload`, which checks the runtime the launcher file carries or a runtime
//! zip it is given, and `--inspect`, which reports what a folder is to the launcher. They run
//! before anything builds a window, and what they print is technical text for whoever runs them,
//! never words for players. A run is handed its arguments, its build, its own file's sections and
//! the streams to write to, so that every mode runs inside a test as it runs in the program.

use std::ffi::{OsStr, OsString};
use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, Write};
use std::path::Path;

use gdl_files::{Cancel, Job, NoProgress};
use gdl_install::{
    Access, INSPECTION_SCHEMA, Inspection, InstallFolder, InstallationSummary, Payload,
    PayloadError, Relation, Sections, SectionsError,
};
use gdl_release::Version;
use serde::Serialize;

use crate::codes::{self, Report};

/// The exit code of a mode that did what it was asked.
pub const EXIT_OK: u8 = 0;

/// The exit code of a mode that refused what it was given, or could not write its report.
pub const EXIT_REFUSED: u8 = 1;

/// The exit code of arguments that ask for a mode in a form it does not take, and of arguments
/// that ask for no mode at all.
pub const EXIT_USAGE: u8 = 2;

/// The line that answers arguments the program does not take.
const USAGE: &str = "usage: gdl-launcher-headless --check-payload [--payload <zip>] | \
                     --inspect <dir> [--json] | --version";

/// The mode that prints the program's version.
const VERSION_OPTION: &str = "--version";

/// The mode that checks a runtime: the launcher file's own, or the one `--payload` names.
const CHECK_PAYLOAD_OPTION: &str = "--check-payload";

/// The mode that reports what the folder after it is to the launcher.
const INSPECT_OPTION: &str = "--inspect";

/// The option of `--check-payload` that names a runtime zip to check in place of the launcher
/// file's own.
const PAYLOAD_OPTION: &str = "--payload";

/// The option of `--inspect` that makes it report in JSON.
const JSON_OPTION: &str = "--json";

/// The options that choose a mode: arguments with none of them are no headless run.
const MODE_OPTIONS: [&str; 3] = [VERSION_OPTION, CHECK_PAYLOAD_OPTION, INSPECT_OPTION];

/// How every option begins, and so how no option's value may.
const OPTION_PREFIX: &str = "--";

/// What `--check-payload` lists for a build that carries no optional feature.
const NO_FEATURES: &str = "none";

/// What joins the names of a list that a report gives on one line.
const LIST_SEPARATOR: &str = ", ";

/// The build of the program that runs a mode.
#[derive(Clone, Copy, Debug)]
pub struct Build<'a> {
    /// The version the program was built as: a semantic version, which its build script holds
    /// the project's version to.
    pub version: &'a str,
    /// The optional features the build carries, which `--check-payload` lists, so that a test
    /// build is never shipped as a release.
    pub features: &'a [&'a str],
}

/// The sections of the program's own file, as they were found when it started; `None` when the
/// file carries none, as a program fresh from the compiler does.
pub type OwnFile = Result<Option<Sections<File>>, SectionsError>;

/// Opens the program's own file and finds its sections, once. A program does this as it starts,
/// before anything else, and keeps what it gets: an update renames the running launcher aside and
/// puts the new one at its path, and the handle still reads the file the program was started
/// from.
pub fn open_own_file() -> OwnFile {
    Sections::of_running_launcher()
}

/// Runs the headless mode that `args`, the program's arguments after its own name, ask for, and
/// gives back its exit code; or `None` when no argument is `--check-payload`, `--inspect` or
/// `--version`, which is no headless run. A mode writes its report to `out` and a refusal to
/// `err`. Arguments that ask for a mode in a form it does not take get the usage line on `err`
/// and [`EXIT_USAGE`]. An option's value is the argument after it, which must not begin with
/// `--`; options may come in any order, each at most once.
///
/// - `--version` writes the build's version.
/// - `--check-payload` checks the runtime the launcher file carries, its sections as `own` holds
///   them: every section's bytes, the runtime's files against the payload's rules and its
///   inventory, and that the runtime is of this build's platform and of exactly the build's
///   version, as text. With `--payload <zip>` it checks that zip in the same way instead, and
///   never looks at the launcher file. It writes `Verified <version> (<platform>)` and a line
///   listing the build's features, or `none`; a refusal is the line
///   `error: <code> (<rule>): <detail>`, where [`codes`] gives the code, and [`EXIT_REFUSED`].
/// - `--inspect <dir>` reports what the folder is to a launcher of the build's version, as
///   [`InstallFolder::inspect`] finds it: a `name: value` line for each field that holds
///   something, or with `--json` every field as JSON. Whatever the folder holds, an inspection
///   exits [`EXIT_OK`].
///
/// A report that cannot be written to its stream gives [`EXIT_REFUSED`], since nobody saw it.
///
/// Panics when `--inspect` runs as a build whose version is not a semantic version, which no
/// build script lets a build be.
pub fn run(
    args: &[OsString],
    build: &Build<'_>,
    own: &mut OwnFile,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> Option<u8> {
    let mode = match mode_of(args)? {
        Ok(mode) => mode,
        Err(Misused) => return Some(usage(err)),
    };
    Some(match mode {
        Mode::Version => reported(out, &format!("{}\n", build.version)),
        Mode::CheckPayload { zip } => check_payload(zip, build, own, out, err),
        Mode::Inspect { folder, json } => inspect(Path::new(folder), json, build, out),
    })
}

/// Writes the usage line to `err` and gives back [`EXIT_USAGE`], which the program exits with
/// whether or not the line could be written.
pub fn usage(err: &mut dyn Write) -> u8 {
    let _ = written(err, &format!("{USAGE}\n"));
    EXIT_USAGE
}

/// A headless mode, as the arguments ask for it.
#[derive(Debug)]
enum Mode<'a> {
    /// `--version`.
    Version,
    /// `--check-payload`, of the runtime zip at `zip` when `--payload` names one, else of the
    /// launcher file's own runtime.
    CheckPayload { zip: Option<&'a OsStr> },
    /// `--inspect`, of `folder`, reported as JSON when `json` says so, else as text.
    Inspect { folder: &'a OsStr, json: bool },
}

/// The mode option the arguments give, before the other options are held to it.
enum Chosen<'a> {
    /// `--version`.
    Version,
    /// `--check-payload`.
    CheckPayload,
    /// `--inspect`, with the folder after it.
    Inspect(&'a OsStr),
}

/// Arguments that ask for a mode in a form it does not take.
#[derive(Debug)]
struct Misused;

/// The mode `args` ask for, or `None` when none of them is an option that chooses a mode.
fn mode_of(args: &[OsString]) -> Option<Result<Mode<'_>, Misused>> {
    let headless = args
        .iter()
        .any(|arg| MODE_OPTIONS.iter().any(|option| arg == option));
    headless.then(|| parse(args))
}

/// Reads `args`, which name at least one mode, as one of the forms the modes take: one mode, and
/// only the options that mode has, each once.
fn parse(args: &[OsString]) -> Result<Mode<'_>, Misused> {
    let mut chosen = None;
    let mut zip = None;
    let mut json = false;
    let mut rest = args.iter().map(OsString::as_os_str);
    while let Some(arg) = rest.next() {
        let mode = match arg.to_str() {
            Some(VERSION_OPTION) => Chosen::Version,
            Some(CHECK_PAYLOAD_OPTION) => Chosen::CheckPayload,
            Some(INSPECT_OPTION) => Chosen::Inspect(value(&mut rest)?),
            Some(PAYLOAD_OPTION) if zip.is_none() => {
                zip = Some(value(&mut rest)?);
                continue;
            }
            Some(JSON_OPTION) if !json => {
                json = true;
                continue;
            }
            _ => return Err(Misused),
        };
        if chosen.replace(mode).is_some() {
            return Err(Misused);
        }
    }
    match (chosen, zip, json) {
        (Some(Chosen::Version), None, false) => Ok(Mode::Version),
        (Some(Chosen::CheckPayload), zip, false) => Ok(Mode::CheckPayload { zip }),
        (Some(Chosen::Inspect(folder)), None, json) => Ok(Mode::Inspect { folder, json }),
        _ => Err(Misused),
    }
}

/// The value of the option just read: the next argument, which must be there and must not be an
/// option itself.
fn value<'a>(rest: &mut impl Iterator<Item = &'a OsStr>) -> Result<&'a OsStr, Misused> {
    rest.next()
        .filter(|value| {
            !value
                .as_encoded_bytes()
                .starts_with(OPTION_PREFIX.as_bytes())
        })
        .ok_or(Misused)
}

/// `--check-payload`: checks the runtime zip at `zip`, or without one the runtime the launcher
/// file carries in its sections, `own`, and reports it verified to `out` or refused to `err`.
fn check_payload(
    zip: Option<&OsStr>,
    build: &Build<'_>,
    own: &mut OwnFile,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> u8 {
    // Nothing here asks the check to stop, and nobody follows its progress.
    let cancel = Cancel::default();
    let job = Job {
        cancel: &cancel,
        progress: &NoProgress,
    };
    let checked = match (zip, own) {
        (Some(zip), _) => File::open(zip)
            .map_err(PayloadError::from)
            .and_then(|file| check_runtime(Payload::open(file), build, &job)),
        (None, Ok(Some(sections))) => check_own(sections, build, &job),
        (None, Ok(None)) => Err(PayloadError::from(SectionsError::NoSections)),
        (None, Err(error)) => {
            let error: &SectionsError = error;
            return refuse(err, codes::sections(error), error.rule(), error);
        }
    };
    match checked {
        Ok(verified) => reported(out, &format!("{verified}\nFeatures: {}\n", features(build))),
        Err(error) => refuse(err, codes::payload(&error), error.rule(), &error),
    }
}

/// Checks the runtime the launcher file's sections carry, every section's bytes first, as
/// [`check_runtime`] checks a runtime.
fn check_own(
    sections: &mut Sections<File>,
    build: &Build<'_>,
    job: &Job<'_>,
) -> Result<String, PayloadError> {
    sections.verify(job)?;
    check_runtime(sections.runtime(), build, job)
}

/// Holds the runtime `opened` gives to `build`: of the platform it was built for and of exactly
/// its version, every file holding the bytes the runtime's release record lists. Gives back the
/// line that reports it verified.
fn check_runtime<R: Read + Seek>(
    opened: Result<Payload<R>, PayloadError>,
    build: &Build<'_>,
    job: &Job<'_>,
) -> Result<String, PayloadError> {
    let mut payload = opened?;
    payload.for_launcher(build.version)?;
    payload.verify(job)?;
    let record = payload.build_info();
    Ok(format!(
        "Verified {} ({})",
        record.version,
        record.platform.as_str()
    ))
}

/// The build's optional features as `--check-payload` lists them: on one line, or `none`.
fn features(build: &Build<'_>) -> String {
    if build.features.is_empty() {
        NO_FEATURES.to_owned()
    } else {
        build.features.join(LIST_SEPARATOR)
    }
}

/// Writes the line that reports a refusal to `err`: the code `report` gives it, the rule it names
/// and its `detail`; or, for a job asked to stop, only that it was cancelled. Gives back
/// [`EXIT_REFUSED`], written or not.
fn refuse(err: &mut dyn Write, report: Report, rule: &str, detail: &dyn fmt::Display) -> u8 {
    let line = match report {
        Report::Error(code) => format!("error: {code} ({rule}): {detail}\n"),
        Report::Cancelled => "error: cancelled\n".to_owned(),
    };
    let _ = written(err, &line);
    EXIT_REFUSED
}

/// `--inspect`: reports to `out` what the folder at `folder` is to a launcher of the build's
/// version, as JSON when `json` says so.
fn inspect(folder: &Path, json: bool, build: &Build<'_>, out: &mut dyn Write) -> u8 {
    let running = Version::parse(build.version).expect("the build's version is a semantic version");
    let inspection = InstallFolder::inspect(folder, &running);
    let view = InspectionView::of(&inspection);
    let report = if json {
        let mut text =
            serde_json::to_string_pretty(&view).expect("an inspection's fields are all JSON");
        text.push('\n');
        text
    } else {
        view.text()
    };
    reported(out, &report)
}

/// Writes `report`, all that a mode tells whoever runs it, to `out`, and gives back the exit code
/// of a mode that did what it was asked: [`EXIT_OK`] once the report is out, and [`EXIT_REFUSED`]
/// when it cannot be written.
fn reported(out: &mut dyn Write, report: &str) -> u8 {
    match written(out, report) {
        Ok(()) => EXIT_OK,
        Err(_) => EXIT_REFUSED,
    }
}

/// Writes `text` whole to `to` and flushes it, so that it is out before the program ends.
fn written(to: &mut dyn Write, text: &str) -> io::Result<()> {
    to.write_all(text.as_bytes())?;
    to.flush()
}

/// What `--inspect` reports of a folder, field by field in the order of the report, each under
/// its camelCase name in JSON and in text alike.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct InspectionView<'a> {
    /// The report's schema.
    schema: u32,
    /// The folder, absolute and without a verbatim prefix.
    path: &'a str,
    /// What the folder is to the launcher.
    state: &'static str,
    /// The code of the problem the state shows, when it shows one.
    code: Option<&'static str>,
    /// The text of the refusal that decided the state, as the refusal gives it, paths and all.
    detail: Option<&'a str>,
    /// The alpha installer's names at the folder's top.
    alpha_markers: &'a [&'static str],
    /// The journals of the jobs left unfinished in the folder.
    journals: &'a [String],
    /// The names in the launcher's state folder that its rules do not make.
    unknown_names: &'a [String],
    /// The installation, when its receipt and files read as one.
    installation: Option<InstallationView<'a>>,
}

/// What `--inspect` reports of the installation in a folder.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct InstallationView<'a> {
    /// The installed release's version.
    version: &'a str,
    /// The commit the release was built from.
    commit: &'a str,
    /// The platform the release was built for.
    platform: &'static str,
    /// The disc the game's data was copied from, unless a later launcher wrote the receipt.
    disc: Option<&'a str>,
    /// That disc's revision, unless a later launcher wrote the receipt.
    disc_revision: Option<u8>,
    /// The version of the launcher that looked.
    launcher: &'a str,
    /// How that launcher stands against the installed release.
    relation: &'static str,
    /// What that launcher may do with the installation.
    access: &'static str,
}

impl<'a> InspectionView<'a> {
    /// The report of `inspection`.
    fn of(inspection: &'a Inspection) -> InspectionView<'a> {
        InspectionView {
            schema: INSPECTION_SCHEMA,
            path: &inspection.path,
            state: inspection.state.as_str(),
            code: codes::state(inspection.state),
            detail: inspection.detail.as_deref(),
            alpha_markers: &inspection.alpha_markers,
            journals: &inspection.journals,
            unknown_names: &inspection.unknown_names,
            installation: inspection.installation.as_ref().map(InstallationView::of),
        }
    }

    /// The report as text: a `name: value` line for each field, in the report's order, leaving
    /// out the schema and every field that holds nothing, with a list's names on one line.
    fn text(&self) -> String {
        let mut lines = vec![
            ("state", self.state.to_owned()),
            ("path", self.path.to_owned()),
        ];
        lines.extend(self.code.map(|code| ("code", code.to_owned())));
        lines.extend(self.detail.map(|detail| ("detail", detail.to_owned())));
        if !self.alpha_markers.is_empty() {
            lines.push(("alphaMarkers", self.alpha_markers.join(LIST_SEPARATOR)));
        }
        for (name, list) in [
            ("journals", self.journals),
            ("unknownNames", self.unknown_names),
        ] {
            if !list.is_empty() {
                lines.push((name, list.join(LIST_SEPARATOR)));
            }
        }
        if let Some(installation) = &self.installation {
            installation.lines(&mut lines);
        }
        lines
            .into_iter()
            .map(|(name, value)| format!("{name}: {value}\n"))
            .collect()
    }
}

impl<'a> InstallationView<'a> {
    /// The report of `summary`.
    fn of(summary: &'a InstallationSummary) -> InstallationView<'a> {
        InstallationView {
            version: &summary.version,
            commit: &summary.commit,
            platform: summary.platform.as_str(),
            disc: summary.disc.as_deref(),
            disc_revision: summary.disc_revision,
            launcher: &summary.launcher,
            relation: relation_name(summary.relation),
            access: access_name(summary.access),
        }
    }

    /// Adds the report's lines to `lines`, in its order, leaving out what it does not know.
    fn lines(&self, lines: &mut Vec<(&'static str, String)>) {
        lines.push(("version", self.version.to_owned()));
        lines.push(("commit", self.commit.to_owned()));
        lines.push(("platform", self.platform.to_owned()));
        lines.extend(self.disc.map(|disc| ("disc", disc.to_owned())));
        lines.extend(
            self.disc_revision
                .map(|revision| ("discRevision", revision.to_string())),
        );
        lines.push(("launcher", self.launcher.to_owned()));
        lines.push(("relation", self.relation.to_owned()));
        lines.push(("access", self.access.to_owned()));
    }
}

/// How a report names `relation`, in camelCase.
fn relation_name(relation: Relation) -> &'static str {
    match relation {
        Relation::Older => "older",
        Relation::Same => "same",
        Relation::Newer => "newer",
    }
}

/// How a report names `access`, in camelCase.
fn access_name(access: Access) -> &'static str {
    match access {
        Access::Writable => "writable",
        Access::ReadOnly => "readOnly",
    }
}

#[cfg(test)]
mod tests {
    use std::ffi::{OsStr, OsString};
    use std::fs;
    use std::io::{self, Write};
    use std::path::{Path, PathBuf};

    use gdl_fixtures::{
        InstallFixture, RawEntry, RuntimeFiles, append_sections, make_alpha_markers, pretty_json,
        tiny_elf,
    };
    use gdl_install::{
        DEFAULT_CONFIG, GUNE5D, InstallFolder, JOURNAL_DIR, RUNTIME_SECTION, STATE_DIR, Sections,
        SectionsError,
    };
    use gdl_release::{Platform, Version};
    use serde_json::{Value, json};

    use super::{Build, EXIT_OK, EXIT_REFUSED, EXIT_USAGE, OwnFile, USAGE, run, usage};

    /// The version the tests build the program as: the first launcher's.
    const VERSION: &str = "1.0.0-beta";

    /// A build of that version without optional features, as a release is built.
    const RELEASE: Build<'static> = Build {
        version: VERSION,
        features: &[],
    };

    /// The commit a standard runtime records.
    const COMMIT: &str = "test-commit";

    /// The disc revision an installation fixture's receipt records.
    const DISC_REVISION: u8 = 1;

    /// The keys of an inspection's JSON report, in the order it writes them.
    const REPORT_KEYS: [&str; 9] = [
        "schema",
        "path",
        "state",
        "code",
        "detail",
        "alphaMarkers",
        "journals",
        "unknownNames",
        "installation",
    ];

    /// The keys of the installation's part of that report, in the order it writes them.
    const INSTALLATION_KEYS: [&str; 8] = [
        "version",
        "commit",
        "platform",
        "disc",
        "discRevision",
        "launcher",
        "relation",
        "access",
    ];

    /// What a run gave back and wrote.
    #[derive(Debug)]
    struct Ran {
        code: Option<u8>,
        out: String,
        err: String,
    }

    /// A stream that refuses every write, as a closed pipe does.
    struct Closed;

    impl Write for Closed {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::Error::from(io::ErrorKind::BrokenPipe))
        }

        fn flush(&mut self) -> io::Result<()> {
            Err(io::Error::from(io::ErrorKind::BrokenPipe))
        }
    }

    /// Runs the headless modes with `args`, as `build`, with `own` as the sections the program's
    /// own file was found to carry.
    fn run_as<A: AsRef<OsStr>>(args: &[A], build: &Build<'_>, own: &mut OwnFile) -> Ran {
        let args: Vec<OsString> = args.iter().map(|arg| arg.as_ref().to_owned()).collect();
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let code = run(&args, build, own, &mut out, &mut err);
        Ran {
            code,
            out: String::from_utf8(out).unwrap(),
            err: String::from_utf8(err).unwrap(),
        }
    }

    /// Runs them with `args` as a release build whose own file carries no sections, as a program
    /// fresh from the compiler.
    fn run_release<A: AsRef<OsStr>>(args: &[A]) -> Ran {
        run_as(args, &RELEASE, &mut Ok(None))
    }

    /// Checks the runtime zip at `zip` as `build`.
    fn check(zip: &Path, build: &Build<'_>) -> Ran {
        let args = [
            OsStr::new("--check-payload"),
            OsStr::new("--payload"),
            zip.as_os_str(),
        ];
        run_as(&args, build, &mut Ok(None))
    }

    /// Checks that `ran` was refused with exactly the line `line`, and wrote nothing else.
    #[track_caller]
    fn assert_refused(ran: &Ran, line: &str) {
        assert_eq!(ran.code, Some(EXIT_REFUSED), "{ran:?}");
        assert_eq!(ran.err, line);
        assert_eq!(ran.out, "");
    }

    /// The platform this test program was built for, whose runtimes the program takes; `None`, the
    /// reason printed, on a system no release is built for.
    fn this_platform() -> Option<Platform> {
        let platform = Platform::of_this_build();
        if platform.is_none() {
            eprintln!("skipped: no release is built for this system");
        }
        platform
    }

    /// The report of a runtime of `platform` verified by a build whose features are `features`.
    fn verified(platform: Platform, features: &str) -> String {
        format!(
            "Verified {VERSION} ({})\nFeatures: {features}\n",
            platform.as_str()
        )
    }

    /// Writes the runtime zip `zip` as `name` in `dir`, and gives back its path.
    fn zip_file(dir: &Path, name: &str, zip: &[u8]) -> PathBuf {
        let path = dir.join(name);
        fs::write(&path, zip).unwrap();
        path
    }

    /// Writes the standard runtime of `platform` and `version` as `name` in `dir`.
    fn standard_zip(dir: &Path, name: &str, platform: &str, version: &str) -> PathBuf {
        zip_file(dir, name, &RuntimeFiles::standard(platform, version).zip())
    }

    /// An argument that is not Unicode, as a path may be.
    #[cfg(unix)]
    fn not_unicode() -> OsString {
        use std::os::unix::ffi::OsStringExt;
        OsString::from_vec(b"caf\xe9".to_vec())
    }

    /// An argument that is not Unicode, as a path may be.
    #[cfg(windows)]
    fn not_unicode() -> OsString {
        use std::os::windows::ffi::OsStringExt;
        OsString::from_wide(&[u16::from(b'c'), u16::from(b'a'), u16::from(b'f'), 0xD800])
    }

    /// Inspects the folder at `root` as the release build, as JSON when `json` says so, and gives
    /// back the report, which an inspection always writes.
    #[track_caller]
    fn inspected(root: &Path, json: bool) -> String {
        let mut args = vec![OsStr::new("--inspect"), root.as_os_str()];
        if json {
            args.push(OsStr::new("--json"));
        }
        let ran = run_release(args.as_slice());
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.err, "");
        ran.out
    }

    /// The text report's lines for an installation of `version` for `platform`, which the release
    /// build stands to as `relation` and may use as `access` says; the disc's lines only when
    /// `disc` says the receipt names it.
    fn installation_lines(
        version: &str,
        platform: Platform,
        relation: &str,
        access: &str,
        disc: bool,
    ) -> String {
        let disc = if disc {
            format!("disc: {GUNE5D}\ndiscRevision: {DISC_REVISION}\n")
        } else {
            String::new()
        };
        format!(
            "version: {version}\ncommit: {COMMIT}\nplatform: {}\n{disc}launcher: {VERSION}\n\
             relation: {relation}\naccess: {access}\n",
            platform.as_str()
        )
    }

    /// The keys a JSON report writes at `indent` spaces, in the order it writes them.
    fn keys_at(report: &str, indent: usize) -> Vec<String> {
        let margin = " ".repeat(indent);
        report
            .lines()
            .filter_map(|line| line.strip_prefix(&margin)?.strip_prefix('"'))
            .filter_map(|rest| {
                let (key, after) = rest.split_once('"')?;
                after.starts_with(':').then(|| key.to_owned())
            })
            .collect()
    }

    /// The inspection's own refusal text for the folder at `root`, as the launcher of the release
    /// build finds it.
    fn detail_of(root: &Path) -> Option<String> {
        InstallFolder::inspect(root, &Version::parse(VERSION).unwrap()).detail
    }

    #[test]
    fn usage_errors_exit_2() {
        let cases: &[(&[&str], &str)] = &[
            (
                &["--version", "--verbose"],
                "an unknown option beside a mode",
            ),
            (&["--inspect", "folder", "-j"], "an unknown short option"),
            (&["--version", "extra"], "an argument no option takes"),
            (&["--inspect", "folder", "other"], "a second folder"),
            (&["--version", "--check-payload"], "two modes"),
            (&["--check-payload", "--inspect", "folder"], "two modes"),
            (&["--inspect", "folder", "--version"], "two modes"),
            (&["--version", "--version"], "one mode twice"),
            (
                &["--check-payload", "--payload"],
                "--payload without a value",
            ),
            (
                &["--payload", "--check-payload"],
                "--payload followed by an option",
            ),
            (
                &[
                    "--check-payload",
                    "--payload",
                    "a.zip",
                    "--payload",
                    "b.zip",
                ],
                "--payload twice",
            ),
            (
                &["--version", "--payload", "a.zip"],
                "--payload outside --check-payload",
            ),
            (
                &["--inspect", "folder", "--payload", "a.zip"],
                "--payload outside --check-payload",
            ),
            (&["--check-payload", "--json"], "--json without --inspect"),
            (&["--version", "--json"], "--json without --inspect"),
            (&["--inspect", "folder", "--json", "--json"], "--json twice"),
            (&["--inspect"], "--inspect without a folder"),
            (&["--inspect", "--json"], "--inspect followed by an option"),
            (&["--json", "--inspect"], "--inspect at the end"),
        ];
        for &(args, what) in cases {
            let ran = run_release(args);
            assert_eq!(ran.code, Some(EXIT_USAGE), "{what}: {args:?}");
            assert_eq!(ran.err, format!("{USAGE}\n"), "{what}: {args:?}");
            assert_eq!(ran.out, "", "{what}: {args:?}");
        }
        // The usage alone, as the program answers arguments that are no headless run.
        let mut err = Vec::new();
        assert_eq!(usage(&mut err), EXIT_USAGE);
        assert_eq!(String::from_utf8(err).unwrap(), format!("{USAGE}\n"));
        assert_eq!(
            USAGE,
            "usage: gdl-launcher-headless --check-payload [--payload <zip>] | --inspect <dir> \
             [--json] | --version"
        );
    }

    #[test]
    fn other_arguments_are_not_headless() {
        let cases: &[&[&str]] = &[
            &[],
            &["--allow-root"],
            &["--payload", "runtime.zip"],
            &["--json"],
            &["folder"],
            &["-version"],
            &["--VERSION"],
            &["--version=1.0.0"],
            &["--inspect=folder"],
            &["--check-payload=runtime.zip"],
        ];
        for &args in cases {
            let ran = run_release(args);
            assert_eq!(ran.code, None, "{args:?}: {ran:?}");
            assert_eq!((ran.out.as_str(), ran.err.as_str()), ("", ""), "{args:?}");
        }
        let ran = run_release(&[not_unicode()]);
        assert_eq!(ran.code, None, "{ran:?}");
    }

    #[test]
    fn version_prints_the_build_version() {
        let ran = run_release(&["--version"]);
        assert_eq!(ran.code, Some(EXIT_OK));
        assert_eq!((ran.out.as_str(), ran.err.as_str()), ("1.0.0-beta\n", ""));
        // As the build gives it, build metadata and all, whatever its own file holds.
        let build = Build {
            version: "2.3.4-rc.1+build.5",
            features: &["test-hooks"],
        };
        let ran = run_as(&["--version"], &build, &mut Err(SectionsError::NoSections));
        assert_eq!(ran.code, Some(EXIT_OK));
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("2.3.4-rc.1+build.5\n", "")
        );
    }

    #[test]
    fn a_payload_file_verifies() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let zip = standard_zip(dir.path(), "runtime.zip", platform.as_str(), VERSION);
        // The launcher file's own sections are neither read nor needed, so not even a file that
        // could not be read stands in the way.
        let denied = io::Error::from(io::ErrorKind::PermissionDenied);
        let mut own: OwnFile = Err(SectionsError::Io(denied));
        let (mode, option) = (OsStr::new("--check-payload"), OsStr::new("--payload"));
        for args in [
            [mode, option, zip.as_os_str()],
            [option, zip.as_os_str(), mode],
        ] {
            let ran = run_as(&args, &RELEASE, &mut own);
            assert_eq!(ran.code, Some(EXIT_OK), "{args:?}: {ran:?}");
            assert_eq!(ran.out, verified(platform, "none"), "{args:?}");
            assert_eq!(ran.err, "", "{args:?}");
        }
    }

    #[test]
    fn a_payload_of_another_version_is_refused() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let newer = standard_zip(dir.path(), "newer.zip", platform.as_str(), "1.0.1");
        assert_refused(
            &check(&newer, &RELEASE),
            "error: payload.damaged (otherVersion): The runtime's version \"1.0.1\" is not the \
             launcher's version \"1.0.0-beta\"\n",
        );
        // Versions are compared as text, so build metadata that leaves their order alone still
        // makes another version.
        let own = standard_zip(dir.path(), "own.zip", platform.as_str(), VERSION);
        let build = Build {
            version: "1.0.0-beta+rebuilt",
            features: &[],
        };
        assert_refused(
            &check(&own, &build),
            "error: payload.damaged (otherVersion): The runtime's version \"1.0.0-beta\" is not \
             the launcher's version \"1.0.0-beta+rebuilt\"\n",
        );
        // A runtime of this version for the other platform is refused for its platform.
        for other in Platform::ALL.into_iter().filter(|&other| other != platform) {
            let foreign = standard_zip(dir.path(), "foreign.zip", other.as_str(), VERSION);
            assert_refused(
                &check(&foreign, &RELEASE),
                &format!(
                    "error: payload.damaged (otherPlatform): This installer was built for a \
                     different operating system: the runtime is for {}\n",
                    other.as_str()
                ),
            );
        }
    }

    #[test]
    fn a_damaged_payload_file_is_refused() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        // data/config.json's bytes changed under the release record that lists the old ones.
        let mut raw = RuntimeFiles::standard(platform.as_str(), VERSION).raw();
        let entry = raw
            .entries
            .iter_mut()
            .find(|entry| entry.name == DEFAULT_CONFIG.as_bytes())
            .unwrap();
        *entry = RawEntry::deflated(DEFAULT_CONFIG, b"damaged");
        let damaged = zip_file(dir.path(), "damaged.zip", &raw.to_bytes());
        assert_refused(
            &check(&damaged, &RELEASE),
            "error: payload.damaged (damaged): Damaged release payload: data/config.json: its \
             SHA-256 digest is not the one its release record gives it\n",
        );
        // A file that is no zip at all.
        let text = zip_file(dir.path(), "notes.txt", b"not a zip");
        assert_refused(
            &check(&text, &RELEASE),
            "error: payload.damaged (archive): Not a runtime zip: it does not end in a ZIP end \
             record without a comment\n",
        );
        // A file that is not there cannot be read, which is no damage.
        let ran = check(&dir.path().join("missing.zip"), &RELEASE);
        assert_eq!(ran.code, Some(EXIT_REFUSED), "{ran:?}");
        let prefix = "error: io.failed (io): The runtime zip cannot be read or copied: ";
        assert!(ran.err.starts_with(prefix), "{}", ran.err);
        assert_eq!(ran.err.lines().count(), 1, "{}", ran.err);
        assert!(ran.err.ends_with('\n'), "{}", ran.err);
        assert_eq!(ran.out, "");
    }

    #[test]
    fn an_own_file_without_sections_is_refused() {
        assert_refused(
            &run_release(&["--check-payload"]),
            "error: payload.damaged (noSections): the file carries no sections\n",
        );
    }

    #[test]
    fn an_own_file_error_is_refused() {
        let denied = io::Error::from(io::ErrorKind::PermissionDenied);
        let unread = format!("error: io.failed (io): the launcher file cannot be read: {denied}\n");
        for (own, line) in [
            (SectionsError::Io(denied), unread),
            (
                SectionsError::UnknownProgram,
                "error: payload.damaged (unknownProgram): not a PE or ELF program\n".to_owned(),
            ),
            (
                SectionsError::TableHash,
                "error: payload.damaged (tableHash): the table's SHA-256 does not match the \
                 trailer\n"
                    .to_owned(),
            ),
            // A job asked to stop is no problem of the file's, so it gets no code.
            (SectionsError::Cancelled, "error: cancelled\n".to_owned()),
        ] {
            let ran = run_as(&["--check-payload"], &RELEASE, &mut Err(own));
            assert_refused(&ran, &line);
        }
    }

    #[test]
    fn a_sectioned_own_file_verifies() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(platform.as_str(), VERSION).zip();
        let mut file = tiny_elf();
        let listed = append_sections(&mut file, &[(RUNTIME_SECTION, &runtime)], 0);
        let path = dir.path().join("launcher");
        fs::write(&path, &file).unwrap();
        let mut own = Sections::of_path(&path);
        let ran = run_as(&["--check-payload"], &RELEASE, &mut own);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.out, verified(platform, "none"));
        assert_eq!(ran.err, "");

        // One whose runtime changed after its table was written is refused for the section's
        // digest, before the runtime in it is read.
        let start = usize::try_from(listed[0].1).unwrap();
        file[start] ^= 0x01;
        let changed = dir.path().join("changed");
        fs::write(&changed, &file).unwrap();
        let mut own = Sections::of_path(&changed);
        assert_refused(
            &run_as(&["--check-payload"], &RELEASE, &mut own),
            "error: payload.damaged (sectionDigest): section \"runtime.zip\": SHA-256 mismatch\n",
        );

        // One whose runtime is of another version is refused as a runtime zip of it would be.
        let mut file = tiny_elf();
        let newer = RuntimeFiles::standard(platform.as_str(), "1.0.1").zip();
        append_sections(&mut file, &[(RUNTIME_SECTION, &newer)], 0);
        let newer_path = dir.path().join("newer");
        fs::write(&newer_path, &file).unwrap();
        let mut own = Sections::of_path(&newer_path);
        assert_refused(
            &run_as(&["--check-payload"], &RELEASE, &mut own),
            "error: payload.damaged (otherVersion): The runtime's version \"1.0.1\" is not the \
             launcher's version \"1.0.0-beta\"\n",
        );
    }

    #[test]
    fn features_are_listed() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let zip = standard_zip(dir.path(), "runtime.zip", platform.as_str(), VERSION);
        let cases: [(&[&str], &str); 3] = [
            (&[], "none"),
            (&["test-hooks"], "test-hooks"),
            (&["test-hooks", "other"], "test-hooks, other"),
        ];
        for (features, listed) in cases {
            let build = Build {
                version: VERSION,
                features,
            };
            let ran = check(&zip, &build);
            assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
            assert_eq!(ran.out, verified(platform, listed));
        }
    }

    #[test]
    fn inspect_prints_json_and_text() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();

        // An installation of this build's release.
        let runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        let fixture = InstallFixture::write(&dir.path().join("same"), runtime).unwrap();
        let shown = fixture.root.display().to_string();
        assert_eq!(
            inspected(&fixture.root, false),
            format!(
                "state: installed\npath: {shown}\n{}",
                installation_lines(VERSION, platform, "same", "writable", true)
            )
        );
        let report = inspected(&fixture.root, true);
        let parsed: Value = serde_json::from_str(&report).unwrap();
        assert_eq!(
            parsed,
            json!({
                "schema": 1,
                "path": shown,
                "state": "installed",
                "code": null,
                "detail": null,
                "alphaMarkers": [],
                "journals": [],
                "unknownNames": [],
                "installation": {
                    "version": VERSION,
                    "commit": COMMIT,
                    "platform": platform.as_str(),
                    "disc": GUNE5D,
                    "discRevision": DISC_REVISION,
                    "launcher": VERSION,
                    "relation": "same",
                    "access": "writable",
                },
            })
        );
        assert_eq!(keys_at(&report, 2), REPORT_KEYS);
        assert_eq!(keys_at(&report, 4), INSTALLATION_KEYS);
        assert!(report.ends_with("}\n"), "{report}");

        // An installation of an older release, which the build would update.
        let older = RuntimeFiles::standard(platform.as_str(), "0.9.0");
        let fixture = InstallFixture::write(&dir.path().join("older"), older).unwrap();
        assert_eq!(
            inspected(&fixture.root, false),
            format!(
                "state: installed\npath: {}\n{}",
                fixture.root.display(),
                installation_lines("0.9.0", platform, "newer", "writable", true)
            )
        );

        // An installation a later launcher made: still told of, but closed to this one, and with
        // the disc its receipt names unread.
        let later = RuntimeFiles::standard(platform.as_str(), "1.0.1");
        let fixture = InstallFixture::write(&dir.path().join("later"), later).unwrap();
        fixture
            .write_receipt(&json!({"schema": 2, "minLauncher": VERSION, "disc": [7]}))
            .unwrap();
        let shown = fixture.root.display().to_string();
        assert_eq!(
            inspected(&fixture.root, false),
            format!(
                "state: newerLauncher\npath: {shown}\ncode: state.newerLauncher\n{}",
                installation_lines("1.0.1", platform, "older", "readOnly", false)
            )
        );
        let parsed: Value = serde_json::from_str(&inspected(&fixture.root, true)).unwrap();
        assert_eq!(parsed["code"], "state.newerLauncher");
        assert_eq!(parsed["installation"]["disc"], Value::Null);
        assert_eq!(parsed["installation"]["discRevision"], Value::Null);
        assert_eq!(parsed["installation"]["access"], "readOnly");

        // A job left unfinished beside an installation, and names in its state folder that this
        // launcher's rules do not make.
        let runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        let fixture = InstallFixture::write(&dir.path().join("pending"), runtime).unwrap();
        let state = fixture.root.join(STATE_DIR);
        fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
        let journal = json!({"schema": 1, "minLauncher": VERSION, "phase": "staging"});
        fs::write(
            state.join(JOURNAL_DIR).join("install.json"),
            pretty_json(&journal),
        )
        .unwrap();
        fs::write(state.join("notes.txt"), b"").unwrap();
        fs::create_dir(state.join("later-state")).unwrap();
        assert_eq!(
            inspected(&fixture.root, false),
            format!(
                "state: pending\npath: {}\njournals: install.json\n\
                 unknownNames: later-state, notes.txt\n{}",
                fixture.root.display(),
                installation_lines(VERSION, platform, "same", "writable", true)
            )
        );
        let parsed: Value = serde_json::from_str(&inspected(&fixture.root, true)).unwrap();
        assert_eq!(parsed["journals"], json!(["install.json"]));
        assert_eq!(parsed["unknownNames"], json!(["later-state", "notes.txt"]));

        // An alpha installation's folder, its names in the order the launcher knows them by.
        let alpha = dir.path().join("alpha");
        fs::create_dir(&alpha).unwrap();
        make_alpha_markers(&alpha, &[".gdl-update", "installation.json"]).unwrap();
        assert_eq!(
            inspected(&alpha, false),
            format!(
                "state: alphaInstall\npath: {}\ncode: folder.alphaInstall\n\
                 alphaMarkers: installation.json, .gdl-update\n",
                alpha.display()
            )
        );
        let parsed: Value = serde_json::from_str(&inspected(&alpha, true)).unwrap();
        assert_eq!(parsed["state"], "alphaInstall");
        assert_eq!(parsed["code"], "folder.alphaInstall");
        assert_eq!(
            parsed["alphaMarkers"],
            json!(["installation.json", ".gdl-update"])
        );
        assert_eq!(parsed["installation"], Value::Null);

        // An empty folder, which has nothing more to tell.
        let empty = dir.path().join("empty");
        fs::create_dir(&empty).unwrap();
        assert_eq!(
            inspected(&empty, false),
            format!("state: empty\npath: {}\n", empty.display())
        );

        // A file, refused with the text the inspection gives the refusal, unchanged in both
        // reports.
        let file = dir.path().join("file.txt");
        fs::write(&file, b"not a folder").unwrap();
        let detail = format!("Install folder is not a directory: {}", file.display());
        assert_eq!(detail_of(&file).as_deref(), Some(detail.as_str()));
        assert_eq!(
            inspected(&file, false),
            format!(
                "state: notDirectory\npath: {}\ncode: folder.notDirectory\ndetail: {detail}\n",
                file.display()
            )
        );
        let parsed: Value = serde_json::from_str(&inspected(&file, true)).unwrap();
        assert_eq!(parsed["detail"], detail);

        // Given with a verbatim prefix, the folder is shown without it, while the detail keeps
        // the path as the refusal names it.
        #[cfg(windows)]
        {
            use std::path::{Component, Prefix};

            let on_a_drive = matches!(
                file.components().next(),
                Some(Component::Prefix(prefix)) if matches!(prefix.kind(), Prefix::Disk(_))
            );
            if on_a_drive {
                let verbatim = PathBuf::from(format!(r"\\?\{}", file.display()));
                let detail = detail_of(&verbatim).unwrap();
                assert!(detail.contains(r"\\?\"), "{detail}");
                assert_eq!(
                    inspected(&verbatim, false),
                    format!(
                        "state: notDirectory\npath: {}\ncode: folder.notDirectory\n\
                         detail: {detail}\n",
                        file.display()
                    )
                );
            } else {
                eprintln!("skipped the verbatim path: the test's folder is on no drive");
            }
        }

        // A folder whose name is not Unicode is looked for all the same.
        let strange = dir.path().join(not_unicode());
        assert_eq!(
            inspected(&strange, false),
            format!("state: absent\npath: {}\n", strange.display())
        );
    }

    #[test]
    #[should_panic(expected = "the build's version is a semantic version")]
    fn inspecting_needs_a_build_of_a_version() {
        let build = Build {
            version: "a build of no version",
            features: &[],
        };
        run_as(&["--inspect", "folder"], &build, &mut Ok(None));
    }

    #[test]
    fn a_report_that_cannot_be_written_fails_the_run() {
        let dir = tempfile::tempdir().unwrap();
        let mut own: OwnFile = Ok(None);
        let mut err = Vec::new();
        let asking_version = [OsString::from("--version")];
        assert_eq!(
            run(&asking_version, &RELEASE, &mut own, &mut Closed, &mut err),
            Some(EXIT_REFUSED)
        );
        let inspecting = [OsString::from("--inspect"), dir.path().into()];
        assert_eq!(
            run(&inspecting, &RELEASE, &mut own, &mut Closed, &mut err),
            Some(EXIT_REFUSED)
        );
        assert!(err.is_empty());
        // A refusal and a usage error exit as they do when their line is written.
        let checking = [OsString::from("--check-payload")];
        assert_eq!(
            run(&checking, &RELEASE, &mut own, &mut Vec::new(), &mut Closed),
            Some(EXIT_REFUSED)
        );
        let misused = [OsString::from("--version"), OsString::from("--json")];
        assert_eq!(
            run(&misused, &RELEASE, &mut own, &mut Vec::new(), &mut Closed),
            Some(EXIT_USAGE)
        );
        assert_eq!(usage(&mut Closed), EXIT_USAGE);
    }
}
