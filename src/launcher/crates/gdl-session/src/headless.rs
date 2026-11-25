//! The launcher's modes without a window, for the release scripts, CI and developers at a console:
//! `--version`, `--check-payload`, which checks the runtime the launcher file carries or a runtime
//! zip it is given, `--inspect`, which reports what a folder is to the launcher, `--check-folder`,
//! which reports what a folder is to an install, and `--extract`, which extracts a Legends
//! expansion's tree from a dump into a new folder outside every installation. They run before
//! anything builds a window, and what they print is technical text for whoever runs them, never
//! words for players. A run is handed its arguments, its build, its own file's sections and the
//! streams to write to, so that every mode runs inside a test as it runs in the program.

use std::ffi::{OsStr, OsString};
use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, Write};
use std::path::Path;

use gdl_extract::{Expansion, Trust};
use gdl_files::{Cancel, Job, NoProgress, RelPath, strip_verbatim};
use gdl_install::{
    Access, ComponentAttributes, DestinationReport, INSPECTION_SCHEMA, Inspection, InstallFolder,
    InstallationSummary, Payload, PayloadError, Relation, SPACE_MARGIN, Sections, SectionsError,
    judge_destination,
};
use gdl_release::{Platform, Version};
use serde::Serialize;

use crate::codes::{self, Report};
use crate::extraction::{
    Counts, ExtractRequest, ExtractSummary, StrictnessChoice, extract_to_new_folder, output_name,
};

/// The exit code of a mode that did what it was asked.
pub const EXIT_OK: u8 = 0;

/// The exit code of a mode that refused what it was given, or could not write its report.
pub const EXIT_REFUSED: u8 = 1;

/// The exit code of arguments that ask for a mode in a form it does not take, and of arguments
/// that ask for no mode at all.
pub const EXIT_USAGE: u8 = 2;

/// The line that answers arguments the program does not take.
const USAGE: &str = "usage: gdl-launcher-headless --check-payload [--payload <zip>] | \
                     --inspect <dir> [--json] | --check-folder <dir> [--json] | \
                     --extract <n64|dreamcast> <dump> --output <dir> [--strict script] | \
                     --version";

/// The mode that prints the program's version.
const VERSION_OPTION: &str = "--version";

/// The mode that checks a runtime: the launcher file's own, or the one `--payload` names.
const CHECK_PAYLOAD_OPTION: &str = "--check-payload";

/// The mode that reports what the folder after it is to the launcher.
const INSPECT_OPTION: &str = "--inspect";

/// The mode that reports what the folder after it is to an install into it.
const CHECK_FOLDER_OPTION: &str = "--check-folder";

/// The option of `--check-payload` that names a runtime zip to check in place of the launcher
/// file's own.
const PAYLOAD_OPTION: &str = "--payload";

/// The option of `--inspect` and `--check-folder` that makes them report in JSON.
const JSON_OPTION: &str = "--json";

/// The mode that extracts a Legends expansion's tree: the expansion, then the dump, follow it.
const EXTRACT_OPTION: &str = "--extract";

/// The option of `--extract` that names the new folder the tree goes into.
const OUTPUT_OPTION: &str = "--output";

/// The option of `--extract` that chooses how strictly the dump is held.
const STRICT_OPTION: &str = "--strict";

/// The one value `--strict` takes: the dump held as the extraction scripts hold one, rather than
/// as an installation does.
const SCRIPT_STRICTNESS: &str = "script";

/// The options that choose a mode: arguments with none of them are no headless run.
const MODE_OPTIONS: [&str; 5] = [
    VERSION_OPTION,
    CHECK_PAYLOAD_OPTION,
    INSPECT_OPTION,
    CHECK_FOLDER_OPTION,
    EXTRACT_OPTION,
];

/// How every option begins, and so how no option's value may.
const OPTION_PREFIX: &str = "--";

/// What `--check-payload` lists for a build that carries no optional feature.
const NO_FEATURES: &str = "none";

/// What joins the names of a list that a report gives on one line.
const LIST_SEPARATOR: &str = ", ";

/// The schema of the report `--check-folder` writes.
const CHECK_FOLDER_SCHEMA: u32 = 1;

/// How a text report writes a question that the judging answered yes.
const YES: &str = "yes";

/// How a text report writes a question that the judging answered no.
const NO: &str = "no";

/// How a text report writes what a judging that ended early did not ask.
const UNKNOWN: &str = "unknown";

/// How a text report writes an entry that the judging looked for and found nowhere, and a reparse
/// tag that an entry does not have.
const NOWHERE: &str = "none";

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
/// gives back its exit code; or `None` when no argument is `--check-payload`, `--inspect`,
/// `--check-folder`, `--extract` or `--version`, which is no headless run. A mode writes its
/// report to `out` and a refusal to `err`. Arguments that ask for a mode in a form it does not
/// take get the usage line on `err` and [`EXIT_USAGE`]. An option's value is the argument after
/// it, which must not be empty or begin with `--`; options may come in any order, each at most
/// once.
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
///   something, a control character in a value written as its escape, or with `--json` every
///   field as JSON. Whatever the folder holds, an inspection exits [`EXIT_OK`].
/// - `--check-folder <dir>` judges the folder as one to install into, as [`judge_destination`]
///   judges it for an install's plan: held to the names an install on this system makes at its
///   top, and to the room an install keeps free beyond what it writes. It reports what refuses an
///   install there and what to warn of, each with its code, rule and detail, and what the judging
///   asked of the folder and its volume, `unknown` for what a judging that ended early did not
///   ask: a `name: value` line each, a control character in a value written as its escape, or
///   with `--json` every field as JSON. It writes nothing but the entries the folder probe makes
///   and removes at once, a folder and a file in the folder when it is there, or else a folder in
///   the nearest folder above it that is, and the removal of those an earlier probe left in the
///   folder it probes; and none in a folder the launcher must not write into.
///   Whatever the folder holds, a judging exits [`EXIT_OK`]; one that fails is refused with the
///   line `error: <code> (<rule>): <detail>`, and [`EXIT_REFUSED`].
/// - `--extract <n64|dreamcast> <dump> --output <dir>` extracts that expansion's tree from the
///   dump into the new folder `<dir>`, as [`extract_to_new_folder`] does, holding the dump as an
///   installation holds one, to the trust the launcher carries, or with `--strict script` as the
///   extraction scripts hold one. It writes `Extracted <expansion> (<strictness>) into <dir>: `
///   and what the tree's manifests count, `<dir>` made absolute; a refusal is the line
///   `error: <code> (<rule>): <detail>`, and [`EXIT_REFUSED`]. A word that names no expansion,
///   and an output whose last name is none the launcher makes a folder with, are usage errors.
///
/// A refusal's detail is written as a text report writes a value, each control character as its
/// escape, so that the refusal stays one line whatever the names in its paths, or the text it
/// quotes from a dump, hold. A report that cannot be written to its stream gives
/// [`EXIT_REFUSED`], since nobody saw it.
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
        Mode::CheckFolder { folder, json } => check_folder(Path::new(folder), json, out, err),
        Mode::Extract {
            expansion,
            dump,
            output,
            script,
        } => extract_mode(expansion, dump, output, script, out, err),
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
    /// `--check-folder`, of `folder`, reported as JSON when `json` says so, else as text.
    CheckFolder { folder: &'a OsStr, json: bool },
    /// `--extract`, of the expansion the word `expansion` names, from the dump at `dump` into the
    /// new folder `output`, the dump held as the extraction scripts hold one when `script` says
    /// so, else as an installation holds one.
    Extract {
        expansion: &'a OsStr,
        dump: &'a OsStr,
        output: &'a OsStr,
        script: bool,
    },
}

/// The mode option the arguments give, before the other options are held to it.
enum Chosen<'a> {
    /// `--version`.
    Version,
    /// `--check-payload`.
    CheckPayload,
    /// `--inspect`, with the folder after it.
    Inspect(&'a OsStr),
    /// `--check-folder`, with the folder after it.
    CheckFolder(&'a OsStr),
    /// `--extract`, with the word that names the expansion and the dump after it.
    Extract(&'a OsStr, &'a OsStr),
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
    let mut output = None;
    let mut script = false;
    let mut rest = args.iter().map(OsString::as_os_str);
    while let Some(arg) = rest.next() {
        let mode = match arg.to_str() {
            Some(VERSION_OPTION) => Chosen::Version,
            Some(CHECK_PAYLOAD_OPTION) => Chosen::CheckPayload,
            Some(INSPECT_OPTION) => Chosen::Inspect(value(&mut rest)?),
            Some(CHECK_FOLDER_OPTION) => Chosen::CheckFolder(value(&mut rest)?),
            Some(EXTRACT_OPTION) => Chosen::Extract(value(&mut rest)?, value(&mut rest)?),
            Some(PAYLOAD_OPTION) if zip.is_none() => {
                zip = Some(value(&mut rest)?);
                continue;
            }
            Some(JSON_OPTION) if !json => {
                json = true;
                continue;
            }
            Some(OUTPUT_OPTION) if output.is_none() => {
                output = Some(value(&mut rest)?);
                continue;
            }
            Some(STRICT_OPTION) if !script => {
                if value(&mut rest)? != SCRIPT_STRICTNESS {
                    return Err(Misused);
                }
                script = true;
                continue;
            }
            _ => return Err(Misused),
        };
        if chosen.replace(mode).is_some() {
            return Err(Misused);
        }
    }
    let extracting = (output, script);
    match (chosen, zip, json, extracting) {
        (Some(Chosen::Version), None, false, (None, false)) => Ok(Mode::Version),
        (Some(Chosen::CheckPayload), zip, false, (None, false)) => Ok(Mode::CheckPayload { zip }),
        (Some(Chosen::Inspect(folder)), None, json, (None, false)) => {
            Ok(Mode::Inspect { folder, json })
        }
        (Some(Chosen::CheckFolder(folder)), None, json, (None, false)) => {
            Ok(Mode::CheckFolder { folder, json })
        }
        (Some(Chosen::Extract(expansion, dump)), None, false, (Some(output), script)) => {
            Ok(Mode::Extract {
                expansion,
                dump,
                output,
                script,
            })
        }
        _ => Err(Misused),
    }
}

/// The value of the option just read: the next argument, which must be there, must not be empty,
/// since an empty argument names no folder or zip, and must not be an option itself.
fn value<'a>(rest: &mut impl Iterator<Item = &'a OsStr>) -> Result<&'a OsStr, Misused> {
    rest.next()
        .filter(|value| {
            !value.is_empty()
                && !value
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
/// and its `detail`, written as [`text_value`] writes a value, since a path in it may name a folder
/// or a file whose name holds a line end or a terminal's escape; or, for a job asked to stop, only
/// that it was cancelled. Gives back [`EXIT_REFUSED`], written or not.
fn refuse(err: &mut dyn Write, report: Report, rule: &str, detail: &dyn fmt::Display) -> u8 {
    let line = match report {
        Report::Error(code) => {
            let detail = text_value(&detail.to_string());
            format!("error: {code} ({rule}): {detail}\n")
        }
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
        json_report(&view)
    } else {
        view.text()
    };
    reported(out, &report)
}

/// `--check-folder`: judges the folder at `folder` as one to install into, holding it to the names
/// an install on this system makes at its top and to the room an install keeps free, all that it
/// asks of a volume since it installs nothing, and reports what the judging found to `out`, as
/// JSON when `json` says so, or why it failed to `err`.
fn check_folder(folder: &Path, json: bool, out: &mut dyn Write, err: &mut dyn Write) -> u8 {
    let report = match judge_destination(folder, &install_names(), SPACE_MARGIN) {
        Ok(report) => report,
        Err(error) => {
            let code = Report::Error(codes::destination(&error));
            return refuse(err, code, error.rule(), &error);
        }
    };
    let view = FolderView::of(&report);
    let report = if json {
        json_report(&view)
    } else {
        view.text()
    };
    reported(out, &report)
}

/// The names an install on this system makes at a folder's top, as the install crate gives them
/// for this build's platform.
fn install_names() -> Vec<RelPath> {
    gdl_install::install_names(Platform::of_this_build())
}

/// `view` as a JSON report writes it: pretty, and ending in a line end.
fn json_report(view: &impl Serialize) -> String {
    let mut text = serde_json::to_string_pretty(view).expect("a report's fields are all JSON");
    text.push('\n');
    text
}

/// `--extract`: extracts the expansion `word` names from the dump at `dump` into the new folder
/// `output`, holding the dump as the extraction scripts hold one when `script` says so, else as an
/// installation holds one, to the trust the launcher carries; and reports what it made to `out`,
/// or why it refused to `err`. A word that names no expansion, and an output whose last name is
/// none the launcher makes a folder with, get the usage line, before anything is looked at.
fn extract_mode(
    word: &OsStr,
    dump: &OsStr,
    output: &OsStr,
    script: bool,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> u8 {
    let Some(expansion) = word.to_str().and_then(Expansion::parse) else {
        return usage(err);
    };
    let output = Path::new(output);
    if output_name(output).is_err() {
        return usage(err);
    }
    let request = ExtractRequest {
        expansion,
        dump: Path::new(dump),
        output,
        strictness: if script {
            StrictnessChoice::Script
        } else {
            StrictnessChoice::Install
        },
    };
    // Nothing here asks the extraction to stop, and nobody follows its progress.
    let cancel = Cancel::default();
    let job = Job {
        cancel: &cancel,
        progress: &NoProgress,
    };
    match extract_to_new_folder(&request, &Trust::shipped(), &job) {
        Ok(summary) => reported(out, &extracted(&summary)),
        Err(error) => refuse(err, codes::extraction(&error), error.rule(), &error),
    }
}

/// The line that reports `summary`: the expansion, how strictly its dump was held, the folder its
/// tree went into, and what the tree's manifests count.
fn extracted(summary: &ExtractSummary) -> String {
    let counts = match summary.counts {
        Counts::N64 {
            files,
            programs,
            maps,
        } => format!("{files} files, {programs} programs, {maps} maps"),
        Counts::Dreamcast {
            disc_files,
            native_files,
            external,
        } => format!(
            "{disc_files} disc files, {native_files} native files, {external} external records"
        ),
    };
    format!(
        "Extracted {} ({}) into {}: {counts}\n",
        summary.expansion.as_str(),
        summary.strictness,
        summary.output.display()
    )
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
    /// Whether an update stands with its cleanup still to be finished, which the next start tries
    /// again: an installation all the same, not a job to recover.
    cleanup_pending: bool,
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
            cleanup_pending: inspection.cleanup_pending,
            unknown_names: &inspection.unknown_names,
            installation: inspection.installation.as_ref().map(InstallationView::of),
        }
    }

    /// The report as text: a `name: value` line for each field, in the report's order, leaving
    /// out the schema and every field that holds nothing, with a list's names on one line, and
    /// whether a cleanup is pending always told, `yes` or `no`. A value is written as
    /// [`text_value`] writes it, since the folder's own names and records may hold any character.
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
        if !self.journals.is_empty() {
            lines.push(("journals", self.journals.join(LIST_SEPARATOR)));
        }
        lines.push(("cleanupPending", answer(Some(self.cleanup_pending))));
        if !self.unknown_names.is_empty() {
            lines.push(("unknownNames", self.unknown_names.join(LIST_SEPARATOR)));
        }
        if let Some(installation) = &self.installation {
            installation.lines(&mut lines);
        }
        text_report(lines)
    }
}

/// A text report of `lines`, each name and value pair a `name: value` line, the value written as
/// [`text_value`] writes it, since a folder's names and records may hold any character.
fn text_report(lines: Vec<(&str, String)>) -> String {
    lines
        .into_iter()
        .map(|(name, value)| format!("{name}: {}\n", text_value(&value)))
        .collect()
}

/// `value` as a text report writes it: each control character, such as a line end, which would
/// start a line of its own, or the escape that begins a terminal's command, as Rust writes it in
/// a literal, `\n` or `\u{1b}`, and every other character as it is.
fn text_value(value: &str) -> String {
    let mut text = String::with_capacity(value.len());
    for character in value.chars() {
        if character.is_control() {
            text.extend(character.escape_debug());
        } else {
            text.push(character);
        }
    }
    text
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

/// What `--check-folder` reports of a folder, field by field in the order of the report, each
/// under its camelCase name in JSON and in text alike, where a list gives a line to each of its
/// entries under its name in the singular. What the judging did not ask, because it ended before,
/// is `None`.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct FolderView {
    /// The report's schema.
    schema: u32,
    /// The folder, absolute and without a verbatim prefix.
    path: String,
    /// Whether the folder is there, a file or a link included.
    exists: Option<bool>,
    /// The folder when it is there, else the nearest entry above it that is, shown as `path` is;
    /// `None` as well when nothing on its path is there.
    nearest_existing: Option<String>,
    /// The bytes this process may still write to that folder's volume.
    available_bytes: Option<u64>,
    /// The folder's length in UTF-16 units, as Windows' path limits count it.
    root_utf16: Option<usize>,
    /// Whether the folder probe could make its entries there.
    writable: Option<bool>,
    /// Whether the folder probe found its entries by their names in upper case.
    ignores_case: Option<bool>,
    /// Whether the folder's file system runs no program.
    noexec: Option<bool>,
    /// What refuses an install there, in the order the judging found them.
    blockers: Vec<FindingView>,
    /// What to warn of, on Windows.
    warnings: Vec<FindingView>,
    /// What Windows shows of each entry on the path that is there, the nearest first.
    components: Vec<ComponentView>,
    /// The variables that name a OneDrive folder, as they are set, on Windows.
    one_drive_variables: Vec<VariableView>,
    /// The folders that the OneDrive accounts signed in keep in sync, on Windows.
    cloud_roots: Vec<String>,
}

/// What `--check-folder` reports of one finding: what refuses an install, or what to warn of.
#[derive(Serialize)]
struct FindingView {
    /// The code the page turns into words.
    code: &'static str,
    /// The finding's stable machine name.
    rule: &'static str,
    /// The finding's technical text.
    detail: String,
}

/// What `--check-folder` reports of an entry on the folder's path that is there.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ComponentView {
    /// The entry, as shown as the report's `path`.
    path: String,
    /// Its attributes, as the system gives them.
    attributes: u32,
    /// Its reparse tag, when it is a reparse point.
    reparse_tag: Option<u32>,
}

/// What `--check-folder` reports of a variable that names a OneDrive folder.
#[derive(Serialize)]
struct VariableView {
    /// The variable.
    name: String,
    /// The folder it names, as it is set.
    path: String,
}

impl FolderView {
    /// The report of `report`.
    fn of(report: &DestinationReport) -> FolderView {
        FolderView {
            schema: CHECK_FOLDER_SCHEMA,
            path: shown(&report.path),
            exists: report.exists,
            nearest_existing: report.nearest_existing.as_deref().map(shown),
            available_bytes: report.available_bytes,
            root_utf16: report.root_utf16,
            writable: report.probe.map(|probe| probe.writable),
            ignores_case: report.probe.map(|probe| probe.ignores_case),
            noexec: report.noexec,
            blockers: report
                .blockers
                .iter()
                .map(|blocker| FindingView {
                    code: codes::blocker(blocker),
                    rule: blocker.rule(),
                    detail: blocker.to_string(),
                })
                .collect(),
            warnings: report
                .warnings
                .iter()
                .map(|warning| FindingView {
                    code: codes::warning(warning),
                    rule: warning.rule(),
                    detail: warning.to_string(),
                })
                .collect(),
            components: report.components.iter().map(ComponentView::of).collect(),
            one_drive_variables: report
                .onedrive_variables
                .iter()
                .map(|(name, path)| VariableView {
                    name: name.clone(),
                    path: path.display().to_string(),
                })
                .collect(),
            cloud_roots: report
                .cloud_roots
                .iter()
                .map(|root| root.display().to_string())
                .collect(),
        }
    }

    /// The report as text: a `name: value` line for each field, in the report's order, leaving
    /// out the schema, with a line of its own for each entry of a list. A value the judging did
    /// not ask is `unknown`, and so is the nearest entry when the judging ended before it looked;
    /// one it looked for and found nowhere is `none`.
    fn text(&self) -> String {
        let known = |value: Option<String>| value.unwrap_or_else(|| UNKNOWN.to_owned());
        let nearest = match (&self.nearest_existing, self.exists) {
            (Some(path), _) => path.clone(),
            (None, Some(_)) => NOWHERE.to_owned(),
            (None, None) => UNKNOWN.to_owned(),
        };
        let mut lines = vec![
            ("path", self.path.clone()),
            ("exists", answer(self.exists)),
            ("nearestExisting", nearest),
            (
                "availableBytes",
                known(self.available_bytes.map(|bytes| bytes.to_string())),
            ),
            (
                "rootUtf16",
                known(self.root_utf16.map(|units| units.to_string())),
            ),
            ("writable", answer(self.writable)),
            ("ignoresCase", answer(self.ignores_case)),
            ("noexec", answer(self.noexec)),
        ];
        lines.extend(
            self.blockers
                .iter()
                .map(|blocker| ("blocker", blocker.line())),
        );
        lines.extend(
            self.warnings
                .iter()
                .map(|warning| ("warning", warning.line())),
        );
        lines.extend(
            self.components
                .iter()
                .map(|component| ("component", component.line())),
        );
        lines.extend(self.one_drive_variables.iter().map(|variable| {
            let line = format!("{} {}", variable.name, variable.path);
            ("oneDriveVariable", line)
        }));
        lines.extend(
            self.cloud_roots
                .iter()
                .map(|root| ("cloudRoot", root.clone())),
        );
        text_report(lines)
    }
}

impl FindingView {
    /// The finding as a text report's line gives it: `<code> (<rule>): <detail>`.
    fn line(&self) -> String {
        format!("{} ({}): {}", self.code, self.rule, self.detail)
    }
}

impl ComponentView {
    /// The report of `component`.
    fn of(component: &ComponentAttributes) -> ComponentView {
        ComponentView {
            path: shown(&component.path),
            attributes: component.attributes,
            reparse_tag: component.reparse_tag,
        }
    }

    /// The entry as a text report's line gives it: its path, then its attributes and its reparse
    /// tag, or `none`, each as eight hexadecimal digits.
    fn line(&self) -> String {
        let tag = self
            .reparse_tag
            .map_or_else(|| NOWHERE.to_owned(), |tag| format!("0x{tag:08X}"));
        format!(
            "{} attributes 0x{:08X} tag {tag}",
            self.path, self.attributes
        )
    }
}

/// How a text report writes the answer to a question: `yes`, `no`, or `unknown` when the judging
/// did not ask it.
fn answer(value: Option<bool>) -> String {
    match value {
        Some(true) => YES,
        Some(false) => NO,
        None => UNKNOWN,
    }
    .to_owned()
}

/// `path` as a report shows it: without a verbatim prefix, and with a name that is not Unicode
/// shown as near as it can be.
fn shown(path: &Path) -> String {
    strip_verbatim(path).display().to_string()
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::ffi::{OsStr, OsString};
    use std::fs;
    use std::io::{self, Write};
    use std::path::{Path, PathBuf};

    use gdl_extract::Expansion;
    use gdl_files::{FolderProbe, root_utf16_len};
    use gdl_fixtures::{
        DiscSet, Edc, InstallFixture, RawEntry, RuntimeFiles, append_sections, backdate,
        make_alpha_markers, pretty_json, snapshot, tiny_elf,
    };
    use gdl_install::{
        Blocker, ComponentAttributes, DEFAULT_CONFIG, DestinationReport, GUNE5D, InstallFolder,
        JOURNAL_DIR, KnownFolder, LOCK_FILE, RETIRED_LAUNCHER_PREFIX, RUNTIME_SECTION, STATE_DIR,
        Sections, SectionsError, UPDATE_JOURNAL, UPDATE_STAGING_PREFIX, Warning,
        installed_launcher_name,
    };
    use gdl_release::{Platform, Version};
    use serde_json::{Value, json};

    use super::{
        Build, EXIT_OK, EXIT_REFUSED, EXIT_USAGE, FolderView, OwnFile, SPACE_MARGIN, USAGE,
        install_names, json_report, refuse, run, text_value, usage,
    };
    use crate::codes::Report;
    use crate::extraction::vectors::{
        Folders, default_rom, default_tree, full_tree, not_unicode, tree_listing, with_bad_stream,
    };

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

    /// The version of the release after the build's, which a later launcher updated an
    /// installation to.
    const NEXT: &str = "1.0.0-beta.1";

    /// The random part of the staging the tests' update journals name.
    const STAGING_RANDOM: &str = "AbC123xy";

    /// The keys of an inspection's JSON report, in the order it writes them.
    const REPORT_KEYS: [&str; 10] = [
        "schema",
        "path",
        "state",
        "code",
        "detail",
        "alphaMarkers",
        "journals",
        "cleanupPending",
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
                "--payload followed by its own mode",
            ),
            (
                &["--check-payload", "--payload", "--json"],
                "--payload followed by an option",
            ),
            (
                &["--payload", "--version", "--check-payload"],
                "--payload followed by another mode",
            ),
            (
                &["--check-payload", "--payload", ""],
                "--payload with an empty value",
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
            (&["--inspect", ""], "--inspect with an empty folder"),
            (
                &["--inspect", "", "--json"],
                "--inspect with an empty folder",
            ),
            (&["--inspect", "--json"], "--inspect followed by an option"),
            (&["--json", "--inspect"], "--inspect at the end"),
            (&["--extract"], "--extract without its values"),
            (&["--extract", "n64"], "--extract with one value"),
            (
                &["--extract", "n64", "--output", "out"],
                "--extract followed by an option",
            ),
            (
                &["--extract", "n64", "rom.z64"],
                "--extract without --output",
            ),
            (
                &["--extract", "gamecube", "game.iso", "--output", "out"],
                "an expansion the launcher does not install",
            ),
            (
                &["--extract", "N64", "rom.z64", "--output", "out"],
                "an expansion's name in another letter case",
            ),
            (
                &["--extract", "n64", "rom.z64", "--output"],
                "--output without a value",
            ),
            (
                &["--extract", "n64", "rom.z64", "--output", ""],
                "--output with an empty value",
            ),
            (
                &[
                    "--extract",
                    "n64",
                    "rom.z64",
                    "--output",
                    "a",
                    "--output",
                    "b",
                ],
                "--output twice",
            ),
            (
                &["--extract", "n64", "rom.z64", "--output", "out", "--strict"],
                "--strict without a value",
            ),
            (
                &[
                    "--extract",
                    "n64",
                    "rom.z64",
                    "--output",
                    "out",
                    "--strict",
                    "install",
                ],
                "--strict install, which is what --extract does without it",
            ),
            (
                &[
                    "--extract",
                    "n64",
                    "rom.z64",
                    "--output",
                    "out",
                    "--strict",
                    "script",
                    "--strict",
                    "script",
                ],
                "--strict twice",
            ),
            (
                &["--version", "--output", "out"],
                "--output beside --version",
            ),
            (
                &["--inspect", "folder", "--strict", "script"],
                "--strict beside --inspect",
            ),
            (
                &[
                    "--extract",
                    "n64",
                    "rom.z64",
                    "--output",
                    "out",
                    "--payload",
                    "a.zip",
                ],
                "--payload beside --extract",
            ),
            (
                &["--extract", "n64", "rom.z64", "--output", "out", "--json"],
                "--json beside --extract",
            ),
            (
                &[
                    "--extract",
                    "n64",
                    "rom.z64",
                    "--extract",
                    "dreamcast",
                    "disc.cue",
                    "--output",
                    "out",
                ],
                "two extractions",
            ),
            (
                &["--extract", "n64", "rom.z64", "--output", "x."],
                "an output ending in a dot",
            ),
            (
                &["--extract", "n64", "rom.z64", "--output", "x "],
                "an output ending in a space",
            ),
            (
                &[
                    "--extract",
                    "dreamcast",
                    "disc.cue",
                    "--output",
                    ".gdl-launcher",
                ],
                "an output named as the launcher's state folder",
            ),
            (
                &["--extract", "n64", "rom.z64", "--output", "out/.."],
                "an output that ends in no name",
            ),
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
             [--json] | --check-folder <dir> [--json] | --extract <n64|dreamcast> <dump> \
             --output <dir> [--strict script] | --version"
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
            &["--check-folder=folder"],
            &["--Check-Folder", "folder"],
            &["--output", "out"],
            &["--strict", "script"],
            &["--extract=n64"],
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
                "state: installed\npath: {shown}\ncleanupPending: no\n{}",
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
                "cleanupPending": false,
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
                "state: installed\npath: {}\ncleanupPending: no\n{}",
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
                "state: newerLauncher\npath: {shown}\ncode: state.newerLauncher\n\
                 cleanupPending: no\n{}",
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
                "state: pending\npath: {}\njournals: install.json\ncleanupPending: no\n\
                 unknownNames: later-state, notes.txt\n{}",
                fixture.root.display(),
                installation_lines(VERSION, platform, "same", "writable", true)
            )
        );
        let parsed: Value = serde_json::from_str(&inspected(&fixture.root, true)).unwrap();
        assert_eq!(parsed["journals"], json!(["install.json"]));
        assert_eq!(parsed["cleanupPending"], false);
        assert_eq!(parsed["unknownNames"], json!(["later-state", "notes.txt"]));

        // An update that stands with its cleanup left, its journal committed as a cleanup that
        // stopped leaves it: an installation, with the journal told of, not a job to recover.
        let runtime = RuntimeFiles::standard(platform.as_str(), NEXT);
        let fixture = InstallFixture::write(&dir.path().join("updated"), runtime).unwrap();
        let suffix = match platform {
            Platform::WindowsX64 => ".exe",
            Platform::LinuxX64 => "",
        };
        let journal = json!({
            "schema": 1,
            "minLauncher": VERSION,
            "phase": "committed",
            "from": VERSION,
            "to": NEXT,
            "staging": format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}"),
            "backup": ["VERSION", "build-info.json"],
            "publish": ["VERSION", "build-info.json"],
            "madeFolders": [],
            "launcher": installed_launcher_name(platform),
            "retired": format!("{RETIRED_LAUNCHER_PREFIX}{VERSION}-{STAGING_RANDOM}{suffix}"),
            "receipt": fixture.receipt(),
        });
        let journals = fixture.root.join(STATE_DIR).join(JOURNAL_DIR);
        fs::create_dir(&journals).unwrap();
        fs::write(journals.join(UPDATE_JOURNAL), pretty_json(&journal)).unwrap();
        assert_eq!(
            inspected(&fixture.root, false),
            format!(
                "state: installed\npath: {}\njournals: {UPDATE_JOURNAL}\ncleanupPending: yes\n{}",
                fixture.root.display(),
                installation_lines(NEXT, platform, "older", "writable", true)
            )
        );
        let parsed: Value = serde_json::from_str(&inspected(&fixture.root, true)).unwrap();
        assert_eq!(parsed["state"], "installed");
        assert_eq!(parsed["code"], Value::Null);
        assert_eq!(parsed["journals"], json!([UPDATE_JOURNAL]));
        assert_eq!(parsed["cleanupPending"], true);

        // An alpha installation's folder, its names in the order the launcher knows them by.
        let alpha = dir.path().join("alpha");
        fs::create_dir(&alpha).unwrap();
        make_alpha_markers(&alpha, &[".gdl-update", "installation.json"]).unwrap();
        assert_eq!(
            inspected(&alpha, false),
            format!(
                "state: alphaInstall\npath: {}\ncode: folder.alphaInstall\n\
                 alphaMarkers: installation.json, .gdl-update\ncleanupPending: no\n",
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
            format!(
                "state: empty\npath: {}\ncleanupPending: no\n",
                empty.display()
            )
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
                "state: notDirectory\npath: {}\ncode: folder.notDirectory\ndetail: {detail}\n\
                 cleanupPending: no\n",
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
                         detail: {detail}\ncleanupPending: no\n",
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
            format!(
                "state: absent\npath: {}\ncleanupPending: no\n",
                strange.display()
            )
        );
    }

    #[test]
    fn a_folder_cannot_forge_lines_of_the_text_report() {
        let Some(platform) = this_platform() else {
            return;
        };
        // A commit holding a line end with a field of its own after it, then the commands that
        // clear a terminal and colour its text; and how the text report shows it.
        const FORGED: &str = "abc\nstate: empty\u{1b}[2J\u{9b}31m\r";
        const SHOWN: &str = r"abc\nstate: empty\u{1b}[2J\u{9b}31m\r";
        // What a report shows of an installation, with the fields of `before` ahead of whether a
        // cleanup is pending and those of `after` behind it.
        let fields = |before: &[&'static str], after: &[&'static str]| {
            let mut fields = vec!["state", "path"];
            fields.extend_from_slice(before);
            fields.push("cleanupPending");
            fields.extend_from_slice(after);
            fields.extend_from_slice(&INSTALLATION_KEYS);
            fields
        };
        // The fields of a text report, in its order: no line but a field's own, and no control
        // character but the line ends.
        let fields_of = |text: &str| -> Vec<String> {
            assert!(
                !text.chars().any(|c| c.is_control() && c != '\n'),
                "{text:?}"
            );
            text.lines()
                .map(|line| {
                    line.split_once(": ")
                        .map_or(line, |(name, _)| name)
                        .to_owned()
                })
                .collect()
        };

        // A release record and a receipt that both name it, so the folder reads as installed.
        let dir = tempfile::tempdir().unwrap();
        let mut runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        runtime.commit = FORGED.to_owned();
        let fixture = InstallFixture::write(&dir.path().join("forged"), runtime).unwrap();
        let text = inspected(&fixture.root, false);
        assert_eq!(fields_of(&text), fields(&[], &[]), "{text}");
        assert!(text.contains(&format!("\ncommit: {SHOWN}\n")), "{text}");
        // The JSON report escapes as JSON does, and gives the commit back exactly.
        let parsed: Value = serde_json::from_str(&inspected(&fixture.root, true)).unwrap();
        assert_eq!(parsed["installation"]["commit"], FORGED);

        // Unix lets a name hold any character but the separator: in the state folder, and among
        // the journals, where a name of no kind this launcher knows is listed.
        #[cfg(unix)]
        {
            const NOTES: &str = "notes\nstate: empty";
            const JOURNAL: &str = "x\u{1b}]0;title\u{7}";
            let state = fixture.root.join(STATE_DIR);
            fs::write(state.join(NOTES), b"").unwrap();
            fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
            fs::write(state.join(JOURNAL_DIR).join(JOURNAL), b"").unwrap();
            let text = inspected(&fixture.root, false);
            let listed = fields(&["code", "journals"], &["unknownNames"]);
            assert_eq!(fields_of(&text), listed, "{text}");
            assert!(text.contains(r"journals: x\u{1b}]0;title\u{7}"), "{text}");
            assert!(
                text.contains(r"unknownNames: notes\nstate: empty"),
                "{text}"
            );
            let parsed: Value = serde_json::from_str(&inspected(&fixture.root, true)).unwrap();
            assert_eq!(parsed["journals"], json!([JOURNAL]));
            assert_eq!(parsed["unknownNames"], json!([NOTES]));
        }
    }

    #[test]
    fn text_values_escape_control_characters_alone() {
        // A Windows path keeps its backslashes, and letters beyond ASCII stay as they are.
        for plain in [r"C:\Games\Gauntlet Dark Legacy", "caf\u{e9} \u{1d11e}", ""] {
            assert_eq!(text_value(plain), plain);
        }
        assert_eq!(text_value("a\tb\0c\u{7f}d\u{85}"), r"a\tb\0c\u{7f}d\u{85}");
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
        // A folder judged whole, in text and in JSON, whose report is lost.
        let checking = [OsString::from("--check-folder"), dir.path().into()];
        assert_eq!(
            run(&checking, &RELEASE, &mut own, &mut Closed, &mut err),
            Some(EXIT_REFUSED)
        );
        let checking_json = [
            OsString::from("--check-folder"),
            dir.path().into(),
            OsString::from("--json"),
        ];
        assert_eq!(
            run(&checking_json, &RELEASE, &mut own, &mut Closed, &mut err),
            Some(EXIT_REFUSED)
        );
        // A runtime that verifies and loses its report, as the release scripts' check could. No
        // refusal reaches the error stream, so the runtime did verify.
        if let Some(platform) = this_platform() {
            let zip = standard_zip(dir.path(), "runtime.zip", platform.as_str(), VERSION);
            let verifying = [
                OsString::from("--check-payload"),
                OsString::from("--payload"),
                zip.into_os_string(),
            ];
            assert_eq!(
                run(&verifying, &RELEASE, &mut own, &mut Closed, &mut err),
                Some(EXIT_REFUSED)
            );
        }
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
        // An extraction that loses its report made its tree all the same, and fails the run.
        let folders = Folders::new();
        let (name, image) = default_rom();
        let dump = folders.dump(&name, &image);
        let output = folders.output("n64");
        let extracting = extract_args("n64", &dump, &output, true);
        assert_eq!(
            run(&extracting, &RELEASE, &mut own, &mut Closed, &mut err),
            Some(EXIT_REFUSED)
        );
        assert!(err.is_empty());
        assert!(output.is_dir());
    }

    /// The arguments that extract the expansion `expansion` names from the dump at `dump` into
    /// `output`, held as the extraction scripts hold a dump when `script` says so.
    fn extract_args(expansion: &str, dump: &Path, output: &Path, script: bool) -> Vec<OsString> {
        let mut args: Vec<OsString> = vec![
            "--extract".into(),
            expansion.into(),
            dump.into(),
            "--output".into(),
            output.into(),
        ];
        if script {
            args.extend(["--strict".into(), "script".into()]);
        }
        args
    }

    #[test]
    fn extract_writes_a_new_folder_and_nothing_else() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let dump = folders.dump(&name, &image);
        let output = folders.output("n64");
        let before = folders.before(Expansion::N64);
        let ran = run_release(&extract_args("n64", &dump, &output, true));
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.err, "");
        // The tree the N64 extractor writes, in a folder of its own; the folder it was made in is
        // as it was outside it, and the dump as it was.
        folders.assert_published(&before, "n64", &default_tree());
    }

    #[test]
    fn extract_reports_its_counts() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &image);
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));

        // The default image's two files, one of them a map's world, and no overlays, since its
        // digest is no cartridge's whose boot table the launcher knows.
        let output = folders.output("n64");
        let ran = run_release(&extract_args("n64", &rom, &output, true));
        assert_eq!((ran.code, ran.err.as_str()), (Some(EXIT_OK), ""), "{ran:?}");
        assert_eq!(
            ran.out,
            format!(
                "Extracted n64 (script) into {}: 2 files, 0 programs, 1 maps\n",
                output.display()
            )
        );
        assert_eq!(tree_listing(&output), default_tree());

        // The full set's four disc files, and the two files SIZES.ROM places in DISK.ROM and the
        // one it lists outside it.
        let output = folders.output("dreamcast");
        let ran = run_release(&extract_args("dreamcast", &sheet, &output, true));
        assert_eq!((ran.code, ran.err.as_str()), (Some(EXIT_OK), ""), "{ran:?}");
        assert_eq!(
            ran.out,
            format!(
                "Extracted dreamcast (script) into {}: 4 disc files, 2 native files, 1 external \
                 records\n",
                output.display()
            )
        );
        assert_eq!(tree_listing(&output), full_tree());
    }

    #[test]
    fn install_is_the_default_and_refuses_a_synthetic_rom() {
        // The default image names no cartridge of the game, and its digest is not the one the
        // launcher's trust holds.
        let folders = Folders::new();
        let (name, image) = default_rom();
        let dump = folders.dump(&name, &image);
        let output = folders.output("n64");
        let before = folders.before(Expansion::N64);
        assert_refused(
            &run_release(&extract_args("n64", &dump, &output, false)),
            "error: source.otherGame (otherGame): the dump is refused for an installation: \
             otherGame\n",
        );
        folders.assert_untouched(&before);
    }

    #[test]
    fn a_refusal_is_one_line_whatever_its_detail_holds() {
        // A detail holding a line end with a refusal of its own after it, then the commands that
        // clear a terminal and colour its text; and how the refusal shows it.
        const FORGED: &str = "a\nerror: forged (rule): b\u{1b}[2J\u{9b}31m\r";
        const SHOWN: &str = r"a\nerror: forged (rule): b\u{1b}[2J\u{9b}31m\r";
        let mut err = Vec::new();
        let code = refuse(&mut err, Report::Error("io.failed"), "io", &FORGED);
        assert_eq!(code, EXIT_REFUSED);
        assert_eq!(
            String::from_utf8(err).unwrap(),
            format!("error: io.failed (io): {SHOWN}\n")
        );

        // An extraction into a folder whose path holds them, refused before the dump, which is not
        // there, is looked for: the path is in the detail, and shown so.
        let folders = Folders::new();
        let dump = folders.root.join("missing.z64");
        let output = folders.output("x\u{1b}[2J\nerror: forged").join("n64");
        let ran = run_release(&extract_args("n64", &dump, &output, true));
        assert_eq!(
            (ran.code, ran.out.as_str()),
            (Some(EXIT_REFUSED), ""),
            "{ran:?}"
        );
        let line = ran.err.strip_suffix('\n').expect("a line");
        assert!(!line.chars().any(char::is_control), "{line:?}");
        assert!(line.contains(r"x\u{1b}[2J\nerror: forged"), "{line:?}");
    }

    #[test]
    fn a_refused_extraction_leaves_its_folder_as_it_was() {
        // The N64 extractor's corrupt stream, and a disc without its native table: both refused
        // as the dump is checked, before anything is made.
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &with_bad_stream(&image));
        let sheet = folders.disc(&DiscSet::small(Edc::Zero));
        for (expansion, word, dump, rule) in [
            (Expansion::N64, "n64", &rom, "deflate"),
            (
                Expansion::Dreamcast,
                "dreamcast",
                &sheet,
                "nativeTableMissing",
            ),
        ] {
            let before = folders.before(expansion);
            let ran = run_release(&extract_args(word, dump, &folders.output(word), true));
            assert_eq!(ran.code, Some(EXIT_REFUSED), "{ran:?}");
            let prefix = format!("error: source.damaged ({rule}): ");
            assert!(ran.err.starts_with(&prefix), "{}", ran.err);
            assert_eq!(ran.err.lines().count(), 1, "{}", ran.err);
            assert_eq!(ran.out, "");
            folders.assert_untouched(&before);
        }
    }

    /// The keys of a folder check's JSON report, in the order it writes them.
    const FOLDER_KEYS: [&str; 14] = [
        "schema",
        "path",
        "exists",
        "nearestExisting",
        "availableBytes",
        "rootUtf16",
        "writable",
        "ignoresCase",
        "noexec",
        "blockers",
        "warnings",
        "components",
        "oneDriveVariables",
        "cloudRoots",
    ];

    /// The keys of a finding in a folder check's JSON report, in the order it writes them.
    const FINDING_KEYS: [&str; 3] = ["code", "rule", "detail"];

    /// The keys of an entry on the path in a folder check's JSON report, in the order it writes
    /// them.
    const COMPONENT_KEYS: [&str; 3] = ["path", "attributes", "reparseTag"];

    /// The keys of a OneDrive variable in a folder check's JSON report, in the order it writes
    /// them.
    const VARIABLE_KEYS: [&str; 2] = ["name", "path"];

    /// The fields of a folder check's text report that every judging writes, in its order.
    const FOLDER_FIELDS: [&str; 8] = [
        "path",
        "exists",
        "nearestExisting",
        "availableBytes",
        "rootUtf16",
        "writable",
        "ignoresCase",
        "noexec",
    ];

    /// The fields a folder check's text report repeats for a list's entries, which on Windows
    /// alone tell what the system shows of the path and of OneDrive.
    const WINDOWS_FIELDS: [&str; 3] = ["component", "oneDriveVariable", "cloudRoot"];

    /// What the probe finds in a folder of the tests' own: Windows' file systems ignore letter
    /// case, and the Linux tests' do not.
    const IGNORES_CASE: &str = if cfg!(windows) { "yes" } else { "no" };

    /// The codes of the warnings that a folder of the tests' own may call for on Windows, where
    /// the machine's temporary folder may lie in a known folder or a cloud provider's, or have a
    /// name that is not ASCII. Elsewhere no folder is warned of.
    const WARNING_CODES: [&str; 3] = [
        "folder.cloudSynced",
        "folder.protected",
        "folder.nonAsciiOldWindows",
    ];

    /// Whether `line`, a text report's value, is a warning that a folder of the tests' own may
    /// call for, as [`WARNING_CODES`] lists them.
    fn is_the_machines_warning(line: &str) -> bool {
        cfg!(windows)
            && line
                .split_once(' ')
                .is_some_and(|(code, _)| WARNING_CODES.contains(&code))
    }

    /// Checks that `warnings`, a JSON report's, holds no warning but those a folder of the tests'
    /// own may call for: on Windows only those [`WARNING_CODES`] lists, and elsewhere none.
    #[track_caller]
    fn assert_only_the_machines_warnings(warnings: &Value) {
        let warnings = warnings.as_array().expect("a list of warnings");
        if cfg!(windows) {
            for warning in warnings {
                let code = warning["code"].as_str().unwrap_or_default();
                assert!(WARNING_CODES.contains(&code), "{warning}");
            }
        } else {
            assert!(warnings.is_empty(), "{warnings:?}");
        }
    }

    /// Checks the folder at `folder` as the release build, as JSON when `json` says so, and gives
    /// back the report, which a judging writes whatever the folder holds.
    #[track_caller]
    fn checked(folder: &Path, json: bool) -> String {
        let mut args = vec![OsStr::new("--check-folder"), folder.as_os_str()];
        if json {
            args.push(OsStr::new("--json"));
        }
        let ran = run_release(args.as_slice());
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.err, "");
        ran.out
    }

    /// The `name: value` lines of a text report, split at their first `: `.
    fn lines_of(text: &str) -> Vec<(String, String)> {
        assert!(text.ends_with('\n'), "{text:?}");
        text.lines()
            .map(|line| {
                let (name, value) = line.split_once(": ").expect("a name and a value");
                (name.to_owned(), value.to_owned())
            })
            .collect()
    }

    /// Checks that `text`, a real folder's text report, opens with exactly `head`, the fields
    /// every judging writes, then gives the blockers `blockers`, and after them only the
    /// warnings a folder of the tests' own may call for and the lines that tell what Windows
    /// shows, and those only on Windows.
    #[track_caller]
    fn assert_folder_text(text: &str, head: &[(&str, String)], blockers: &[String]) {
        let lines = lines_of(text);
        let names: Vec<&str> = head.iter().map(|(name, _)| *name).collect();
        assert_eq!(names, FOLDER_FIELDS, "the test's own head");
        let found: Vec<(&str, &str)> = lines
            .iter()
            .map(|(name, value)| (name.as_str(), value.as_str()))
            .collect();
        let expected: Vec<(&str, &str)> = head
            .iter()
            .map(|(name, value)| (*name, value.as_str()))
            .chain(blockers.iter().map(|line| ("blocker", line.as_str())))
            .collect();
        assert!(found.len() >= expected.len(), "{text}");
        assert_eq!(found[..expected.len()], expected, "{text}");
        for (name, value) in &found[expected.len()..] {
            let shown = cfg!(windows) && WINDOWS_FIELDS.contains(name);
            let warned = *name == "warning" && is_the_machines_warning(value);
            assert!(shown || warned, "{text}");
        }
    }

    /// The head of `text`, the text report of a folder that the judging measured whole: the
    /// folder at `path`, there when `exists` says so, its nearest entry `nearest`, which takes
    /// entries, on a volume that has room and runs programs.
    fn measured_head(
        path: &Path,
        exists: bool,
        nearest: &Path,
        text: &str,
    ) -> Vec<(&'static str, String)> {
        let available = lines_of(text)
            .into_iter()
            .find(|(name, _)| name == "availableBytes")
            .map(|(_, value)| value)
            .expect("the volume's room");
        assert!(available.parse::<u64>().unwrap() >= SPACE_MARGIN, "{text}");
        vec![
            ("path", path.display().to_string()),
            ("exists", (if exists { "yes" } else { "no" }).to_owned()),
            ("nearestExisting", nearest.display().to_string()),
            ("availableBytes", available),
            ("rootUtf16", root_utf16_len(path).unwrap().to_string()),
            ("writable", "yes".to_owned()),
            ("ignoresCase", IGNORES_CASE.to_owned()),
            ("noexec", "no".to_owned()),
        ]
    }

    /// The head of a text report for a judging of the folder at `path` that ended before it
    /// probed the folder, which tells whether the folder is there as `exists` and its nearest
    /// entry as `nearest`.
    fn unprobed_head(path: &Path, exists: &str, nearest: String) -> Vec<(&'static str, String)> {
        let unknown = || "unknown".to_owned();
        vec![
            ("path", path.display().to_string()),
            ("exists", exists.to_owned()),
            ("nearestExisting", nearest),
            ("availableBytes", unknown()),
            ("rootUtf16", unknown()),
            ("writable", unknown()),
            ("ignoresCase", unknown()),
            ("noexec", unknown()),
        ]
    }

    /// The text report's lines `head`, then a blocker line for each of `blockers`.
    fn report_lines(head: &[(&str, String)], blockers: &[&str]) -> String {
        head.iter()
            .map(|(name, value)| format!("{name}: {value}\n"))
            .chain(
                blockers
                    .iter()
                    .map(|blocker| format!("blocker: {blocker}\n")),
            )
            .collect()
    }

    /// A report of the judging of `path` that found nothing and asked nothing.
    fn blank_report(path: PathBuf) -> DestinationReport {
        DestinationReport {
            path,
            exists: None,
            nearest_existing: None,
            available_bytes: None,
            root_utf16: None,
            probe: None,
            noexec: None,
            blockers: Vec::new(),
            warnings: Vec::new(),
            components: Vec::new(),
            onedrive_variables: Vec::new(),
            cloud_roots: Vec::new(),
        }
    }

    /// A folder this system's paths can name, though no test makes it.
    fn games_folder() -> PathBuf {
        PathBuf::from(if cfg!(windows) { r"C:\Games" } else { "/games" })
    }

    #[test]
    fn check_folder_reports_text_and_json() {
        // Every field a report has, as a judging that went to the end on Windows would give it.
        let games = games_folder();
        let root = games.parent().unwrap().to_path_buf();
        let destination = games.join("Gauntlet Dark Legacy");
        let onedrive = root.join("Users").join("player").join("OneDrive");
        let report = DestinationReport {
            exists: Some(false),
            nearest_existing: Some(games.clone()),
            available_bytes: Some(7),
            root_utf16: Some(25),
            probe: Some(FolderProbe {
                writable: false,
                ignores_case: false,
            }),
            noexec: Some(true),
            blockers: vec![
                Blocker::NotWritable {
                    path: games.clone(),
                },
                Blocker::Noexec {
                    path: games.clone(),
                },
                Blocker::Space {
                    needed: SPACE_MARGIN,
                    available: 7,
                },
            ],
            warnings: vec![
                Warning::CloudSynced {
                    path: onedrive.clone(),
                },
                Warning::Protected {
                    folder: KnownFolder::Documents,
                    path: games.clone(),
                },
                Warning::NonAsciiOldWindows { build: 17_763 },
            ],
            components: vec![
                ComponentAttributes {
                    path: games.clone(),
                    attributes: 0x10,
                    reparse_tag: None,
                },
                ComponentAttributes {
                    path: root.clone(),
                    attributes: 0x0416,
                    reparse_tag: Some(0x9000_001A),
                },
            ],
            onedrive_variables: vec![("OneDrive".to_owned(), onedrive.clone())],
            cloud_roots: vec![onedrive.clone()],
            ..blank_report(destination.clone())
        };
        let view = FolderView::of(&report);
        let (games_shown, root_shown) = (games.display(), root.display());
        let onedrive_shown = onedrive.display();
        assert_eq!(
            view.text(),
            format!(
                "path: {}\n\
                 exists: no\n\
                 nearestExisting: {games_shown}\n\
                 availableBytes: 7\n\
                 rootUtf16: 25\n\
                 writable: no\n\
                 ignoresCase: no\n\
                 noexec: yes\n\
                 blocker: folder.notWritable (notWritable): The launcher may not make entries in \
                 {games_shown}\n\
                 blocker: folder.noexec (noexec): The file system at {games_shown} does not run \
                 programs (noexec)\n\
                 blocker: space.insufficient (space): Not enough free disk space for the \
                 installation\n\
                 warning: folder.cloudSynced (cloudSynced): The destination is kept in sync with \
                 the cloud: {onedrive_shown}\n\
                 warning: folder.protected (protected): The destination lies in the documents \
                 folder, which Controlled Folder Access may guard: {games_shown}\n\
                 warning: folder.nonAsciiOldWindows (nonAsciiOldWindows): This Windows build \
                 (17763) predates 1903, and the destination's path is not ASCII\n\
                 component: {games_shown} attributes 0x00000010 tag none\n\
                 component: {root_shown} attributes 0x00000416 tag 0x9000001A\n\
                 oneDriveVariable: OneDrive {onedrive_shown}\n\
                 cloudRoot: {onedrive_shown}\n",
                destination.display()
            )
        );
        let text = json_report(&view);
        let parsed: Value = serde_json::from_str(&text).unwrap();
        let finding = |code: &str, rule: &str, detail: String| json!({"code": code, "rule": rule, "detail": detail});
        assert_eq!(
            parsed,
            json!({
                "schema": 1,
                "path": destination.display().to_string(),
                "exists": false,
                "nearestExisting": games_shown.to_string(),
                "availableBytes": 7,
                "rootUtf16": 25,
                "writable": false,
                "ignoresCase": false,
                "noexec": true,
                "blockers": [
                    finding(
                        "folder.notWritable",
                        "notWritable",
                        format!("The launcher may not make entries in {games_shown}"),
                    ),
                    finding(
                        "folder.noexec",
                        "noexec",
                        format!("The file system at {games_shown} does not run programs (noexec)"),
                    ),
                    finding(
                        "space.insufficient",
                        "space",
                        "Not enough free disk space for the installation".to_owned(),
                    ),
                ],
                "warnings": [
                    finding(
                        "folder.cloudSynced",
                        "cloudSynced",
                        format!("The destination is kept in sync with the cloud: {onedrive_shown}"),
                    ),
                    finding(
                        "folder.protected",
                        "protected",
                        format!(
                            "The destination lies in the documents folder, which Controlled \
                             Folder Access may guard: {games_shown}"
                        ),
                    ),
                    finding(
                        "folder.nonAsciiOldWindows",
                        "nonAsciiOldWindows",
                        "This Windows build (17763) predates 1903, and the destination's path is \
                         not ASCII"
                            .to_owned(),
                    ),
                ],
                "components": [
                    {"path": games_shown.to_string(), "attributes": 16, "reparseTag": null},
                    {"path": root_shown.to_string(), "attributes": 1046, "reparseTag": 0x9000_001A_u32},
                ],
                "oneDriveVariables": [{"name": "OneDrive", "path": onedrive_shown.to_string()}],
                "cloudRoots": [onedrive_shown.to_string()],
            })
        );
        assert_eq!(keys_at(&text, 2), FOLDER_KEYS);
        let mut item_keys = FINDING_KEYS.repeat(6);
        item_keys.extend(COMPONENT_KEYS.repeat(2));
        item_keys.extend(VARIABLE_KEYS);
        assert_eq!(keys_at(&text, 6), item_keys);
        assert!(text.ends_with("}\n"), "{text}");

        // A judging that ended at an alpha installer's name on the path asked nothing, the
        // nearest entry included; one that found nothing on the path looked for that entry.
        let alpha = games.join(".gdl-update").join("game");
        let ended = DestinationReport {
            blockers: vec![Blocker::AlphaName {
                name: ".gdl-update",
            }],
            ..blank_report(alpha.clone())
        };
        assert_eq!(
            FolderView::of(&ended).text(),
            report_lines(
                &unprobed_head(&alpha, "unknown", "unknown".to_owned()),
                &[
                    "folder.alphaInstall (alphaName): The destination's path holds the alpha \
                   installer's name .gdl-update, which the launcher never writes"
                ]
            )
        );
        let parsed: Value = serde_json::from_str(&json_report(&FolderView::of(&ended))).unwrap();
        for key in &FOLDER_KEYS[2..9] {
            assert_eq!(parsed[key], Value::Null, "{key}");
        }
        let unreachable = DestinationReport {
            exists: Some(false),
            blockers: vec![Blocker::Unreachable {
                path: destination.clone(),
            }],
            ..blank_report(destination.clone())
        };
        assert_eq!(
            FolderView::of(&unreachable).text(),
            report_lines(
                &unprobed_head(&destination, "no", "none".to_owned()),
                &[&format!(
                    "folder.notDirectory (unreachable): No folder on the destination's path \
                     exists: {}",
                    destination.display()
                )]
            )
        );
        let parsed: Value =
            serde_json::from_str(&json_report(&FolderView::of(&unreachable))).unwrap();
        assert_eq!(parsed["exists"], false);
        assert_eq!(parsed["nearestExisting"], Value::Null);

        // Names that hold a line end with a field of their own after it, then the commands that
        // clear a terminal and colour its text: escaped in text, given back exactly in JSON.
        const FORGED: &str = "x\nwritable: yes\u{1b}[2J\u{9b}31m\r";
        const SHOWN: &str = r"x\nwritable: yes\u{1b}[2J\u{9b}31m\r";
        let forged = games.join(FORGED);
        let report = DestinationReport {
            exists: Some(true),
            nearest_existing: Some(forged.clone()),
            blockers: vec![Blocker::Links {
                path: forged.clone(),
            }],
            components: vec![ComponentAttributes {
                path: forged.clone(),
                attributes: 0x10,
                reparse_tag: None,
            }],
            onedrive_variables: vec![("OneDrive".to_owned(), forged.clone())],
            cloud_roots: vec![forged.clone()],
            ..blank_report(forged.clone())
        };
        let text = FolderView::of(&report).text();
        assert!(
            !text.chars().any(|c| c.is_control() && c != '\n'),
            "{text:?}"
        );
        let shown = games.join(SHOWN).display().to_string();
        let names: Vec<String> = lines_of(&text).into_iter().map(|(name, _)| name).collect();
        let mut expected = FOLDER_FIELDS.to_vec();
        expected.extend(["blocker", "component", "oneDriveVariable", "cloudRoot"]);
        assert_eq!(names, expected, "{text}");
        for line in [
            format!("path: {shown}\n"),
            format!("nearestExisting: {shown}\n"),
            format!(
                "blocker: folder.links (links): Choose a directory without links or junctions: \
                 {shown}\n"
            ),
            format!("component: {shown} attributes 0x00000010 tag none\n"),
            format!("oneDriveVariable: OneDrive {shown}\n"),
            format!("cloudRoot: {shown}\n"),
        ] {
            assert!(text.contains(&line), "{line:?} in {text}");
        }
        let parsed: Value = serde_json::from_str(&json_report(&FolderView::of(&report))).unwrap();
        let exactly = forged.display().to_string();
        assert_eq!(parsed["path"], exactly);
        assert_eq!(parsed["components"][0]["path"], exactly);
        assert_eq!(parsed["cloudRoots"][0], exactly);
    }

    #[test]
    fn check_folder_reports_real_folders() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();

        // A new folder, made in a folder that takes entries: judged whole, its probe made and
        // removed in the folder above it.
        let fresh = top.join("game");
        let text = checked(&fresh, false);
        assert_folder_text(&text, &measured_head(&fresh, false, top, &text), &[]);
        assert!(!fresh.exists());
        assert_eq!(fs::read_dir(top).unwrap().count(), 0, "the probe was left");
        let report = checked(&fresh, true);
        assert_eq!(keys_at(&report, 2), FOLDER_KEYS[..14]);
        let parsed: Value = serde_json::from_str(&report).unwrap();
        assert_eq!(parsed["schema"], 1);
        assert_eq!(parsed["path"], fresh.display().to_string());
        assert_eq!(parsed["exists"], false);
        assert_eq!(parsed["nearestExisting"], top.display().to_string());
        assert!(parsed["availableBytes"].is_u64(), "{report}");
        assert_eq!(parsed["rootUtf16"], root_utf16_len(&fresh).unwrap());
        assert_eq!(parsed["writable"], true);
        assert_eq!(parsed["ignoresCase"], cfg!(windows));
        assert_eq!(parsed["noexec"], false);
        assert_eq!(parsed["blockers"], json!([]));
        assert_only_the_machines_warnings(&parsed["warnings"]);
        // What Windows shows of each entry on the path that is there, the nearest first.
        let components: Vec<Value> = if cfg!(windows) {
            top.ancestors()
                .map(|entry| Value::from(entry.display().to_string()))
                .collect()
        } else {
            Vec::new()
        };
        let shown: Vec<Value> = parsed["components"]
            .as_array()
            .unwrap()
            .iter()
            .map(|component| component["path"].clone())
            .collect();
        assert_eq!(shown, components);
        if cfg!(not(windows)) {
            assert_eq!(parsed["oneDriveVariables"], json!([]));
            assert_eq!(parsed["cloudRoots"], json!([]));
        }

        // An empty folder, probed as the destination itself; and one holding names the install
        // makes, in another letter case too, which goes on to be probed.
        let empty = top.join("empty");
        fs::create_dir(&empty).unwrap();
        let text = checked(&empty, false);
        assert_folder_text(&text, &measured_head(&empty, true, &empty, &text), &[]);
        assert_eq!(
            fs::read_dir(&empty).unwrap().count(),
            0,
            "the probe was left"
        );
        // Names are compared in upper case, so on Linux the disc's folder takes the
        // executable's name too.
        let taken = top.join("taken");
        fs::create_dir_all(taken.join("Gauntlet")).unwrap();
        fs::write(taken.join("DATA"), b"the player's").unwrap();
        let colliding = if Platform::of_this_build() == Some(Platform::LinuxX64) {
            "Gauntlet, data, gauntlet"
        } else {
            "Gauntlet, data"
        };
        let text = checked(&taken, false);
        assert_folder_text(
            &text,
            &measured_head(&taken, true, &taken, &text),
            &[format!(
                "folder.collision (collision): Existing installation/files would be overwritten: \
                 {colliding}. Choose a fresh directory."
            )],
        );

        // Folders the launcher must not write into: the judging ends before it probes them or
        // asks their volume, and leaves them as they were, their own entries included.
        let state = top.join("another");
        fs::create_dir_all(state.join(STATE_DIR)).unwrap();
        fs::write(state.join(STATE_DIR).join(LOCK_FILE), b"other process").unwrap();
        let alpha = top.join("alpha");
        fs::create_dir(&alpha).unwrap();
        make_alpha_markers(&alpha, &[".gdl-update", "installation.json"]).unwrap();
        let file = top.join("file.txt");
        fs::write(&file, b"not a folder").unwrap();
        let named = top.join(".GDL-UPDATE").join("game");
        backdate(top).unwrap();
        let before = snapshot(top).unwrap();
        let there = |path: &Path| unprobed_head(path, "yes", path.display().to_string());
        let cases = [
            (
                &state,
                there(&state),
                "folder.collision (collision): Existing installation/files would be overwritten: \
                 .gdl-launcher. Choose a fresh directory.",
            ),
            (
                &alpha,
                there(&alpha),
                "folder.alphaInstall (alphaInstall): The folder holds an alpha installation: \
                 installation.json, .gdl-update",
            ),
            (
                &file,
                there(&file),
                "folder.notDirectory (notDirectory): Install destination is not a directory",
            ),
            (
                &named,
                unprobed_head(&named, "unknown", "unknown".to_owned()),
                "folder.alphaInstall (alphaName): The destination's path holds the alpha \
                 installer's name .gdl-update, which the launcher never writes",
            ),
        ];
        for (folder, head, blocker) in &cases {
            assert_eq!(checked(folder, false), report_lines(head, &[blocker]));
            let parsed: Value = serde_json::from_str(&checked(folder, true)).unwrap();
            assert_eq!(parsed["writable"], Value::Null, "{}", folder.display());
            assert_eq!(
                parsed["availableBytes"],
                Value::Null,
                "{}",
                folder.display()
            );
            assert_eq!(parsed["components"], json!([]), "{}", folder.display());
        }
        assert!(snapshot(top).unwrap() == before, "a folder changed");

        // Given with a verbatim prefix, the folder and the entries on its path are shown without
        // it.
        #[cfg(windows)]
        {
            use std::path::{Component, Prefix};

            let on_a_drive = matches!(
                empty.components().next(),
                Some(Component::Prefix(prefix)) if matches!(prefix.kind(), Prefix::Disk(_))
            );
            if on_a_drive {
                let verbatim = PathBuf::from(format!(r"\\?\{}", empty.display()));
                let text = checked(&verbatim, false);
                assert_folder_text(&text, &measured_head(&empty, true, &empty, &text), &[]);
                let parsed: Value = serde_json::from_str(&checked(&verbatim, true)).unwrap();
                assert_eq!(parsed["components"][0]["path"], empty.display().to_string());
            } else {
                eprintln!("skipped the verbatim path: the test's folder is on no drive");
            }
        }
    }

    #[test]
    fn check_folder_names_the_folder_as_the_system_makes_it() {
        // A last name that ends in a dot or a space: Windows drops them from a name it makes, and
        // the judging, which makes the path absolute as Windows does, judges and names the folder
        // without them; elsewhere the name is the folder's own.
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        for name in ["game.", "game ", "game. "] {
            let typed = top.join(name);
            let judged = if cfg!(windows) {
                top.join("game")
            } else {
                typed.clone()
            };
            let parsed: Value = serde_json::from_str(&checked(&typed, true)).unwrap();
            assert_eq!(parsed["path"], judged.display().to_string(), "{name:?}");
            assert_eq!(parsed["exists"], false, "{name:?}");
            assert_eq!(parsed["blockers"], json!([]), "{name:?}");
            assert_eq!(fs::read_dir(top).unwrap().count(), 0, "{name:?}");
        }
    }

    #[test]
    fn the_check_holds_a_folder_to_every_name_an_install_makes() {
        let names: Vec<String> = install_names()
            .iter()
            .map(|name| name.as_str().to_owned())
            .collect();
        let mut expected = vec!["Gauntlet", "carddemo", "saves", "config", STATE_DIR];
        match Platform::of_this_build() {
            Some(Platform::WindowsX64) => {
                expected.extend(["GauntletDarkLegacy-Launcher.exe", "gauntlet.exe"]);
            }
            Some(Platform::LinuxX64) => {
                expected.extend(["GauntletDarkLegacy-Launcher", "gauntlet"]);
            }
            None => {}
        }
        expected.extend(["VERSION", "build-info.json", "portable.flag"]);
        expected.extend(["shaders", "data", "licenses"]);
        if Platform::of_this_build() == Some(Platform::LinuxX64) {
            expected.push("lib");
        }
        assert_eq!(names, expected);

        // A folder holding each of them in upper case, but the state folder, which would end the
        // judging: every one of them collides, named as the install spells it.
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("taken");
        fs::create_dir(&folder).unwrap();
        let upper: BTreeSet<String> = names
            .iter()
            .filter(|name| *name != STATE_DIR)
            .map(|name| name.to_uppercase())
            .collect();
        for name in &upper {
            fs::write(folder.join(name), b"").unwrap();
        }
        let mut colliding: Vec<&str> = names
            .iter()
            .map(String::as_str)
            .filter(|&name| name != STATE_DIR)
            .collect();
        colliding.sort_unstable();
        let text = checked(&folder, false);
        assert_folder_text(
            &text,
            &measured_head(&folder, true, &folder, &text),
            &[format!(
                "folder.collision (collision): Existing installation/files would be overwritten: \
                 {}. Choose a fresh directory.",
                colliding.join(", ")
            )],
        );
    }

    #[test]
    fn check_folder_misuse_is_a_usage_error() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().to_str().expect("the test's folder is Unicode");
        let cases: &[(&[&str], &str)] = &[
            (&["--check-folder"], "--check-folder without a folder"),
            (
                &["--check-folder", ""],
                "--check-folder with an empty folder",
            ),
            (
                &["--check-folder", "", "--json"],
                "--check-folder with an empty folder",
            ),
            (
                &["--check-folder", "--json"],
                "--check-folder followed by an option",
            ),
            (&["--json", "--check-folder"], "--check-folder at the end"),
            (&["--check-folder", folder, "other"], "a second folder"),
            (
                &["--check-folder", folder, "--json", "--json"],
                "--json twice",
            ),
            (
                &["--check-folder", folder, "--check-folder", folder],
                "the mode twice",
            ),
            (
                &["--check-folder", folder, "--inspect", folder],
                "two modes",
            ),
            (&["--version", "--check-folder", folder], "two modes"),
            (&["--check-folder", folder, "--check-payload"], "two modes"),
            (
                &["--check-folder", folder, "--payload", "a.zip"],
                "--payload beside --check-folder",
            ),
            (
                &["--check-folder", folder, "--output", "out"],
                "--output beside --check-folder",
            ),
            (
                &["--check-folder", folder, "--strict", "script"],
                "--strict beside --check-folder",
            ),
            (&["--check-folder", folder, "-j"], "an unknown short option"),
            (
                &["--check-folder", folder, "--verbose"],
                "an unknown option beside the mode",
            ),
        ];
        // Not one of them looks at the folder, so none probes it.
        backdate(dir.path()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        for &(args, what) in cases {
            let ran = run_release(args);
            assert_eq!(ran.code, Some(EXIT_USAGE), "{what}: {args:?}");
            assert_eq!(ran.err, format!("{USAGE}\n"), "{what}: {args:?}");
            assert_eq!(ran.out, "", "{what}: {args:?}");
        }
        assert!(
            snapshot(dir.path()).unwrap() == before,
            "the folder changed"
        );
    }

    #[test]
    fn a_failed_judging_is_a_refusal_line() {
        // A name longer than any file system takes, which the system refuses to look at, holding
        // a line end with a refusal of its own after it and the commands that clear a terminal.
        let dir = tempfile::tempdir().unwrap();
        let name = format!("x\u{1b}[2J\nerror: forged{}", "a".repeat(300));
        let folder = dir.path().join(&name);
        let refused = fs::symlink_metadata(&folder).unwrap_err();
        let line = format!(
            "error: io.failed (io): {}\n",
            text_value(&format!("{}: {refused}", folder.display()))
        );
        for json in [false, true] {
            let mut args = vec![OsStr::new("--check-folder"), folder.as_os_str()];
            if json {
                args.push(OsStr::new("--json"));
            }
            let ran = run_release(args.as_slice());
            assert_refused(&ran, &line);
            assert!(
                ran.err.contains(r"x\u{1b}[2J\nerror: forged"),
                "{}",
                ran.err
            );
            assert_eq!(ran.err.lines().count(), 1, "{}", ran.err);
        }
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 0);
    }
}
