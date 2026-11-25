//! The launcher's modes without a window, for the release scripts, CI and developers at a console:
//! `--version`, `--check-payload`, which checks the runtime the launcher file carries or a runtime
//! zip it is given, `--inspect`, which reports what a folder is to the launcher, `--check-folder`,
//! which reports what a folder is to an install, `--extract`, which extracts a Legends
//! expansion's tree from a dump into a new folder outside every installation, `--check-feed`,
//! which reads the release feed once and reports its newest release for this system without
//! downloading anything, `--update`, which plans an installation's update, or makes it, from the
//! feed's newest release or the runtime the program's own file carries, `--recover`, which
//! recovers whatever jobs a crash or a kill left unfinished in an installation's folder, and
//! `--after-update`, which a launcher copy that an update put in place runs first, to wait for
//! the launcher that ran the update to exit and to remove the copy it ran from.
//! They run before anything builds a window, and what they print is technical text for whoever
//! runs them, never words for players. A run is handed its arguments, its build, its own file's
//! sections and the streams to write to, so that every mode runs inside a test as it runs in the
//! program.

use std::ffi::{OsStr, OsString};
use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, Write};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, PoisonError, mpsc};
use std::{env, panic, process, thread};

use gdl_extract::{Expansion, Trust};
use gdl_files::{
    Cancel, Hold, Job, NoProgress, PhaseId, Progress, RelPath, absolute_lexical, strip_verbatim,
};
use gdl_install::{
    AFTER_UPDATE_OPTION, Access, Changes, ComponentAttributes, DestinationReport, FolderError,
    FolderState, INSPECTION_SCHEMA, Inspection, InstallFolder, InstallationSummary, Linger,
    LockError, PREVIOUS_WAIT_LIMIT, Payload, PayloadError, PlanSource, Previous, Probed,
    RETIRED_DELETE_INTERVAL, RETIRED_DELETE_WINDOW, RUNTIME_SECTION, RecoverError, Recovered,
    RecoveryOutcome, Relation, RetiredCopy, RetiredSweep, SPACE_MARGIN, Sections, SectionsError,
    SourceSummary, Unrecorded, UpdateError, UpdateNotice, UpdateOutcome, UpdatePlan,
    UpdateRecovery, UpdateRequest, UpdateSource, Updated, hand_over, hand_over_needed,
    installed_launcher_name, installed_release, is_installed_copy, judge_destination, plan_update,
    recover, sweep_retired, update, wait_for_previous,
};
use gdl_release::{Asset, Feed, FeedConfig, Platform, Release, ReleaseError, Scan, Version};
use serde::Serialize;

use crate::codes::{self, Report};
use crate::extraction::{
    Counts, ExtractRequest, ExtractSummary, StrictnessChoice, extract_to_new_folder, output_name,
};
use crate::updating::{SourceChoice, choose_source, system_proxies};

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
                     --check-feed | --update <dir> [--yes] [--offline] | --recover <dir> | \
                     --after-update <pid> | --version";

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

/// The mode that reads the release feed once and reports its newest release for this system.
const CHECK_FEED_OPTION: &str = "--check-feed";

/// The mode that plans the update of the installation in the folder after it, or with `--yes`
/// makes it.
const UPDATE_OPTION: &str = "--update";

/// The option of `--update` that makes the update it plans.
const YES_OPTION: &str = "--yes";

/// The option of `--update` that asks no release feed, so that only the runtime the program's own
/// file carries can bring a newer release.
const OFFLINE_OPTION: &str = "--offline";

/// The mode that recovers whatever jobs were left unfinished in the folder after it.
const RECOVER_OPTION: &str = "--recover";

/// The options that choose a mode: arguments with none of them are no headless run. The last is
/// the mode a hand-over starts the launcher copy an update put in place with, the id of the
/// process that ran the update after it.
const MODE_OPTIONS: [&str; 9] = [
    VERSION_OPTION,
    CHECK_PAYLOAD_OPTION,
    INSPECT_OPTION,
    CHECK_FOLDER_OPTION,
    EXTRACT_OPTION,
    CHECK_FEED_OPTION,
    UPDATE_OPTION,
    RECOVER_OPTION,
    AFTER_UPDATE_OPTION,
];

/// How a refusal names a process the system was asked about, before its id: a process has no path
/// of its own.
const PROCESS: &str = "process";

/// A phase's share done, in percent, once all of it is done.
const WHOLE_PERCENT: u64 = 100;

/// How far a phase's share done, in percent, goes on between the progress lines a mode prints.
const PROGRESS_STEP_PERCENT: u64 = 10;

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

/// How a text report writes an entry that the judging looked for and found nowhere, a reparse tag
/// that an entry does not have, and a release or a release's signature that the feed does not
/// hold.
const NOWHERE: &str = "none";

/// How a plan, and an update, write that nothing newer than the installed release is at hand.
const UP_TO_DATE: &str = "upToDate";

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
/// `--check-folder`, `--extract`, `--check-feed`, `--update`, `--recover`, `--after-update` or
/// `--version`, which is no headless run. A mode writes its report to `out` and a refusal to
/// `err`. Arguments that ask for a mode in a form it does not take get the usage line on `err` and
/// [`EXIT_USAGE`]. An option's value is the argument after it, which must not be empty or begin
/// with `--`; options may come in any order, each at most once.
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
/// - `--check-feed` reads the release feed, GitHub's releases list, once, through the system's
///   proxy settings and trust store, as [`Feed::scan`] reads it for the platform this program was
///   built for, with no version below which a release is passed over, and downloads nothing. It
///   writes the platform, the build's version, how many releases the list holds and how many of
///   them were passed over for carrying no launcher file of this platform's on the new line, then
///   the newest release's version, or `none`, each a `name: value` line. For a release, it then
///   writes whether that release is newer than the build, `yes` or `no`, and its launcher file,
///   checksum list and signature, or `none` for a signature it does not carry, each as its name,
///   its size in bytes and GitHub's SHA-256 of it. A feed that cannot be read is refused with the
///   line `error: <code> (<rule>): <detail>`, and [`EXIT_REFUSED`]. A build for a system no
///   release is built for has no platform to read the feed for, and gets the usage line.
/// - `--update <dir>` plans the update of the installation in the folder, `<dir>` made absolute,
///   by a launcher of the build's version, changing nothing, not even what a recovery would: it
///   writes `plan: recover` when a job was left unfinished there, which the update would finish or
///   take back first, and `plan: finishCleanup` when an earlier update's cleanup is still to be
///   finished, which the update would try first. It then chooses where the update comes from, as
///   [`choose_source`] chooses, between the runtime the program's own file carries and the
///   release feed's newest release above the installed one, read as `--check-feed` reads the feed,
///   and plans the update from there as [`plan_update`] does: `plan: <from> -> <to>
///   (bundled|feed)`, then `plan: download <bytes>` for a release, or for the program's own file a
///   `plan: replace <name>`, `plan: add <name>` or `plan: remove <name>` line for each name the
///   update would move; or `plan: upToDate <version>` when nothing newer is at hand. With
///   `--offline` it asks no feed, and only the program's own file can bring a newer release.
///   With `--yes` it makes that update, as [`update`] makes one, waiting while another job holds
///   the folder's lock or the game runs from it, and writing the update's progress as `--recover`
///   writes a recovery's; then `updated: <from> -> <to> (bundled|feed)`, `probed: staged` or
///   `probed: notStaged`, whether the game was probed, a `notice: <code> (<rule>): <detail>` line
///   for each notice, and `retired: removed`, `retired: left <name>` or `retired: none`, what
///   became of the launcher copy installed before. When the program's own file was the
///   installation's launcher copy as the program started, or is older than the release now
///   installed, it starts the new copy to wait for this program to exit and finish the hand-over,
///   and writes `handedOver: <pid>`. With nothing newer at hand, or a release no newer than the
///   installation by the time the update looks, it recovers what jobs left in the folder, as
///   `--recover` does, and writes `upToDate: <version>`, the release installed once it is done,
///   handing over as `--recover` hands over. A refusal is the line
///   `error: <code> (<rule>): <detail>`, and [`EXIT_REFUSED`]. With `--yes`, the line
///   `changes: none`, `changes: rolledBack` or `changes: kept` follows the refusal of an update,
///   telling what it left, `none` for one refused before it began; the refusal of a recovery made
///   because nothing newer is at hand, and of a hand-over once the update stands, is the error
///   line alone, as `--recover` writes one. A build for a system no release is built for has no
///   platform to update, and gets the usage line.
/// - `--recover <dir>` recovers whatever jobs were left unfinished in the folder, `<dir>` made
///   absolute, as [`recover`] recovers them for a launcher of the build's version, waiting while
///   another job holds the folder's lock or the game runs from it, since nothing here cancels a
///   hold. As the recovery goes it writes its progress as every job's is written here: a
///   `phase: <id>` line as each part begins, `progress: <id> <n>%` each time a part's share done
///   passes a tenth, and `hold: <code>` and `resumed` as a wait begins and ends. Then it writes
///   `install: <outcome>` and `update: <outcome>`, what became of a fresh install's journal and of
///   an update's, and a `swept: <name>` line for each leftover removed and a `left: <name>` line
///   for each one kept, in byte order. When the program's own file was the installation's
///   launcher copy as the program started, and the recovery took an update back and put the
///   installed copy back in its place, the program now runs from a retired name: it starts that
///   copy to wait for this program to exit and finish the hand-over, and writes
///   `handedOver: <pid>`. A refusal is the line `error: <code> (<rule>): <detail>`, and
///   [`EXIT_REFUSED`].
/// - `--after-update <pid>` finishes a hand-over as the launcher copy an update put in place,
///   which a launcher that ran the update starts with its own process's id, as [`hand_over`]
///   starts it: the installation is the folder of the program's own file. It waits for that
///   process to exit, as [`wait_for_previous`] waits for it, and writes what became of it:
///   `previous: gone`, `previous: newer` for a process that started no earlier than this one,
///   `previous: exited after <ms> ms`, `previous: ended` for a launcher ended at the limit,
///   `previous: lingering notALauncher` or `previous: lingering noRight` for a process left
///   running, or `previous: unknown` on a system that cannot be asked. Then it removes the
///   installation's retired launcher copies that no journal names, as [`sweep_retired`] removes
///   them for a launcher of the build's version, and writes `retired: removed <n>, kept <k>`, how
///   many it removed and how many it left; or `retired: lockBusy` when another job holds the
///   installation's lock, since nothing here waits for it or cancels. A process id must be a
///   decimal of 1 or more, and not this process's own; any other is a usage error. A wait or a
///   sweep the system or the installation refused is the line
///   `error: <code> (<rule>): <detail>`, and [`EXIT_REFUSED`].
///
/// A refusal's detail is written as a text report writes a value, each control character as its
/// escape, so that the refusal stays one line whatever the names in its paths, or the text it
/// quotes from a dump, hold. A report that cannot be written to its stream gives
/// [`EXIT_REFUSED`], since nobody saw it.
///
/// Panics when `--inspect`, `--check-feed`, `--update`, `--recover` or `--after-update` runs as a
/// build whose version is not a semantic version, which no build script lets a build be.
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
        Mode::CheckFeed => match Platform::of_this_build() {
            Some(platform) => {
                let config = FeedConfig::github(build.version, system_proxies());
                check_feed(config, platform, build, out, err)
            }
            None => usage(err),
        },
        Mode::Update {
            folder,
            yes,
            offline,
        } => match Platform::of_this_build() {
            Some(platform) => {
                let feed = (!offline).then(|| FeedConfig::github(build.version, system_proxies()));
                let asked = UpdateAsked {
                    folder: Path::new(folder),
                    yes,
                    feed,
                    platform,
                };
                update_mode(asked, build, own, out, err)
            }
            None => usage(err),
        },
        Mode::Recover { folder } => recover_mode(Path::new(folder), build, own, out, err),
        // A process never waits for itself: its own id names no launcher an update replaced.
        Mode::AfterUpdate { pid } if pid == process::id() => usage(err),
        Mode::AfterUpdate { pid } => after_update_mode(pid, build, out, err),
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
    /// `--check-feed`.
    CheckFeed,
    /// `--update`, of the installation in `folder`: made when `yes` says so, else planned, and
    /// from the program's own file alone when `offline` says so.
    Update {
        folder: &'a OsStr,
        yes: bool,
        offline: bool,
    },
    /// `--recover`, of `folder`.
    Recover { folder: &'a OsStr },
    /// `--after-update`, for the launcher process whose id is `pid`.
    AfterUpdate { pid: u32 },
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
    /// `--check-feed`.
    CheckFeed,
    /// `--update`, with the folder after it.
    Update(&'a OsStr),
    /// `--recover`, with the folder after it.
    Recover(&'a OsStr),
    /// `--after-update`, with the process id after it, as it was given.
    AfterUpdate(&'a OsStr),
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
    let mut yes = false;
    let mut offline = false;
    let mut rest = args.iter().map(OsString::as_os_str);
    while let Some(arg) = rest.next() {
        let mode = match arg.to_str() {
            Some(VERSION_OPTION) => Chosen::Version,
            Some(CHECK_PAYLOAD_OPTION) => Chosen::CheckPayload,
            Some(INSPECT_OPTION) => Chosen::Inspect(value(&mut rest)?),
            Some(CHECK_FOLDER_OPTION) => Chosen::CheckFolder(value(&mut rest)?),
            Some(EXTRACT_OPTION) => Chosen::Extract(value(&mut rest)?, value(&mut rest)?),
            Some(CHECK_FEED_OPTION) => Chosen::CheckFeed,
            Some(UPDATE_OPTION) => Chosen::Update(value(&mut rest)?),
            Some(RECOVER_OPTION) => Chosen::Recover(value(&mut rest)?),
            Some(AFTER_UPDATE_OPTION) => Chosen::AfterUpdate(value(&mut rest)?),
            Some(YES_OPTION) if !yes => {
                yes = true;
                continue;
            }
            Some(OFFLINE_OPTION) if !offline => {
                offline = true;
                continue;
            }
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
    let updating = (yes, offline);
    match (chosen, zip, json, extracting, updating) {
        (Some(Chosen::Version), None, false, (None, false), (false, false)) => Ok(Mode::Version),
        (Some(Chosen::CheckPayload), zip, false, (None, false), (false, false)) => {
            Ok(Mode::CheckPayload { zip })
        }
        (Some(Chosen::Inspect(folder)), None, json, (None, false), (false, false)) => {
            Ok(Mode::Inspect { folder, json })
        }
        (Some(Chosen::CheckFolder(folder)), None, json, (None, false), (false, false)) => {
            Ok(Mode::CheckFolder { folder, json })
        }
        (
            Some(Chosen::Extract(expansion, dump)),
            None,
            false,
            (Some(output), script),
            (false, false),
        ) => Ok(Mode::Extract {
            expansion,
            dump,
            output,
            script,
        }),
        (Some(Chosen::CheckFeed), None, false, (None, false), (false, false)) => {
            Ok(Mode::CheckFeed)
        }
        (Some(Chosen::Update(folder)), None, false, (None, false), (yes, offline)) => {
            Ok(Mode::Update {
                folder,
                yes,
                offline,
            })
        }
        (Some(Chosen::Recover(folder)), None, false, (None, false), (false, false)) => {
            Ok(Mode::Recover { folder })
        }
        (Some(Chosen::AfterUpdate(pid)), None, false, (None, false), (false, false)) => {
            process_id(pid).map(|pid| Mode::AfterUpdate { pid })
        }
        _ => Err(Misused),
    }
}

/// The id of a process as an argument gives it: a decimal of ASCII digits alone, 1 or more and
/// within what an id holds. A sign, a space, any other character, nought and a number past that
/// name no process.
fn process_id(value: &OsStr) -> Result<u32, Misused> {
    value
        .to_str()
        .filter(|digits| digits.bytes().all(|byte| byte.is_ascii_digit()))
        .and_then(|digits| digits.parse().ok())
        .filter(|&pid| pid != 0)
        .ok_or(Misused)
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
    let inspection = InstallFolder::inspect(folder, &running_version(build));
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

/// The version of the launcher `build` is, which a mode that judges or changes an installation
/// acts as.
fn running_version(build: &Build<'_>) -> Version {
    Version::parse(build.version).expect("the build's version is a semantic version")
}

/// `--check-feed`: reads the release feed `config` names once, as the launcher `build` is, for
/// `platform` and with no floor, so that the newest release is found whatever its version, and
/// reports what it found to `out`, or why it could not be read to `err`. Nothing is downloaded.
fn check_feed(
    config: FeedConfig,
    platform: Platform,
    build: &Build<'_>,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> u8 {
    let running = running_version(build);
    // Nothing here asks the read to stop.
    let cancel = Cancel::default();
    let scanned = Feed::new(config).scan(None, platform, &cancel);
    feed_checked(scanned, &running, platform, out, err)
}

/// Reports what a read of the release feed gave, `scanned`, to a launcher of `running` built for
/// `platform`: what the feed holds, to `out` as [`print_scan`] writes it, and [`EXIT_OK`]; or why
/// it could not be read, to `err` as [`refuse`] writes a refusal, and [`EXIT_REFUSED`].
fn feed_checked(
    scanned: Result<Scan, ReleaseError>,
    running: &Version,
    platform: Platform,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> u8 {
    match scanned {
        Ok(scan) => match print_scan(&scan, running, platform, out) {
            Ok(()) => EXIT_OK,
            Err(_) => EXIT_REFUSED,
        },
        Err(error) => refuse(err, codes::release(&error), error.rule(), &error),
    }
}

/// Writes to `out` what a read of the release feed found, `scan`, for a launcher of `running`
/// built for `platform`: the platform, the launcher's version, how many releases the list holds
/// and how many were passed over, then the newest release's version, or `none`, each a
/// `name: value` line; and for a release, whether it is newer than the launcher, `yes` or `no`,
/// then its launcher file, its checksum list and its signature, or `none` when it carries none,
/// each as [`asset_line`] gives it. Each value is written as a text report writes a value.
fn print_scan(
    scan: &Scan,
    running: &Version,
    platform: Platform,
    out: &mut dyn Write,
) -> io::Result<()> {
    let mut lines = vec![
        ("platform", platform.as_str().to_owned()),
        ("running", running.to_string()),
        ("releases", scan.rows.to_string()),
        ("passedOver", scan.passed_over.to_string()),
    ];
    match &scan.candidate {
        None => lines.push(("newest", NOWHERE.to_owned())),
        Some(release) => {
            let signature = release
                .signature
                .as_ref()
                .map_or_else(|| NOWHERE.to_owned(), asset_line);
            lines.extend([
                ("newest", release.version.to_string()),
                ("newer", answer(Some(release.version > *running))),
                ("launcher", asset_line(&release.launcher)),
                ("checksums", asset_line(&release.checksums)),
                ("signature", signature),
            ]);
        }
    }
    written(out, &text_report(lines))
}

/// A release's file as the feed check reports it: its name, its size in bytes and GitHub's
/// SHA-256 of it, in lowercase hexadecimal.
fn asset_line(asset: &Asset) -> String {
    format!("{} {} {}", asset.name, asset.size, asset.sha256)
}

/// `--recover`: recovers whatever jobs were left unfinished in the folder at `folder` for the
/// launcher the build is, writing the recovery's progress to `out` as it goes and what it did once
/// it is done, or why it refused to `err`; then hands over to the installation's launcher copy
/// when the recovery put that copy back in place of the program's own file, `own`.
fn recover_mode(
    folder: &Path,
    build: &Build<'_>,
    own: &OwnFile,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> u8 {
    let running = running_version(build);
    let platform = Platform::of_this_build();
    let root = match absolute_lexical(folder) {
        Ok(root) => root,
        Err(source) => {
            let path = folder.to_path_buf();
            return refuse_recovery(err, &RecoverError::Io { path, source });
        }
    };
    // Told before the recovery, which may move the copy aside and put another file at its name.
    let was_installed_copy = match was_installed_copy(own, &root, platform, recovery_io) {
        Ok(answer) => answer,
        Err(error) => return refuse_recovery(err, &error),
    };
    let (recovered, shown) = printing(out, |progress| {
        // Nothing here asks the recovery to stop, so a hold lasts until what holds it ends.
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress,
        };
        recover(&root, &running, &mut Unrecorded, &job)
    });
    let recovered = match recovered {
        Ok(recovered) => recovered,
        Err(error) => return refuse_recovery(err, &error),
    };
    let mut report = shown.and_then(|()| print_recovered(&recovered, out));
    // The program runs from a retired name once the copy is back, whether or not its report was
    // seen, so it hands over all the same.
    if let Some(platform) = platform.filter(|_| hands_over(was_installed_copy, &recovered)) {
        match handed_over(&root, platform, recovery_io) {
            Ok(line) => report = report.and_then(|()| written(out, &line)),
            Err(error) => return refuse_recovery(err, &error),
        }
    }
    match report {
        Ok(()) => EXIT_OK,
        Err(_) => EXIT_REFUSED,
    }
}

/// Writes the line that reports a recovery's refusal, `error`, to `err`, as [`refuse`] writes one,
/// and gives back [`EXIT_REFUSED`].
fn refuse_recovery(err: &mut dyn Write, error: &RecoverError) -> u8 {
    refuse(err, codes::recover(error), error.rule(), error)
}

/// A recovery's refusal of what the system answered for `path`.
fn recovery_io(path: PathBuf, source: io::Error) -> RecoverError {
    RecoverError::Io { path, source }
}

/// Whether the program's own file, `own` as it was opened when the program started, is the
/// launcher copy of the installation at `root`, for a launcher built for `platform`: told by the
/// file and not its name, so that it can be told before a job moves the copy aside. A file whose
/// sections could not be found is none, since an install copies the launcher file whole with its
/// sections and nothing else is ever put at the copy's name; and so is every file on a system no
/// release is built for. What the system answers a look at the copy is refused as `refused` makes
/// a refusal of the copy's path and that answer.
fn was_installed_copy<E>(
    own: &OwnFile,
    root: &Path,
    platform: Option<Platform>,
    refused: impl FnOnce(PathBuf, io::Error) -> E,
) -> Result<bool, E> {
    let (Ok(Some(sections)), Some(platform)) = (own, platform) else {
        return Ok(false);
    };
    is_installed_copy(sections.file(), root, platform)
        .map_err(|source| refused(root.join(installed_launcher_name(platform)), source))
}

/// Starts the launcher copy of the installation at `root`, for a launcher built for `platform`, to
/// wait for this program to exit and finish the hand-over, as [`hand_over`] does, and gives back
/// the line that tells it: `handedOver: <pid>`. A copy that cannot be started is refused as
/// `refused` makes a refusal of the copy's path and what the system answered.
fn handed_over<E>(
    root: &Path,
    platform: Platform,
    refused: impl FnOnce(PathBuf, io::Error) -> E,
) -> Result<String, E> {
    match hand_over(root, platform) {
        Ok(pid) => Ok(format!("handedOver: {pid}\n")),
        Err(source) => Err(refused(
            root.join(installed_launcher_name(platform)),
            source,
        )),
    }
}

/// Whether a program whose own file `was_installed_copy` of an installation hands over to that
/// installation's launcher copy once a recovery of it did `recovered`: when the recovery took an
/// update back and put the installed copy back in its place from its retired name, which leaves
/// this program running from a retired name of its own. A copy put back stands at the
/// installation's top to hand over to; an update that found no copy there to move aside put none
/// back, and then nothing is handed over to.
fn hands_over(was_installed_copy: bool, recovered: &Recovered) -> bool {
    was_installed_copy
        && matches!(
            recovered.update,
            UpdateRecovery::RolledBack {
                launcher_restored: true,
                ..
            }
        )
}

/// Writes to `out` what a recovery did, `recovered`: what became of a fresh install's journal and
/// of an update's, as `install: <outcome>` and `update: <outcome>`, then a `swept: <name>` line
/// for each leftover removed and a `left: <name>` line for each one kept, in byte order, each name
/// written as a text report writes a value.
fn print_recovered(recovered: &Recovered, out: &mut dyn Write) -> io::Result<()> {
    let mut lines = vec![
        ("install", install_outcome(&recovered.install).to_owned()),
        ("update", update_outcome(&recovered.update).to_owned()),
    ];
    lines.extend(recovered.swept.iter().map(|name| ("swept", name.clone())));
    lines.extend(recovered.left.iter().map(|name| ("left", name.clone())));
    written(out, &text_report(lines))
}

/// How a report names what became of a fresh install's journal, in camelCase.
fn install_outcome(outcome: &RecoveryOutcome) -> &'static str {
    match outcome {
        RecoveryOutcome::Nothing => "nothing",
        RecoveryOutcome::RolledBack { .. } => "rolledBack",
        RecoveryOutcome::Finished { .. } => "finished",
        RecoveryOutcome::CleanedUp => "cleanedUp",
        RecoveryOutcome::AbandonedRemoved => "abandonedRemoved",
    }
}

/// How a report names what became of an update's journal, in camelCase.
fn update_outcome(outcome: &UpdateRecovery) -> &'static str {
    match outcome {
        UpdateRecovery::Nothing => "nothing",
        UpdateRecovery::RolledBack { .. } => "rolledBack",
        UpdateRecovery::Finished => "finished",
        UpdateRecovery::CleanedUp => "cleanedUp",
        UpdateRecovery::CleanupLeft { .. } => "cleanupLeft",
    }
}

/// What `--update` is asked to do: plan the update of the installation in `folder`, or make it
/// when `yes` says so, reading the release feed `feed` names, or none when it is `None`, for the
/// platform this program was built for.
struct UpdateAsked<'a> {
    /// The installation's folder, as it was given.
    folder: &'a Path,
    /// Whether to make the update rather than only plan it.
    yes: bool,
    /// The release feed to read, or `None` to read none.
    feed: Option<FeedConfig>,
    /// The platform this program was built for.
    platform: Platform,
}

/// Where an update's new release comes from, as its reports name it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SourceKind {
    /// The runtime the program's own file carries.
    Bundled,
    /// A release the feed found.
    Feed,
}

impl SourceKind {
    /// The source's name in a report.
    fn as_str(self) -> &'static str {
        match self {
            SourceKind::Bundled => "bundled",
            SourceKind::Feed => "feed",
        }
    }
}

/// What `--update` found to update from: nothing newer than the installation, or a source.
enum Picked<'s> {
    /// Nothing newer than the installed release, of this version, is at hand.
    UpToDate(Version),
    /// The source of a newer release.
    Source(Source<'s>),
}

/// The source `--update` takes a newer release from.
enum Source<'s> {
    /// The program's own file, through the sections it found as it started.
    Bundled(&'s mut Sections<File>),
    /// A release of the feed's, whose files are fetched from that feed.
    Release {
        /// The feed the release came from.
        feed: &'s Feed,
        /// The release.
        release: Box<Release>,
    },
}

impl Source<'_> {
    /// How a report names the source.
    fn kind(&self) -> SourceKind {
        match self {
            Source::Bundled(_) => SourceKind::Bundled,
            Source::Release { .. } => SourceKind::Feed,
        }
    }
}

/// An update `--update` plans or makes: of the installation at `root`, an absolute path, by the
/// launcher of version `running` built for `platform`.
struct Updating<'a> {
    /// The installation's folder, absolute.
    root: &'a Path,
    /// The version of the launcher that runs.
    running: &'a Version,
    /// The platform this program was built for.
    platform: Platform,
}

/// `--update`: plans the update `asked` names, or makes it, for the launcher the build is, the
/// program's own file, `own`, among its sources; writes the plan, or the update's progress and
/// what it did, to `out`, and a refusal to `err`; and hands over to the installation's launcher
/// copy when the update or its recovery leaves this program running from a file the installation
/// no longer runs.
fn update_mode(
    asked: UpdateAsked<'_>,
    build: &Build<'_>,
    own: &mut OwnFile,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> u8 {
    let UpdateAsked {
        folder,
        yes,
        feed,
        platform,
    } = asked;
    // A refusal before the update began left everything as it was, which a made update tells.
    let untouched = yes.then_some(Changes::None);
    let running = running_version(build);
    let root = match absolute_lexical(folder) {
        Ok(root) => root,
        Err(source) => {
            let path = folder.to_path_buf();
            return refuse_update(err, &UpdateError::Io { path, source }, untouched);
        }
    };
    // Told before the update, which moves the copy aside and puts another file at its name; a
    // plan moves nothing, and hands nothing over.
    let was_installed_copy = if yes {
        match was_installed_copy(own, &root, Some(platform), update_io) {
            Ok(answer) => answer,
            Err(error) => return refuse_update(err, &error, untouched),
        }
    } else {
        false
    };
    let bundled = match own {
        Ok(Some(sections)) => carries_runtime(sections).then_some(sections),
        Ok(None) => None,
        Err(error) => {
            let error: &SectionsError = error;
            return refused(err, codes::sections(error), error.rule(), error, untouched);
        }
    };
    let feed = feed.map(Feed::new);
    let updating = Updating {
        root: &root,
        running: &running,
        platform,
    };
    if yes {
        let picked = updating.pick(bundled, feed.as_ref());
        return updating.make(picked, was_installed_copy, out, err);
    }
    // What the update would do first is told before its source is sought, which may take a while.
    let inspection = InstallFolder::inspect(&root, &running);
    let mut first = Vec::new();
    if inspection.state == FolderState::Pending {
        first.push(("plan", "recover".to_owned()));
    }
    if inspection.cleanup_pending {
        first.push(("plan", "finishCleanup".to_owned()));
    }
    if written(out, &text_report(first)).is_err() {
        return EXIT_REFUSED;
    }
    let picked = updating.pick(bundled, feed.as_ref());
    updating.plan(picked, out, err)
}

impl Updating<'_> {
    /// Chooses what the update of the installation comes from, as [`choose_source`] chooses: the
    /// program's own file, `bundled` when it carries a runtime, and the newest release above the
    /// installed one of `feed`, when one is read. The installed release is read as
    /// [`installed_release`] reads it, which refuses what the update's first recovery would
    /// refuse, so that a plan with nothing newer at hand refuses what the update would. A launcher
    /// older than the installation, and a feed that could not be read with nothing newer in the
    /// program's own file, are refused too.
    fn pick<'s>(
        &self,
        bundled: Option<&'s mut Sections<File>>,
        feed: Option<&'s Feed>,
    ) -> Result<Picked<'s>, UpdateError> {
        let installed = installed_release(self.root, self.running)?;
        // The runtime a launcher file carries is of the launcher's own version, which the update
        // holds it to before anything of it is used.
        let own = bundled.as_ref().map(|_| self.running);
        // Nothing here asks the read to stop.
        let cancel = Cancel::default();
        let answer = feed.map(|feed| feed.newest(&installed, self.platform, &cancel));
        match choose_source(self.running, own, &installed, answer).map_err(UpdateError::Release)? {
            SourceChoice::OlderLauncher { installed } => Err(UpdateError::OlderLauncher {
                running: self.running.to_string().into(),
                installed: installed.to_string().into(),
            }),
            SourceChoice::UpToDate { version } => Ok(Picked::UpToDate(version)),
            SourceChoice::Bundled { .. } => Ok(Picked::Source(Source::Bundled(
                bundled
                    .expect("the program's own runtime is chosen only when its file carries one"),
            ))),
            SourceChoice::Release(release) => Ok(Picked::Source(Source::Release {
                feed: feed.expect("a release is chosen only from a feed that was read"),
                release,
            })),
        }
    }

    /// Plans the update from what `picked` found, as [`plan_update`] plans one, and writes the
    /// plan to `out` as [`print_plan`] writes it, or why it was refused to `err`.
    fn plan(
        &self,
        picked: Result<Picked<'_>, UpdateError>,
        out: &mut dyn Write,
        err: &mut dyn Write,
    ) -> u8 {
        let planned = match picked {
            Ok(Picked::UpToDate(version)) => {
                let lines = vec![("plan", format!("{UP_TO_DATE} {version}"))];
                return reported(out, &text_report(lines));
            }
            Ok(Picked::Source(Source::Bundled(sections))) => {
                plan_update(self.root, self.running, PlanSource::Bundled(sections))
            }
            Ok(Picked::Source(Source::Release { release, .. })) => {
                plan_update(self.root, self.running, PlanSource::Release(&release))
            }
            Err(error) => Err(error),
        };
        match planned.map(|plan| print_plan(&plan, out)) {
            Ok(Ok(())) => EXIT_OK,
            Ok(Err(_)) => EXIT_REFUSED,
            Err(error) => refuse_update(err, &error, None),
        }
    }

    /// Makes the update from what `picked` found, as [`update`] makes one, writing its progress to
    /// `out` as it goes and what it did once it stands, as [`print_updated`] writes it, or why it
    /// was refused, with what it left, to `err`. With nothing newer at hand, it recovers what jobs
    /// left in the folder instead. Then it hands over to the installation's launcher copy when the
    /// program's own file, which `was_installed_copy` tells, is no longer what the installation
    /// runs: as [`hand_over_needed`] says once the update stands, and as a recovery's hand-over
    /// does when it changed nothing more.
    fn make(
        &self,
        picked: Result<Picked<'_>, UpdateError>,
        was_installed_copy: bool,
        out: &mut dyn Write,
        err: &mut dyn Write,
    ) -> u8 {
        let source = match picked {
            Ok(Picked::Source(source)) => source,
            Ok(Picked::UpToDate(_)) => return self.recover_only(was_installed_copy, out, err),
            Err(error) => return refuse_update(err, &error, Some(Changes::None)),
        };
        let kind = source.kind();
        let (answered, shown) = printing(out, move |progress| {
            // Nothing here asks the update to stop, so a hold lasts until what holds it ends.
            let cancel = Cancel::default();
            let job = Job {
                cancel: &cancel,
                progress,
            };
            match source {
                Source::Bundled(sections) => {
                    let request = UpdateRequest {
                        root: self.root,
                        running: self.running,
                        source: UpdateSource::Bundled(sections),
                    };
                    update(request, &job)
                }
                Source::Release { feed, release } => {
                    let request = UpdateRequest {
                        root: self.root,
                        running: self.running,
                        source: UpdateSource::Release {
                            files: feed,
                            release: &release,
                        },
                    };
                    update(request, &job)
                }
            }
        });
        let (report, hands) = match answered {
            Ok(UpdateOutcome::Updated(updated)) => (
                shown.and_then(|()| print_updated(&updated, kind, out)),
                hand_over_needed(was_installed_copy, self.running, &updated.to),
            ),
            Ok(UpdateOutcome::AlreadyCurrent { version, recovered }) => (
                shown.and_then(|()| written(out, &up_to_date(&version))),
                hands_over(was_installed_copy, &recovered),
            ),
            Err(failure) => return refuse_update(err, &failure.error, Some(failure.changes)),
        };
        self.finish(report, hands, out, err)
    }

    /// Recovers what jobs left in the folder, as [`recover`] does, when nothing newer than the
    /// installation is at hand, writing its progress to `out` as it goes and then `upToDate:` and
    /// the version installed once it is done, or why it was refused to `err`; then hands over as
    /// `--recover` hands over, when the recovery put the installation's launcher copy back in
    /// place of the program's own file, which `was_installed_copy` tells.
    fn recover_only(
        &self,
        was_installed_copy: bool,
        out: &mut dyn Write,
        err: &mut dyn Write,
    ) -> u8 {
        let (recovered, shown) = printing(out, |progress| {
            // Nothing here asks the recovery to stop, so a hold lasts until what holds it ends.
            let cancel = Cancel::default();
            let job = Job {
                cancel: &cancel,
                progress,
            };
            recover(self.root, self.running, &mut Unrecorded, &job)
        });
        let recovered = match recovered {
            Ok(recovered) => recovered,
            Err(error) => return refuse_recovery(err, &error),
        };
        // A recovery may finish an update that left the installation's files moved, so the
        // installed release is read again once it is done.
        let version = match installed_release(self.root, self.running) {
            Ok(version) => version,
            Err(error) => return refuse_update(err, &error, None),
        };
        let report = shown.and_then(|()| written(out, &up_to_date(&version)));
        self.finish(report, hands_over(was_installed_copy, &recovered), out, err)
    }

    /// Hands over to the installation's launcher copy when `hands` says so, writing
    /// `handedOver: <pid>` to `out` after the report, whose writing `report` tells; and gives back
    /// the exit code of a run whose update or recovery stands: [`EXIT_OK`] once every line is out,
    /// [`EXIT_REFUSED`] when one could not be written or the copy could not be started, which is
    /// refused on `err` with the error line alone.
    fn finish(
        &self,
        report: io::Result<()>,
        hands: bool,
        out: &mut dyn Write,
        err: &mut dyn Write,
    ) -> u8 {
        let mut report = report;
        if hands {
            match handed_over(self.root, self.platform, update_io) {
                Ok(line) => report = report.and_then(|()| written(out, &line)),
                Err(error) => return refuse_update(err, &error, None),
            }
        }
        match report {
            Ok(()) => EXIT_OK,
            Err(_) => EXIT_REFUSED,
        }
    }
}

/// The line that reports a made update's installation of `version` with nothing newer at hand.
fn up_to_date(version: &Version) -> String {
    text_report(vec![(UP_TO_DATE, version.to_string())])
}

/// An update's refusal of what the system answered for `path`.
fn update_io(path: PathBuf, source: io::Error) -> UpdateError {
    UpdateError::Io { path, source }
}

/// Whether the launcher file whose sections are `sections` carries a runtime.
fn carries_runtime(sections: &Sections<File>) -> bool {
    sections
        .sections()
        .iter()
        .any(|section| section.name == RUNTIME_SECTION)
}

/// Writes the line that reports an update's refusal, `error`, to `err`, as [`refuse`] writes one,
/// then, when `changes` tells what the refused update left, a line that tells it; and gives back
/// [`EXIT_REFUSED`].
fn refuse_update(err: &mut dyn Write, error: &UpdateError, changes: Option<Changes>) -> u8 {
    refused(err, codes::update(error), error.rule(), error, changes)
}

/// Writes the line that reports a refusal to `err`, as [`refuse`] writes one, then, when `changes`
/// tells what the refused update left, `changes: none`, `changes: rolledBack` or `changes: kept`;
/// and gives back [`EXIT_REFUSED`], written or not.
fn refused(
    err: &mut dyn Write,
    report: Report,
    rule: &str,
    detail: &dyn fmt::Display,
    changes: Option<Changes>,
) -> u8 {
    let code = refuse(err, report, rule, detail);
    if let Some(changes) = changes {
        let _ = written(
            err,
            &text_report(vec![("changes", changes_name(changes).to_owned())]),
        );
    }
    code
}

/// How a report names what a refused update left, in camelCase.
fn changes_name(changes: Changes) -> &'static str {
    match changes {
        Changes::None => "none",
        Changes::RolledBack => "rolledBack",
        Changes::Kept => "kept",
    }
}

/// Writes to `out` what `plan` would do, each line `plan: <what>`: `upToDate <version>` when the
/// release it would bring is no newer than the installed one, which the update would then leave
/// as it is; else `<from> -> <to> (bundled|feed)`, then for a release `download <bytes>`, the size
/// of its launcher file, and for the program's own file `replace <name>`, `add <name>` and
/// `remove <name>` for each name the update would move, in that order and each in byte order,
/// none while a job left unfinished keeps them from being known. Each value is written as a text
/// report writes a value.
fn print_plan(plan: &UpdatePlan, out: &mut dyn Write) -> io::Result<()> {
    let mut lines = Vec::new();
    if plan.to <= plan.from {
        lines.push(format!("{UP_TO_DATE} {}", plan.from));
    } else {
        let kind = match plan.source {
            SourceSummary::Bundled { .. } => SourceKind::Bundled,
            SourceSummary::Release { .. } => SourceKind::Feed,
        };
        lines.push(format!("{} -> {} ({})", plan.from, plan.to, kind.as_str()));
        match &plan.source {
            SourceSummary::Release { download_bytes, .. } => {
                lines.push(format!("download {download_bytes}"));
            }
            SourceSummary::Bundled { .. } => {
                if let Some(changes) = &plan.changes {
                    let moved = [
                        ("replace", &changes.replaced),
                        ("add", &changes.added),
                        ("remove", &changes.removed),
                    ];
                    for (how, names) in moved {
                        lines.extend(names.iter().map(|name| format!("{how} {}", name.as_str())));
                    }
                }
            }
        }
    }
    written(
        out,
        &text_report(lines.into_iter().map(|line| ("plan", line)).collect()),
    )
}

/// Writes to `out` what an update that stands did, `updated`, its new release taken from the
/// source `kind` names, each a `name: value` line: `updated: <from> -> <to> (bundled|feed)`;
/// `probed: staged` or `probed: notStaged`, whether the game was probed; a
/// `notice: <code> (<rule>): <detail>` line for each notice, in the order the update met them; and
/// `retired: removed`, `retired: left <name>` or `retired: none`, what became of the launcher copy
/// installed before. Each value is written as a text report writes a value.
fn print_updated(updated: &Updated, kind: SourceKind, out: &mut dyn Write) -> io::Result<()> {
    let probed = match updated.probed {
        Probed::Staged => "staged",
        Probed::NotStaged => "notStaged",
    };
    let mut lines = vec![
        (
            "updated",
            format!("{} -> {} ({})", updated.from, updated.to, kind.as_str()),
        ),
        ("probed", probed.to_owned()),
    ];
    lines.extend(
        updated
            .notices
            .iter()
            .map(|notice| ("notice", notice_line(notice))),
    );
    let retired = match &updated.retired {
        RetiredCopy::Removed => "removed".to_owned(),
        RetiredCopy::Left { name } => format!("left {name}"),
        RetiredCopy::NotMoved => NOWHERE.to_owned(),
    };
    lines.push(("retired", retired));
    written(out, &text_report(lines))
}

/// A notice of an update that stands as its report's line gives it: `<code> (<rule>): <detail>`,
/// the code as [`codes::update_notice`] gives it and the rule as the notice names it. A failed
/// probe's detail is the staged game's exit status, the first line it wrote and the end of its
/// error stream, each `none` when there is none; a cleanup that stopped has what stopped it.
fn notice_line(notice: &UpdateNotice) -> String {
    let detail = match notice {
        UpdateNotice::ProbeFailed {
            status,
            first_line,
            stderr_tail,
        } => {
            let status = status.map_or_else(|| NOWHERE.to_owned(), |status| status.to_string());
            let first_line = first_line.as_deref().unwrap_or(NOWHERE);
            let stderr = if stderr_tail.is_empty() {
                NOWHERE
            } else {
                stderr_tail.as_str()
            };
            format!("status {status}; first line {first_line}; stderr {stderr}")
        }
        UpdateNotice::CleanupLeft { detail } => detail.to_string(),
    };
    format!(
        "{} ({}): {detail}",
        codes::update_notice(notice),
        notice.rule()
    )
}

/// `--after-update`: finishes the hand-over of the installation the program runs in, the folder of
/// its own file, as the launcher copy of the build's version an update put there, for the launcher
/// process `pid` that ran the update, as [`after_update`] finishes it.
fn after_update_mode(pid: u32, build: &Build<'_>, out: &mut dyn Write, err: &mut dyn Write) -> u8 {
    let running = running_version(build);
    match own_folder() {
        Ok(root) => after_update(pid, &root, &running, out, err),
        Err(error) => refuse_recovery(err, &error),
    }
}

/// The folder of the program's own file, by the path the system gives for this process's program:
/// a hand-over starts the installation's launcher copy at the installation's top, so this is the
/// installation the copy belongs to. A path the system will not give is refused as what it
/// answered when asked about this process.
fn own_folder() -> Result<PathBuf, RecoverError> {
    let program = env::current_exe().map_err(|source| process_refusal(process::id(), source))?;
    program.parent().map(Path::to_path_buf).ok_or_else(|| {
        let source = io::Error::new(
            io::ErrorKind::InvalidData,
            "the program's own file has no folder",
        );
        process_refusal(process::id(), source)
    })
}

/// Finishes the hand-over of the installation at `root`, an absolute path, as its launcher copy of
/// version `running`: waits for the launcher process `pid`, which the update replaced, to exit, as
/// [`wait_for_previous`] waits for it up to [`PREVIOUS_WAIT_LIMIT`], and writes what became of it
/// to `out` as [`print_previous`] writes it; then removes the installation's retired launcher
/// copies that no journal names, as [`sweep_retired`] removes them, trying each for
/// [`RETIRED_DELETE_WINDOW`], and writes what it did as [`print_retired`] writes it. Nothing here
/// waits for the lock: another job that holds it leaves the copies to the next start's sweep.
///
/// A wait the system refused is refused on `err` as a look at the process by its id, and nothing
/// is swept, since the process may still run; a sweep refused, as one is of an installation a
/// later launcher's state stands in or of a release later than `running`, is refused on `err` as
/// [`codes::recover`] reports it, with nothing removed. A refusal gives [`EXIT_REFUSED`].
fn after_update(
    pid: u32,
    root: &Path,
    running: &Version,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> u8 {
    let previous = match wait_for_previous(pid, PREVIOUS_WAIT_LIMIT, root) {
        Ok(previous) => previous,
        Err(source) => return refuse_recovery(err, &process_refusal(pid, source)),
    };
    // Told before the sweep, which may try a copy still held for a while.
    let shown = print_previous(previous, out);
    let swept = match retired_swept(root, running) {
        Ok(swept) => swept,
        Err(error) => return refuse_recovery(err, &error),
    };
    match shown.and_then(|()| print_retired(&swept, out)) {
        Ok(()) => EXIT_OK,
        Err(_) => EXIT_REFUSED,
    }
}

/// A refusal of what the system answered when asked about the process whose id is `pid`, which has
/// no path of its own: it is named by that id, as `process <pid>`.
fn process_refusal(pid: u32, source: io::Error) -> RecoverError {
    RecoverError::Io {
        path: PathBuf::from(format!("{PROCESS} {pid}")),
        source,
    }
}

/// Removes the retired launcher copies of the installation at `root` that no journal names, for
/// the launcher of version `running`, as [`sweep_retired`] removes them. A folder that is not
/// there, or is no folder, holds no state folder and so nothing to sweep, as a recovery finds
/// it; one with a link on its path is refused as a recovery refuses it.
fn retired_swept(root: &Path, running: &Version) -> Result<RetiredSweep, RecoverError> {
    let folder = match InstallFolder::at(root) {
        Ok(folder) => folder,
        Err(FolderError::Absent { .. } | FolderError::NotDirectory { .. }) => {
            return Ok(RetiredSweep {
                removed: Vec::new(),
                kept: Vec::new(),
                lock_busy: false,
            });
        }
        Err(FolderError::Links { path }) => {
            return Err(RecoverError::Lock(LockError::Links { path }));
        }
        Err(FolderError::Io { path, source }) => return Err(RecoverError::Io { path, source }),
    };
    sweep_retired(
        &folder,
        running,
        RETIRED_DELETE_WINDOW,
        RETIRED_DELETE_INTERVAL,
    )
}

/// Writes to `out` what became of the launcher process an update replaced, `previous`, as the line
/// `previous: <what>`: `gone`, `newer`, `exited after <ms> ms`, how long it was waited for in whole
/// milliseconds, `ended`, `lingering notALauncher`, `lingering noRight` or `unknown`.
fn print_previous(previous: Previous, out: &mut dyn Write) -> io::Result<()> {
    let what = match previous {
        Previous::Gone => "gone".to_owned(),
        Previous::Newer => "newer".to_owned(),
        Previous::Exited { waited } => format!("exited after {} ms", waited.as_millis()),
        Previous::Ended => "ended".to_owned(),
        Previous::Lingering(Linger::NotALauncher) => "lingering notALauncher".to_owned(),
        Previous::Lingering(Linger::NoRight) => "lingering noRight".to_owned(),
        Previous::Unknown => "unknown".to_owned(),
    };
    written(out, &text_report(vec![("previous", what)]))
}

/// Writes to `out` what a sweep of the retired launcher copies did, `swept`, as the line
/// `retired: removed <n>, kept <k>`, how many copies it removed and how many it left; or
/// `retired: lockBusy` when another job held the installation's lock, so that it looked at none.
fn print_retired(swept: &RetiredSweep, out: &mut dyn Write) -> io::Result<()> {
    let what = if swept.lock_busy {
        "lockBusy".to_owned()
    } else {
        format!("removed {}, kept {}", swept.removed.len(), swept.kept.len())
    };
    written(out, &text_report(vec![("retired", what)]))
}

/// Runs `job` on a thread of its own, handing it a progress that prints, and writes each line that
/// progress gives to `out` as it comes, so that whoever runs the program sees a hold while the job
/// waits in it. Gives back what the job gave, and whether every line was written: a line that
/// cannot be written stops the writing, never the job, which runs to its end. A panic in the job
/// goes on in the caller.
fn printing<T: Send>(
    out: &mut dyn Write,
    job: impl FnOnce(&dyn Progress) -> T + Send,
) -> (T, io::Result<()>) {
    let (lines, heard) = mpsc::channel();
    thread::scope(|scope| {
        let running = scope.spawn(move || {
            let progress = Printed {
                lines,
                shown: Mutex::new(None),
            };
            job(&progress)
        });
        // The lines end once the job has ended and dropped its progress, a panic included.
        let mut shown = Ok(());
        for line in heard {
            if shown.is_ok() {
                shown = written(out, &line);
            }
        }
        match running.join() {
            Ok(answer) => (answer, shown),
            Err(panicked) => panic::resume_unwind(panicked),
        }
    })
}

/// A job's progress as the headless modes write it: a line as each phase begins, one each time the
/// phase's share done passes a tenth, and one as a hold begins and as it ends, each sent, whole and
/// in the order the job told them, to the thread that writes the mode's report.
struct Printed {
    /// Where the lines go.
    lines: mpsc::Sender<String>,
    /// The phase begun last, and the share of it done, in percent, that its last progress line
    /// told, a whole tenth; `None` before the first phase.
    shown: Mutex<Option<(PhaseId, u64)>>,
}

impl Printed {
    /// Sends `line` to be written. The thread that writes it outlives the job, so it is always
    /// there to take it.
    fn send(&self, line: String) {
        let _ = self.lines.send(line);
    }
}

impl Progress for Printed {
    fn phase(&self, phase: PhaseId) {
        let mut shown = self.shown.lock().unwrap_or_else(PoisonError::into_inner);
        *shown = Some((phase, 0));
        self.send(format!("phase: {}\n", phase.0));
    }

    /// Writes a line each time the phase's share done passes a tenth, the share it reached in
    /// whole percent, at most all of it. A count that starts over, as a download tried again does,
    /// is told again as it passes each tenth; work of nothing, and work told before any phase,
    /// writes nothing.
    fn advance(&self, done: u64, total: u64, _: Option<&RelPath>) {
        let Some(percent) = share_done(done, total) else {
            return;
        };
        let mut shown = self.shown.lock().unwrap_or_else(PoisonError::into_inner);
        let Some((phase, told)) = shown.as_mut() else {
            return;
        };
        let tenth = percent - percent % PROGRESS_STEP_PERCENT;
        if tenth > *told {
            self.send(format!("progress: {} {percent}%\n", phase.0));
        }
        *told = tenth;
    }

    fn hold(&self, hold: Hold) {
        self.send(format!("hold: {}\n", codes::hold(hold)));
    }

    fn resumed(&self) {
        self.send("resumed\n".to_owned());
    }
}

/// The share `done` is of `total`, in whole percent, at most all of it; `None` for a total of
/// nothing, which has no share.
fn share_done(done: u64, total: u64) -> Option<u64> {
    let whole = u128::from(WHOLE_PERCENT);
    let share = (u128::from(done) * whole).checked_div(u128::from(total))?;
    u64::try_from(share.min(whole)).ok()
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
    use std::sync::mpsc;
    use std::thread;
    use std::time::Duration;

    use gdl_extract::Expansion;
    use gdl_files::{FolderProbe, Hold, PAYLOAD_NAMES, PhaseId, RelPath, root_utf16_len};
    use gdl_fixtures::{
        DiscSet, Edc, InstallFixture, RawEntry, RuntimeFiles, append_sections, backdate,
        folder_names, make_alpha_markers, pretty_json, snapshot, tiny_elf,
    };
    use gdl_install::{
        Blocker, ComponentAttributes, DEFAULT_CONFIG, DOWNLOAD_PREFIX, DestinationReport,
        FileChanges, GUNE5D, InstallFolder, JOURNAL_DIR, KnownFolder, LOCK_FILE, Linger, Previous,
        Probed, RECEIPT_FILE, RETIRED_LAUNCHER_PREFIX, RUNTIME_SECTION, Receipt, Recovered,
        RecoveryOutcome, RetiredCopy, RetiredSweep, STATE_DIR, Sections, SectionsError,
        SourceSummary, UPDATE_BACKUP, UPDATE_JOURNAL, UPDATE_STAGING_PREFIX, UpdateNotice,
        UpdatePlan, UpdateRecovery, Updated, Warning, installed_launcher_name,
    };
    use gdl_media::Sha256Digest;
    use gdl_release::{
        Asset, Platform, REPOSITORY, Release, ReleaseError, Scan, Version, checksums_asset,
        launcher_asset, signature_asset,
    };
    use serde_json::{Value, json};

    use super::{
        Build, EXIT_OK, EXIT_REFUSED, EXIT_USAGE, FolderView, Mode, OwnFile, SPACE_MARGIN,
        SourceKind, USAGE, after_update, feed_checked, hands_over, install_names, json_report,
        mode_of, print_plan, print_previous, print_recovered, print_retired, print_scan,
        print_updated, printing, recovery_io, refuse, run, text_value, usage, was_installed_copy,
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
            (&["--recover"], "--recover without a folder"),
            (&["--recover", ""], "--recover with an empty folder"),
            (&["--recover", "--json"], "--recover followed by an option"),
            (&["--json", "--recover"], "--recover at the end"),
            (&["--recover", "folder", "other"], "a second folder"),
            (
                &["--recover", "folder", "--json"],
                "--json beside --recover",
            ),
            (
                &["--recover", "folder", "--payload", "a.zip"],
                "--payload beside --recover",
            ),
            (
                &["--recover", "folder", "--output", "out"],
                "--output beside --recover",
            ),
            (
                &["--recover", "folder", "--strict", "script"],
                "--strict beside --recover",
            ),
            (&["--recover", "a", "--recover", "b"], "--recover twice"),
            (&["--recover", "folder", "--version"], "two modes"),
            (&["--inspect", "folder", "--recover", "folder"], "two modes"),
            // Each refused before anything is read, since no test reads the real feed.
            (&["--check-feed", "extra"], "an argument no option takes"),
            (&["--check-feed", "--json"], "--json beside --check-feed"),
            (
                &["--check-feed", "--payload", "a.zip"],
                "--payload beside --check-feed",
            ),
            (
                &["--check-feed", "--output", "out"],
                "--output beside --check-feed",
            ),
            (
                &["--check-feed", "--strict", "script"],
                "--strict beside --check-feed",
            ),
            (&["--check-feed", "--check-feed"], "--check-feed twice"),
            (&["--check-feed", "--version"], "two modes"),
            (&["--check-feed", "--inspect", "folder"], "two modes"),
            (&["--recover", "folder", "--check-feed"], "two modes"),
            (&["--update"], "--update without a folder"),
            (&["--update", ""], "--update with an empty folder"),
            (&["--update", "--yes"], "--update followed by an option"),
            (&["--yes", "--update"], "--update at the end"),
            (&["--update", "folder", "other"], "a second folder"),
            (&["--update", "a", "--update", "b"], "--update twice"),
            (&["--update", "folder", "--yes", "--yes"], "--yes twice"),
            (
                &["--update", "folder", "--offline", "--offline"],
                "--offline twice",
            ),
            (&["--update", "folder", "--yes=no"], "--yes with a value"),
            (&["--update", "folder", "--json"], "--json beside --update"),
            (
                &["--update", "folder", "--payload", "a.zip"],
                "--payload beside --update",
            ),
            (
                &["--update", "folder", "--output", "out"],
                "--output beside --update",
            ),
            (
                &["--update", "folder", "--strict", "script"],
                "--strict beside --update",
            ),
            (&["--update", "folder", "--recover", "folder"], "two modes"),
            (&["--update", "folder", "--check-feed"], "two modes"),
            (&["--version", "--yes"], "--yes outside --update"),
            (&["--recover", "folder", "--yes"], "--yes beside --recover"),
            (
                &["--inspect", "folder", "--offline"],
                "--offline outside --update",
            ),
            (
                &["--check-feed", "--offline"],
                "--offline beside --check-feed",
            ),
            (&["--check-feed", "--yes"], "--yes beside --check-feed"),
            // Each refused before any process is looked at.
            (&["--after-update"], "--after-update without a process id"),
            (&["--after-update", ""], "--after-update with an empty id"),
            (
                &["--after-update", "--yes"],
                "--after-update followed by an option",
            ),
            (&["--yes", "--after-update"], "--after-update at the end"),
            (&["--after-update", "0"], "a process id of nought"),
            (&["--after-update", "00"], "a process id of nought"),
            (&["--after-update", "-1"], "a negative process id"),
            (&["--after-update", "+1"], "a process id with a sign"),
            (&["--after-update", " 1"], "a process id after a space"),
            (&["--after-update", "1 "], "a process id before a space"),
            (
                &["--after-update", "1.5"],
                "a process id that is no whole number",
            ),
            (&["--after-update", "0x10"], "a process id in hexadecimal"),
            (&["--after-update", "１"], "a process id in other digits"),
            (&["--after-update", "pid"], "a process id that is no number"),
            (
                &["--after-update", "4294967296"],
                "a process id past what an id holds",
            ),
            (&["--after-update", "1", "2"], "a second process id"),
            (
                &["--after-update", "1", "--after-update", "2"],
                "--after-update twice",
            ),
            (
                &["--after-update", "1", "--json"],
                "--json beside --after-update",
            ),
            (
                &["--after-update", "1", "--yes"],
                "--yes beside --after-update",
            ),
            (
                &["--after-update", "1", "--offline"],
                "--offline beside --after-update",
            ),
            (
                &["--after-update", "1", "--payload", "a.zip"],
                "--payload beside --after-update",
            ),
            (
                &["--after-update", "1", "--output", "out"],
                "--output beside --after-update",
            ),
            (
                &["--after-update", "1", "--strict", "script"],
                "--strict beside --after-update",
            ),
            (&["--after-update", "1", "--version"], "two modes"),
            (&["--after-update", "1", "--recover", "folder"], "two modes"),
            (&["--update", "folder", "--after-update", "1"], "two modes"),
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
             --output <dir> [--strict script] | --check-feed | --update <dir> [--yes] \
             [--offline] | --recover <dir> | --after-update <pid> | --version"
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
            &["--recover=folder"],
            &["--Recover", "folder"],
            &["--check-feed=github"],
            &["--Check-Feed"],
            &["--update=folder"],
            &["--Update", "folder"],
            &["--yes"],
            &["--offline"],
            &["--yes", "--offline", "folder"],
            &["--after-update=1"],
            &["--After-Update", "1"],
            &["1234"],
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
        // A folder with nothing to recover, whose report is lost.
        let recovering = [OsString::from("--recover"), dir.path().into()];
        assert_eq!(
            run(&recovering, &RELEASE, &mut own, &mut Closed, &mut err),
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

    /// How long the hold test's other job keeps the lock at most, waiting for the recovery to write
    /// that it holds: far longer than the recovery takes to look at the lock.
    const HOLD_SEEN_LIMIT: Duration = Duration::from_secs(60);

    /// The release an update the tests left unfinished updated from.
    const PREVIOUS: &str = "0.9.0";

    /// A recovery that found nothing to do.
    fn nothing_recovered() -> Recovered {
        Recovered {
            install: RecoveryOutcome::Nothing,
            update: UpdateRecovery::Nothing,
            swept: Vec::new(),
            left: Vec::new(),
        }
    }

    /// What `print_recovered` writes of `recovered`.
    fn printed(recovered: &Recovered) -> String {
        let mut out = Vec::new();
        print_recovered(recovered, &mut out).unwrap();
        String::from_utf8(out).unwrap()
    }

    /// Recovers the folder at `root` as the release build, whose own file carries no sections.
    fn recovered(root: &Path) -> Ran {
        run_release(&[OsStr::new("--recover"), root.as_os_str()])
    }

    /// The name an update from `from`, staged in the tests' staging, gives the launcher copy it
    /// moves aside on `platform`.
    fn retired_copy(platform: Platform, from: &str) -> String {
        let suffix = match platform {
            Platform::WindowsX64 => ".exe",
            Platform::LinuxX64 => "",
        };
        format!("{RETIRED_LAUNCHER_PREFIX}{from}-{STAGING_RANDOM}{suffix}")
    }

    /// An installation of the standard runtime of `version` for `platform` in a folder of its own
    /// in `dir`, and its state folder.
    fn installation(dir: &Path, platform: Platform, version: &str) -> (InstallFixture, PathBuf) {
        let runtime = RuntimeFiles::standard(platform.as_str(), version);
        let fixture = InstallFixture::write(&dir.join("install"), runtime).unwrap();
        let state = fixture.root.join(STATE_DIR);
        (fixture, state)
    }

    /// A stream that keeps what is written to it, and tells `seen` once what it holds first holds
    /// `watched`.
    struct Watched {
        written: Vec<u8>,
        watched: &'static [u8],
        seen: Option<mpsc::Sender<()>>,
    }

    impl Write for Watched {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
            self.written.extend_from_slice(bytes);
            let watched = self.watched;
            if self
                .written
                .windows(watched.len())
                .any(|part| part == watched)
                && let Some(seen) = self.seen.take()
            {
                let _ = seen.send(());
            }
            Ok(bytes.len())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn a_recovery_is_printed_outcome_by_outcome() {
        let fixture = InstallFixture::new(
            Path::new("install"),
            RuntimeFiles::standard(Platform::LinuxX64.as_str(), VERSION),
        );
        let receipt: Receipt = serde_json::from_value(fixture.receipt()).unwrap();
        let installs = [
            (RecoveryOutcome::Nothing, "nothing"),
            (
                RecoveryOutcome::RolledBack {
                    kept: vec!["saves".to_owned()],
                },
                "rolledBack",
            ),
            (
                RecoveryOutcome::Finished {
                    receipt: Box::new(receipt),
                },
                "finished",
            ),
            (RecoveryOutcome::CleanedUp, "cleanedUp"),
            (RecoveryOutcome::AbandonedRemoved, "abandonedRemoved"),
        ];
        let updates = [
            (UpdateRecovery::Nothing, "nothing"),
            (
                UpdateRecovery::RolledBack {
                    kept: vec!["lib".to_owned()],
                    launcher_restored: true,
                },
                "rolledBack",
            ),
            (
                UpdateRecovery::RolledBack {
                    kept: Vec::new(),
                    launcher_restored: false,
                },
                "rolledBack",
            ),
            (UpdateRecovery::Finished, "finished"),
            (UpdateRecovery::CleanedUp, "cleanedUp"),
            (
                UpdateRecovery::CleanupLeft {
                    detail: "held".into(),
                },
                "cleanupLeft",
            ),
        ];
        for (install, install_word) in &installs {
            for (update, update_word) in &updates {
                let recovered = Recovered {
                    install: install.clone(),
                    update: update.clone(),
                    ..nothing_recovered()
                };
                assert_eq!(
                    printed(&recovered),
                    format!("install: {install_word}\nupdate: {update_word}\n")
                );
            }
        }

        // The leftovers removed, then those kept, each in the byte order the recovery gives them;
        // a name that would begin a line of its own is escaped.
        let leftovers = Recovered {
            swept: vec![
                "download-AbC123xy".to_owned(),
                "journal/update.json.pending".to_owned(),
                "stage-AbC123xy".to_owned(),
            ],
            left: vec![
                "retired-launcher-0.9.0-AbC123xy.exe".to_owned(),
                "update-backup".to_owned(),
                "x\nupdate: finished".to_owned(),
            ],
            ..nothing_recovered()
        };
        assert_eq!(
            printed(&leftovers),
            "install: nothing\nupdate: nothing\nswept: download-AbC123xy\n\
             swept: journal/update.json.pending\nswept: stage-AbC123xy\n\
             left: retired-launcher-0.9.0-AbC123xy.exe\nleft: update-backup\n\
             left: x\\nupdate: finished\n"
        );
        assert!(print_recovered(&nothing_recovered(), &mut Closed).is_err());
    }

    #[test]
    fn a_jobs_progress_is_printed_as_it_is_told() {
        let mut out = Vec::new();
        let (answer, shown) = printing(&mut out, |progress| {
            // Work told before any phase has no phase to name.
            progress.advance(50, 100, None);
            progress.phase(PhaseId("first"));
            for done in [0, 9, 10, 15, 19, 20, 55, 100, 120] {
                progress.advance(done, 100, None);
            }
            // Work of nothing has no share.
            progress.advance(0, 0, None);
            progress.hold(Hold::LockBusy);
            progress.resumed();
            // A new phase starts from nothing, and a count that starts over is told again as it
            // passes each tenth.
            progress.phase(PhaseId("second"));
            for done in [500, 3, 300, 1000] {
                progress.advance(done, 1000, None);
            }
            progress.hold(Hold::GameRunning);
            progress.resumed();
            // Counts as large as a count can be.
            progress.phase(PhaseId("large"));
            progress.advance(u64::MAX / 2, u64::MAX, None);
            progress.advance(u64::MAX, u64::MAX, None);
            progress.advance(u64::MAX, 1, None);
            "the job's answer"
        });
        assert_eq!(answer, "the job's answer");
        shown.unwrap();
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "phase: first\nprogress: first 10%\nprogress: first 20%\nprogress: first 55%\n\
             progress: first 100%\nhold: lock.busy\nresumed\nphase: second\n\
             progress: second 50%\nprogress: second 30%\nprogress: second 100%\n\
             hold: game.running\nresumed\nphase: large\nprogress: large 49%\n\
             progress: large 100%\n"
        );

        // A stream that takes no line stops the writing, not the job.
        let (answer, shown) = printing(&mut Closed, |progress| {
            progress.phase(PhaseId("first"));
            progress.hold(Hold::LockBusy);
            7
        });
        assert_eq!(answer, 7);
        assert!(shown.is_err());
    }

    #[test]
    #[should_panic(expected = "the job's own panic")]
    fn a_panic_in_a_printed_job_goes_on_in_its_caller() {
        let _ = printing(&mut Vec::new(), |progress| -> u8 {
            progress.phase(PhaseId("first"));
            panic!("the job's own panic")
        });
    }

    #[test]
    fn a_recovery_hands_over_only_when_it_put_the_copy_back() {
        let rolled_back = |launcher_restored| Recovered {
            update: UpdateRecovery::RolledBack {
                kept: Vec::new(),
                launcher_restored,
            },
            ..nothing_recovered()
        };
        // The copy put back where the program's own file was: the program runs from a retired
        // name, and the copy takes over.
        assert!(hands_over(true, &rolled_back(true)));
        // An update that found no copy to move aside put none back, and there is nothing to hand
        // over to.
        assert!(!hands_over(true, &rolled_back(false)));
        // A program that was no installed copy runs from where it ran before.
        assert!(!hands_over(false, &rolled_back(true)));
        // An update finished or cleaned up leaves the copy that ran it where it was.
        for update in [
            UpdateRecovery::Nothing,
            UpdateRecovery::Finished,
            UpdateRecovery::CleanedUp,
            UpdateRecovery::CleanupLeft {
                detail: "held".into(),
            },
        ] {
            let recovered = Recovered {
                update,
                ..nothing_recovered()
            };
            assert!(!hands_over(true, &recovered), "{recovered:?}");
        }
        // A fresh install's recovery never hands over.
        let install = Recovered {
            install: RecoveryOutcome::RolledBack { kept: Vec::new() },
            ..nothing_recovered()
        };
        assert!(!hands_over(true, &install));
    }

    #[test]
    fn the_installed_copy_is_told_by_the_programs_own_file() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("install");
        fs::create_dir(&root).unwrap();
        // A launcher file carrying a runtime at the copy's name, and its bytes in another file.
        let mut file = tiny_elf();
        append_sections(&mut file, &[(RUNTIME_SECTION, b"runtime")], 0);
        let copy = root.join(installed_launcher_name(platform));
        fs::write(&copy, &file).unwrap();
        let downloaded = dir.path().join("downloaded");
        fs::write(&downloaded, &file).unwrap();

        let told = |own: &OwnFile, root: &Path, platform| {
            was_installed_copy(own, root, platform, recovery_io).unwrap()
        };
        let installed: OwnFile = Sections::of_path(&copy);
        assert!(told(&installed, &root, Some(platform)));
        let other: OwnFile = Sections::of_path(&downloaded);
        assert!(!told(&other, &root, Some(platform)));
        // An installation without a copy has none for the file to be.
        let empty = dir.path().join("empty");
        fs::create_dir(&empty).unwrap();
        assert!(!told(&installed, &empty, Some(platform)));
        assert!(!told(
            &installed,
            &dir.path().join("absent"),
            Some(platform)
        ));
        // A file whose sections were not found, and a system no release is built for.
        assert!(!told(&Ok(None), &root, Some(platform)));
        let unread: OwnFile = Err(SectionsError::NoSections);
        assert!(!told(&unread, &root, Some(platform)));
        assert!(!told(&installed, &root, None));
    }

    #[test]
    fn a_folder_with_nothing_to_recover_is_left_as_it_was() {
        let dir = tempfile::tempdir().unwrap();
        let absent = dir.path().join("absent");
        let ran = recovered(&absent);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("install: nothing\nupdate: nothing\n", "")
        );
        assert!(!absent.exists());

        // An installation that holds nothing to recover is looked at without the lock, so not
        // even the lock's file is made.
        let Some(platform) = this_platform() else {
            return;
        };
        let (fixture, _) = installation(dir.path(), platform, VERSION);
        backdate(&fixture.root).unwrap();
        let before = snapshot(&fixture.root).unwrap();
        let ran = recovered(&fixture.root);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("install: nothing\nupdate: nothing\n", "")
        );
        assert!(
            snapshot(&fixture.root).unwrap() == before,
            "the folder changed"
        );
    }

    #[test]
    fn a_recovery_sweeps_what_jobs_left_and_names_each() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, VERSION);
        let mut kept = folder_names(&state).unwrap();
        let staging = format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}");
        let download = format!("{DOWNLOAD_PREFIX}{STAGING_RANDOM}");
        let retired = retired_copy(platform, PREVIOUS);
        fs::create_dir_all(state.join(&staging).join("data")).unwrap();
        fs::write(state.join(&staging).join(DEFAULT_CONFIG), b"{}").unwrap();
        fs::create_dir(state.join(&download)).unwrap();
        fs::create_dir(state.join(UPDATE_BACKUP)).unwrap();
        fs::write(state.join(UPDATE_BACKUP).join("VERSION"), PREVIOUS).unwrap();
        fs::write(state.join(&retired), b"an older launcher").unwrap();
        // Names the launcher's rules do not make there, which stay.
        let others = ["notes.txt", "stage-n64-AbC123xy"];
        fs::write(state.join(others[0]), b"").unwrap();
        fs::create_dir(state.join(others[1])).unwrap();

        let ran = recovered(&fixture.root);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.err, "");
        assert_eq!(
            ran.out,
            format!(
                "phase: finishCleanup\ninstall: nothing\nupdate: nothing\nswept: {download}\n\
                 swept: {retired}\nswept: {staging}\nswept: {UPDATE_BACKUP}\n"
            )
        );
        // What the fixture laid out, the names that are not the launcher's, and the lock the
        // recovery took.
        kept.extend(others.map(str::to_owned));
        kept.push(LOCK_FILE.to_owned());
        kept.sort();
        assert_eq!(folder_names(&state).unwrap(), kept);
    }

    /// Lays out in the installation `fixture`, for `platform`, what an update from [`PREVIOUS`] in
    /// `phase` leaves: its staging, its backup and the retired launcher copy, and its journal,
    /// whose path it gives back with the staging's name and the retired copy's.
    fn update_left(
        fixture: &InstallFixture,
        platform: Platform,
        phase: &str,
    ) -> (PathBuf, String, String) {
        let state = fixture.root.join(STATE_DIR);
        let staging = format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}");
        let retired = retired_copy(platform, PREVIOUS);
        fs::create_dir(state.join(&staging)).unwrap();
        fs::create_dir(state.join(UPDATE_BACKUP)).unwrap();
        fs::write(state.join(UPDATE_BACKUP).join("VERSION"), PREVIOUS).unwrap();
        fs::write(state.join(&retired), b"an older launcher").unwrap();
        let journal = json!({
            "schema": 1,
            "minLauncher": VERSION,
            "phase": phase,
            "from": PREVIOUS,
            "to": VERSION,
            "staging": staging,
            "backup": ["VERSION", "build-info.json"],
            "publish": ["VERSION", "build-info.json"],
            "madeFolders": [],
            "launcher": installed_launcher_name(platform),
            "retired": retired,
            "receipt": fixture.receipt(),
        });
        fs::create_dir(state.join(JOURNAL_DIR)).unwrap();
        let journal_path = state.join(JOURNAL_DIR).join(UPDATE_JOURNAL);
        fs::write(&journal_path, pretty_json(&journal)).unwrap();
        (journal_path, staging, retired)
    }

    #[test]
    fn a_committed_update_is_cleaned_up() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, VERSION);
        let (journal_path, staging, retired) = update_left(&fixture, platform, "committed");

        let ran = recovered(&fixture.root);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.err, "");
        assert_eq!(
            ran.out,
            "phase: recoverUpdate\nphase: finishCleanup\ninstall: nothing\nupdate: cleanedUp\n"
        );
        for gone in [staging.as_str(), UPDATE_BACKUP, retired.as_str()] {
            assert!(!state.join(gone).exists(), "{gone}");
        }
        assert!(!journal_path.exists());
        assert_eq!(
            inspected(&fixture.root, false).lines().next(),
            Some("state: installed")
        );
    }

    #[test]
    fn an_installation_of_a_later_release_is_refused() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, "1.0.1");
        let staging = state.join(format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}"));
        fs::create_dir(&staging).unwrap();
        assert_refused(
            &recovered(&fixture.root),
            "error: launcher.older (olderLauncher): This launcher file (1.0.0-beta) is older than \
             the installation (1.0.1)\n",
        );
        assert!(staging.is_dir(), "the staging was removed");
    }

    #[test]
    fn a_recovery_writes_its_hold_while_it_waits() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, VERSION);
        let staging = format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}");
        fs::create_dir(state.join(&staging)).unwrap();
        // Another job holds the lock until the recovery has written that it waits for it, so a
        // hold written only once the recovery ended would keep it waiting to the limit.
        let (taken, lock_taken) = mpsc::channel();
        let (seen, hold_seen) = mpsc::channel();
        let root = fixture.root.clone();
        let holder = thread::spawn(move || {
            let lock = InstallFolder::at(&root).unwrap().lock().unwrap();
            taken.send(()).unwrap();
            let in_time = hold_seen.recv_timeout(HOLD_SEEN_LIMIT).is_ok();
            drop(lock);
            in_time
        });
        lock_taken.recv().unwrap();
        let mut out = Watched {
            written: Vec::new(),
            watched: b"hold: lock.busy\n",
            seen: Some(seen),
        };
        let mut err = Vec::new();
        let args = [OsString::from("--recover"), fixture.root.clone().into()];
        let code = run(&args, &RELEASE, &mut Ok(None), &mut out, &mut err);
        assert!(
            holder.join().unwrap(),
            "the hold was not written while it lasted"
        );
        assert_eq!(code, Some(EXIT_OK), "{}", String::from_utf8_lossy(&err));
        assert_eq!(
            String::from_utf8(out.written).unwrap(),
            format!(
                "hold: lock.busy\nresumed\nphase: finishCleanup\ninstall: nothing\n\
                 update: nothing\nswept: {staging}\n"
            )
        );
    }

    /// How many bytes a SHA-256 digest holds.
    const DIGEST_BYTES: usize = 32;

    /// The size of the launcher file of the releases the feed check's tests report.
    const LAUNCHER_SIZE: u64 = 7_340_032;

    /// The size of their checksum lists.
    const LIST_SIZE: u64 = 300;

    /// The size of their signatures.
    const SIGNATURE_SIZE: u64 = 380;

    /// The file `name` of the release of `version`, as the feed lists it: `size` bytes, whose
    /// SHA-256 is the byte `byte` throughout.
    fn feed_file(version: &Version, name: String, size: u64, byte: u8) -> Asset {
        Asset {
            url: format!("https://github.com/{REPOSITORY}/releases/download/v{version}/{name}"),
            name,
            size,
            sha256: Sha256Digest([byte; DIGEST_BYTES]),
        }
    }

    /// The feed's release of `text` for `platform`, whose checksum list's signature it carries
    /// when `signed` says so.
    fn feed_release(text: &str, platform: Platform, signed: bool) -> Release {
        let version = Version::parse(text).unwrap();
        let signature = signed.then(|| {
            let name = signature_asset(&version, platform);
            feed_file(&version, name, SIGNATURE_SIZE, 0xC3)
        });
        Release {
            tag: format!("v{text}"),
            platform,
            launcher: feed_file(
                &version,
                launcher_asset(&version, platform),
                LAUNCHER_SIZE,
                0xA1,
            ),
            checksums: feed_file(
                &version,
                checksums_asset(&version, platform),
                LIST_SIZE,
                0xB2,
            ),
            signature,
            notes: Some("Added\n\n- A line of notes".to_owned()),
            version,
        }
    }

    /// A read of the feed that found `candidate` among twelve releases, ten of them passed over.
    fn scan_of(candidate: Option<Release>) -> Scan {
        Scan {
            candidate,
            rows: 12,
            passed_over: 10,
        }
    }

    /// What `print_scan` writes of `scan` for the release build on `platform`.
    fn scanned(scan: &Scan, platform: Platform) -> String {
        let mut out = Vec::new();
        let running = Version::parse(VERSION).unwrap();
        print_scan(scan, &running, platform, &mut out).unwrap();
        String::from_utf8(out).unwrap()
    }

    /// What the release build's feed check reports of the feed's answer `scanned` on Windows.
    fn feed_check(scanned: Result<Scan, ReleaseError>) -> Ran {
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let running = Version::parse(VERSION).unwrap();
        let code = feed_checked(scanned, &running, Platform::WindowsX64, &mut out, &mut err);
        Ran {
            code: Some(code),
            out: String::from_utf8(out).unwrap(),
            err: String::from_utf8(err).unwrap(),
        }
    }

    #[test]
    fn the_feed_check_takes_no_option() {
        // Read as the mode alone and never run, since running it reads the real feed.
        let args = [OsString::from("--check-feed")];
        assert!(
            matches!(mode_of(&args), Some(Ok(Mode::CheckFeed))),
            "{:?}",
            mode_of(&args)
        );
    }

    #[test]
    fn a_feed_check_is_printed_line_by_line() {
        // The alpha releases alone, as the feed holds them before the launcher's first release.
        let alpha = Scan {
            candidate: None,
            rows: 10,
            passed_over: 10,
        };
        assert_eq!(
            scanned(&alpha, Platform::WindowsX64),
            "platform: windows-x64\nrunning: 1.0.0-beta\nreleases: 10\npassedOver: 10\n\
             newest: none\n"
        );

        // A newer release, signed, on each platform.
        let [launcher, list, signature] = ["a1", "b2", "c3"].map(|byte| byte.repeat(DIGEST_BYTES));
        assert_eq!(
            scanned(
                &scan_of(Some(feed_release(NEXT, Platform::WindowsX64, true))),
                Platform::WindowsX64
            ),
            format!(
                "platform: windows-x64\nrunning: 1.0.0-beta\nreleases: 12\npassedOver: 10\n\
                 newest: 1.0.0-beta.1\nnewer: yes\n\
                 launcher: GauntletDarkLegacy-1.0.0-beta.1-windows-x64-launcher.exe 7340032 \
                 {launcher}\n\
                 checksums: GauntletDarkLegacy-1.0.0-beta.1-windows-x64.sha256 300 {list}\n\
                 signature: GauntletDarkLegacy-1.0.0-beta.1-windows-x64.sha256.sig 380 \
                 {signature}\n"
            )
        );
        assert_eq!(
            scanned(
                &scan_of(Some(feed_release(NEXT, Platform::LinuxX64, true))),
                Platform::LinuxX64
            ),
            format!(
                "platform: linux-x64\nrunning: 1.0.0-beta\nreleases: 12\npassedOver: 10\n\
                 newest: 1.0.0-beta.1\nnewer: yes\n\
                 launcher: GauntletDarkLegacy-1.0.0-beta.1-linux-x64-launcher.tar.gz 7340032 \
                 {launcher}\n\
                 checksums: GauntletDarkLegacy-1.0.0-beta.1-linux-x64.sha256 300 {list}\n\
                 signature: GauntletDarkLegacy-1.0.0-beta.1-linux-x64.sha256.sig 380 \
                 {signature}\n"
            )
        );

        // Read with no floor, the newest release may be no newer than the build: its own release,
        // another build of it, or an older one. One that carries no signature names none.
        for newest in [VERSION, "1.0.0-beta+rebuilt.2", "1.0.0-alpha.9"] {
            let release = feed_release(newest, Platform::WindowsX64, false);
            let (launcher_name, list_name) = (
                release.launcher.name.clone(),
                release.checksums.name.clone(),
            );
            assert_eq!(
                scanned(&scan_of(Some(release)), Platform::WindowsX64),
                format!(
                    "platform: windows-x64\nrunning: 1.0.0-beta\nreleases: 12\npassedOver: 10\n\
                     newest: {newest}\nnewer: no\nlauncher: {launcher_name} 7340032 {launcher}\n\
                     checksums: {list_name} 300 {list}\nsignature: none\n"
                ),
                "{newest}"
            );
        }

        // A report that cannot be written is an error.
        let running = Version::parse(VERSION).unwrap();
        assert!(print_scan(&alpha, &running, Platform::WindowsX64, &mut Closed).is_err());
    }

    #[test]
    fn a_feed_that_cannot_be_read_is_a_refusal_line() {
        let incomplete = launcher_asset(&Version::parse(NEXT).unwrap(), Platform::WindowsX64);
        let page =
            format!("https://api.github.com/repos/{REPOSITORY}/releases?per_page=100&page=1");
        let cases = [
            (
                ReleaseError::Offline {
                    detail: "connection refused".into(),
                },
                "error: net.offline (offline): The release feed could not be reached: connection \
                 refused\n"
                    .to_owned(),
            ),
            (
                ReleaseError::Status {
                    status: 502,
                    url: page.as_str().into(),
                },
                format!("error: net.failed (status): GitHub answered HTTP 502 for {page}\n"),
            ),
            (
                ReleaseError::RateLimited {
                    reset_at: Some(1_800_000_000),
                },
                "error: net.rateLimited (rateLimited): GitHub's limit of unauthenticated requests \
                 is spent until 1800000000 (seconds since the Unix epoch)\n"
                    .to_owned(),
            ),
            (
                ReleaseError::RateLimited { reset_at: None },
                "error: net.rateLimited (rateLimited): GitHub's limit of unauthenticated requests \
                 is spent, with no time given for its reset\n"
                    .to_owned(),
            ),
            (
                ReleaseError::Incomplete {
                    asset: incomplete.as_str().into(),
                },
                format!(
                    "error: release.incomplete (incomplete): Release asset is incomplete or lacks \
                     its GitHub SHA-256 digest: {incomplete}\n"
                ),
            ),
            // A detail that holds a line end stays one line.
            (
                ReleaseError::InvalidResponse {
                    detail: "expected value\nat line 2".into(),
                },
                "error: net.failed (invalidResponse): Invalid GitHub release response: expected \
                 value\\nat line 2\n"
                    .to_owned(),
            ),
            (ReleaseError::Cancelled, "error: cancelled\n".to_owned()),
        ];
        for (error, line) in cases {
            assert_refused(&feed_check(Err(error)), &line);
        }

        // What the feed holds is reported whole, with nothing on the error stream.
        let found = scan_of(Some(feed_release(NEXT, Platform::WindowsX64, false)));
        let ran = feed_check(Ok(found.clone()));
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.out, scanned(&found, Platform::WindowsX64));
        assert_eq!(ran.err, "");
        // A report nobody saw fails the run.
        let running = Version::parse(VERSION).unwrap();
        let code = feed_checked(
            Ok(found),
            &running,
            Platform::WindowsX64,
            &mut Closed,
            &mut Vec::new(),
        );
        assert_eq!(code, EXIT_REFUSED);
    }

    /// A launcher file in `dir` carrying `runtime`, as a release appends one to the launcher's
    /// program, and its path.
    fn launcher_carrying(dir: &Path, runtime: &RuntimeFiles) -> PathBuf {
        let mut file = tiny_elf();
        append_sections(&mut file, &[(RUNTIME_SECTION, runtime.zip().as_slice())], 0);
        let path = dir.join("launcher");
        fs::write(&path, &file).unwrap();
        path
    }

    /// Updates the installation at `root` without asking a feed, as the release build whose own
    /// file is the launcher file at `launcher`; only plans the update unless `yes` says so.
    fn updated_offline(root: &Path, launcher: &Path, yes: bool) -> Ran {
        let mut own: OwnFile = Sections::of_path(launcher);
        run_as(&update_args(root, yes), &RELEASE, &mut own)
    }

    /// The arguments that update the installation at `root` without asking a feed, or only plan
    /// the update unless `yes` says so.
    fn update_args(root: &Path, yes: bool) -> Vec<&OsStr> {
        let mut args = vec![
            OsStr::new("--update"),
            root.as_os_str(),
            OsStr::new("--offline"),
        ];
        if yes {
            args.push(OsStr::new("--yes"));
        }
        args
    }

    /// The phases a run's output tells, in turn.
    fn phases_of(out: &str) -> Vec<&str> {
        out.lines()
            .filter_map(|line| line.strip_prefix("phase: "))
            .collect()
    }

    /// The lines of a run's output but those that tell its job's progress: its phases and their
    /// shares done.
    fn past_progress(out: &str) -> Vec<&str> {
        out.lines()
            .filter(|line| !line.starts_with("phase: ") && !line.starts_with("progress: "))
            .collect()
    }

    /// A runtime name.
    fn name(text: &str) -> RelPath {
        RelPath::parse(text, &PAYLOAD_NAMES).unwrap()
    }

    /// An update from [`PREVIOUS`] to [`VERSION`] of the installation in `dir` that stands, the
    /// launcher copy installed before ending as `retired`, the game probed as `probed` says, with
    /// `notices`.
    fn an_update(
        dir: &Path,
        retired: RetiredCopy,
        probed: Probed,
        notices: Vec<UpdateNotice>,
    ) -> Updated {
        let fixture = InstallFixture::new(
            dir,
            RuntimeFiles::standard(Platform::LinuxX64.as_str(), VERSION),
        );
        Updated {
            folder: InstallFolder::at(dir).unwrap(),
            from: Version::parse(PREVIOUS).unwrap(),
            to: Version::parse(VERSION).unwrap(),
            receipt: serde_json::from_value(fixture.receipt()).unwrap(),
            changes: FileChanges {
                replaced: vec![name("VERSION"), name("build-info.json")],
                added: Vec::new(),
                removed: Vec::new(),
                unchanged: 4,
            },
            recovered: nothing_recovered(),
            retired,
            probed,
            notices,
        }
    }

    /// What `print_updated` writes of `updated`, from the source `kind` names.
    fn printed_update(updated: &Updated, kind: SourceKind) -> String {
        let mut out = Vec::new();
        print_updated(updated, kind, &mut out).unwrap();
        String::from_utf8(out).unwrap()
    }

    /// A plan of the update from `from` to `to` from `source`, which would move `changes`.
    fn a_plan(
        from: &str,
        to: &str,
        source: SourceSummary,
        changes: Option<FileChanges>,
    ) -> UpdatePlan {
        UpdatePlan {
            root: PathBuf::from("install"),
            from: Version::parse(from).unwrap(),
            to: Version::parse(to).unwrap(),
            source,
            changes,
            needed_bytes: None,
            recovery_pending: false,
            cleanup_pending: false,
        }
    }

    /// What `print_plan` writes of `plan`.
    fn printed_plan(plan: &UpdatePlan) -> String {
        let mut out = Vec::new();
        print_plan(plan, &mut out).unwrap();
        String::from_utf8(out).unwrap()
    }

    #[test]
    fn an_update_takes_its_folder_and_each_of_its_options_once() {
        let cases: &[(&[&str], bool, bool)] = &[
            (&["--update", "dir"], false, false),
            (&["--update", "dir", "--yes"], true, false),
            (&["--offline", "--update", "dir"], false, true),
            (&["--yes", "--offline", "--update", "dir"], true, true),
            (&["--update", "dir", "--offline", "--yes"], true, true),
        ];
        for &(args, made, asks_no_feed) in cases {
            let args: Vec<OsString> = args.iter().map(OsString::from).collect();
            let mode = mode_of(&args);
            assert!(
                matches!(
                    mode,
                    Some(Ok(Mode::Update { folder, yes, offline }))
                        if folder == "dir" && yes == made && offline == asks_no_feed
                ),
                "{args:?}: {mode:?}"
            );
        }
    }

    #[test]
    fn an_update_is_printed_line_by_line() {
        let dir = tempfile::tempdir().unwrap();
        // From the program's own file, the game left as it was and the copy installed before gone.
        let plain = an_update(
            dir.path(),
            RetiredCopy::Removed,
            Probed::NotStaged,
            Vec::new(),
        );
        assert_eq!(
            printed_update(&plain, SourceKind::Bundled),
            "updated: 0.9.0 -> 1.0.0-beta (bundled)\nprobed: notStaged\nretired: removed\n"
        );

        // From the feed, the game probed, with each notice in turn and the copy left for later; a
        // value that would begin a line of its own is escaped.
        let retired = retired_copy(Platform::WindowsX64, PREVIOUS);
        let notices = vec![
            UpdateNotice::ProbeFailed {
                status: Some(3),
                first_line: Some("Gauntlet 0.9.0".to_owned()),
                stderr_tail: "failed\nprobed: notStaged".to_owned(),
            },
            UpdateNotice::CleanupLeft {
                detail: "update-backup: in use".into(),
            },
        ];
        let told = an_update(
            dir.path(),
            RetiredCopy::Left {
                name: retired.clone(),
            },
            Probed::Staged,
            notices,
        );
        assert_eq!(
            printed_update(&told, SourceKind::Feed),
            format!(
                "updated: 0.9.0 -> 1.0.0-beta (feed)\nprobed: staged\n\
                 notice: game.probeFailed (probeFailed): status 3; first line Gauntlet 0.9.0; \
                 stderr failed\\nprobed: notStaged\n\
                 notice: io.failed (cleanupLeft): update-backup: in use\n\
                 retired: left {retired}\n"
            )
        );

        // A game that never answered, and an installation that held no launcher copy to retire.
        let silent = vec![UpdateNotice::ProbeFailed {
            status: None,
            first_line: None,
            stderr_tail: String::new(),
        }];
        let unanswered = an_update(dir.path(), RetiredCopy::NotMoved, Probed::Staged, silent);
        assert_eq!(
            printed_update(&unanswered, SourceKind::Bundled),
            "updated: 0.9.0 -> 1.0.0-beta (bundled)\nprobed: staged\n\
             notice: game.probeFailed (probeFailed): status none; first line none; stderr none\n\
             retired: none\n"
        );
        assert!(print_updated(&plain, SourceKind::Bundled, &mut Closed).is_err());
    }

    #[test]
    fn a_plan_is_printed_line_by_line() {
        let bundled = SourceSummary::Bundled { file_len: 4096 };
        // The names the program's own file would move, each kind in byte order.
        let changes = FileChanges {
            replaced: vec![
                name("VERSION"),
                name("build-info.json"),
                name("data/text/en.json"),
            ],
            added: vec![name("shaders/next.spv")],
            removed: vec![name("shaders/test.spv")],
            unchanged: 3,
        };
        assert_eq!(
            printed_plan(&a_plan(PREVIOUS, VERSION, bundled.clone(), Some(changes))),
            "plan: 0.9.0 -> 1.0.0-beta (bundled)\nplan: replace VERSION\n\
             plan: replace build-info.json\nplan: replace data/text/en.json\n\
             plan: add shaders/next.spv\nplan: remove shaders/test.spv\n"
        );
        // While a job left unfinished keeps them from being known, no name is told.
        assert_eq!(
            printed_plan(&a_plan(PREVIOUS, VERSION, bundled.clone(), None)),
            "plan: 0.9.0 -> 1.0.0-beta (bundled)\n"
        );
        // A release is downloaded whole.
        let release = SourceSummary::Release {
            tag: format!("v{NEXT}"),
            download_bytes: 7_340_032,
        };
        assert_eq!(
            printed_plan(&a_plan(VERSION, NEXT, release.clone(), None)),
            "plan: 1.0.0-beta -> 1.0.0-beta.1 (feed)\nplan: download 7340032\n"
        );
        // A release no newer than the installed one, which another launcher may have installed
        // since the source was chosen, would leave the installation as it is.
        for (from, to, source) in [
            (VERSION, VERSION, bundled.clone()),
            (NEXT, VERSION, bundled),
            (NEXT, VERSION, release.clone()),
        ] {
            assert_eq!(
                printed_plan(&a_plan(from, to, source, None)),
                format!("plan: upToDate {from}\n"),
                "{from} -> {to}"
            );
        }
        let plan = a_plan(VERSION, NEXT, release, None);
        assert!(print_plan(&plan, &mut Closed).is_err());
    }

    #[test]
    fn a_plan_names_what_the_update_would_move_and_changes_nothing() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, _) = installation(dir.path(), platform, PREVIOUS);
        let launcher = launcher_carrying(
            dir.path(),
            &RuntimeFiles::standard(platform.as_str(), VERSION),
        );
        backdate(&fixture.root).unwrap();
        let before = snapshot(&fixture.root).unwrap();
        // The program's own file carries the release after the installed one: the two runtimes
        // differ in their version alone.
        let ran = updated_offline(&fixture.root, &launcher, false);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            (
                "plan: 0.9.0 -> 1.0.0-beta (bundled)\nplan: replace VERSION\n\
                 plan: replace build-info.json\n",
                ""
            )
        );
        // A file that carries no runtime has nothing newer to bring.
        let ran = run_release(&update_args(&fixture.root, false));
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("plan: upToDate 0.9.0\n", "")
        );
        assert!(
            snapshot(&fixture.root).unwrap() == before,
            "the folder changed"
        );
    }

    #[test]
    fn a_plan_tells_what_the_update_would_finish_first() {
        let Some(platform) = this_platform() else {
            return;
        };
        for (phase, first) in [("committed", "finishCleanup"), ("staged", "recover")] {
            let dir = tempfile::tempdir().unwrap();
            let (fixture, _) = installation(dir.path(), platform, VERSION);
            update_left(&fixture, platform, phase);
            backdate(&fixture.root).unwrap();
            let before = snapshot(&fixture.root).unwrap();
            let ran = run_release(&update_args(&fixture.root, false));
            assert_eq!(ran.code, Some(EXIT_OK), "{phase}: {ran:?}");
            assert_eq!(
                (ran.out.as_str(), ran.err.as_str()),
                (
                    format!("plan: {first}\nplan: upToDate 1.0.0-beta\n").as_str(),
                    ""
                ),
                "{phase}"
            );
            assert!(
                snapshot(&fixture.root).unwrap() == before,
                "{phase}: the folder changed"
            );
        }
    }

    #[test]
    fn an_update_brings_the_programs_own_release_without_a_feed() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, PREVIOUS);
        let launcher = launcher_carrying(
            dir.path(),
            &RuntimeFiles::standard(platform.as_str(), VERSION),
        );
        let ran = updated_offline(&fixture.root, &launcher, true);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(ran.err, "");
        // The job's progress as it went, then what it did: the game, unchanged, was not probed,
        // and the launcher copy installed before was removed, since no program ran from it.
        assert_eq!(
            phases_of(&ran.out),
            [
                "checkInstalled",
                "verifyDownload",
                "stage",
                "checkGame",
                "swap",
                "cleanup"
            ]
        );
        assert_eq!(
            past_progress(&ran.out),
            [
                "updated: 0.9.0 -> 1.0.0-beta (bundled)",
                "probed: notStaged",
                "retired: removed"
            ]
        );
        // The installation is the release the file carries, its launcher copy that file, and
        // nothing the update made or moved aside is left.
        let report = inspected(&fixture.root, false);
        assert_eq!(report.lines().next(), Some("state: installed"), "{report}");
        assert!(
            report.lines().any(|line| line == "version: 1.0.0-beta"),
            "{report}"
        );
        assert_eq!(
            fs::read(fixture.root.join(installed_launcher_name(platform))).unwrap(),
            fs::read(&launcher).unwrap()
        );
        for name in folder_names(&state).unwrap() {
            assert!(
                [
                    UPDATE_STAGING_PREFIX,
                    RETIRED_LAUNCHER_PREFIX,
                    UPDATE_BACKUP
                ]
                .iter()
                .all(|made| !name.starts_with(made)),
                "{name} is left"
            );
        }
        assert!(!state.join(JOURNAL_DIR).join(UPDATE_JOURNAL).exists());
    }

    #[test]
    fn nothing_newer_recovers_the_folder_and_tells_the_installed_release() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let carrying_this = launcher_carrying(
            dir.path(),
            &RuntimeFiles::standard(platform.as_str(), VERSION),
        );
        // A file that carries no runtime, and one that carries the installed release.
        for launcher in [None, Some(carrying_this.as_path())] {
            let dir = tempfile::tempdir().unwrap();
            let (fixture, state) = installation(dir.path(), platform, VERSION);
            let staging = state.join(format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}"));
            fs::create_dir(&staging).unwrap();
            let ran = match launcher {
                Some(launcher) => updated_offline(&fixture.root, launcher, true),
                None => run_release(&update_args(&fixture.root, true)),
            };
            assert_eq!(ran.code, Some(EXIT_OK), "{launcher:?}: {ran:?}");
            assert_eq!(
                (ran.out.as_str(), ran.err.as_str()),
                ("phase: finishCleanup\nupToDate: 1.0.0-beta\n", ""),
                "{launcher:?}"
            );
            assert!(!staging.exists(), "{launcher:?}: the staging was left");
        }
    }

    #[test]
    fn a_refused_update_tells_what_it_left() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();

        // An installation of a later release than this launcher.
        let (later, state) = installation(&dir.path().join("later"), platform, "1.0.1");
        let staging = state.join(format!("{UPDATE_STAGING_PREFIX}{STAGING_RANDOM}"));
        fs::create_dir(&staging).unwrap();
        let older = "error: launcher.older (olderLauncher): This launcher file (1.0.0-beta) is \
                     older than the installation (1.0.1)\n";
        assert_refused(
            &run_release(&update_args(&later.root, true)),
            &format!("{older}changes: none\n"),
        );
        assert_refused(&run_release(&update_args(&later.root, false)), older);
        assert!(staging.is_dir(), "the staging was removed");

        // The program's own file carries a runtime of another release than its own.
        let (fixture, state) = installation(&dir.path().join("previous"), platform, PREVIOUS);
        let other = launcher_carrying(
            dir.path(),
            &RuntimeFiles::standard(platform.as_str(), "1.0.1"),
        );
        let receipt = fs::read(state.join(RECEIPT_FILE)).unwrap();
        let damaged = "error: payload.damaged (otherVersion): The runtime's version \"1.0.1\" is \
                       not the launcher's version \"1.0.0-beta\"\n";
        let ran = updated_offline(&fixture.root, &other, true);
        assert_eq!(ran.code, Some(EXIT_REFUSED), "{ran:?}");
        assert_eq!(ran.err, format!("{damaged}changes: none\n"));
        assert!(past_progress(&ran.out).is_empty(), "{ran:?}");
        assert_refused(&updated_offline(&fixture.root, &other, false), damaged);
        assert_eq!(fs::read(state.join(RECEIPT_FILE)).unwrap(), receipt);
        for name in folder_names(&state).unwrap() {
            assert!(!name.starts_with(UPDATE_STAGING_PREFIX), "{name} is left");
        }

        // A folder that is not there.
        let absent = dir.path().join("absent");
        for yes in [true, false] {
            let ran = run_release(&update_args(&absent, yes));
            assert_eq!(ran.code, Some(EXIT_REFUSED), "{ran:?}");
            assert_eq!(ran.out, "");
            let mut lines = ran.err.lines();
            let line = lines.next().unwrap();
            assert!(line.starts_with("error: io.failed (io): "), "{}", ran.err);
            assert_eq!(lines.next(), yes.then_some("changes: none"), "{}", ran.err);
            assert_eq!(lines.next(), None, "{}", ran.err);
        }
        assert!(!absent.exists());

        // The program's own file, whose sections could not be read.
        let table = "error: payload.damaged (tableHash): the table's SHA-256 does not match the \
                     trailer\n";
        for (yes, line) in [
            (true, format!("{table}changes: none\n")),
            (false, table.to_owned()),
        ] {
            let mut own: OwnFile = Err(SectionsError::TableHash);
            let ran = run_as(&update_args(&fixture.root, yes), &RELEASE, &mut own);
            assert_refused(&ran, &line);
        }
    }

    #[test]
    fn a_plan_with_nothing_newer_refuses_what_the_update_would() {
        let Some(platform) = this_platform() else {
            return;
        };
        // Installations of the release the program is, whose recovery the update makes first
        // refuses: so does the plan, although nothing newer is at hand to update from, and in the
        // same words as the update, which changes nothing. What the plan tells the update would
        // do first comes before, as for any plan: another job's journal is a job left unfinished.
        let journals = [
            (
                "an update journal that cannot be read",
                "journalUnreadable",
                "",
            ),
            (
                "another job's journal",
                "otherJob",
                "plan: recover
",
            ),
            (
                "an update journal a later launcher stamped",
                "newerLauncher",
                "",
            ),
        ];
        for (what, rule, first) in journals {
            let dir = tempfile::tempdir().unwrap();
            let (fixture, state) = installation(dir.path(), platform, VERSION);
            let journals = state.join(JOURNAL_DIR);
            match rule {
                "journalUnreadable" => {
                    fs::create_dir(&journals).unwrap();
                    fs::write(journals.join(UPDATE_JOURNAL), b"not JSON").unwrap();
                }
                "otherJob" => {
                    fs::create_dir(&journals).unwrap();
                    let journal =
                        json!({"schema": 1, "minLauncher": VERSION, "phase": "extracting"});
                    fs::write(journals.join("expansion-n64.json"), pretty_json(&journal)).unwrap();
                }
                _ => {
                    let (journal, _, _) = update_left(&fixture, platform, "staged");
                    let mut stamped: Value =
                        serde_json::from_slice(&fs::read(&journal).unwrap()).unwrap();
                    stamped["minLauncher"] = json!(LATER_LAUNCHER);
                    fs::write(&journal, pretty_json(&stamped)).unwrap();
                }
            }
            backdate(&fixture.root).unwrap();
            let before = snapshot(&fixture.root).unwrap();

            let planned = run_release(&update_args(&fixture.root, false));
            assert_eq!(planned.code, Some(EXIT_REFUSED), "{what}: {planned:?}");
            assert_eq!(planned.out, first, "{what}");
            let refusal = planned.err.as_str();
            assert!(
                refusal.contains(&format!(" ({rule}): ")) && refusal.lines().count() == 1,
                "{what}: {refusal:?}"
            );
            let made = run_release(&update_args(&fixture.root, true));
            assert_eq!(made.code, Some(EXIT_REFUSED), "{what}: {made:?}");
            assert_eq!(made.out, "", "{what}");
            assert_eq!(made.err, format!("{refusal}changes: none\n"), "{what}");
            assert!(
                snapshot(&fixture.root).unwrap() == before,
                "{what}: the folder changed"
            );
        }
    }

    /// A launcher later than any the tests build the program as, whose stamp closes an
    /// installation to them.
    const LATER_LAUNCHER: &str = "999.0.0";

    /// Finishes a hand-over of the installation at `root` as the release build would, for this
    /// test program's own process: it started no earlier than itself, so it is never waited for,
    /// and the sweep is tested without starting a process.
    fn after_own_update(root: &Path) -> Ran {
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let running = Version::parse(VERSION).unwrap();
        let code = after_update(std::process::id(), root, &running, &mut out, &mut err);
        Ran {
            code: Some(code),
            out: String::from_utf8(out).unwrap(),
            err: String::from_utf8(err).unwrap(),
        }
    }

    #[test]
    fn an_after_update_takes_one_process_id_of_another_process() {
        let cases: &[(&str, u32)] = &[
            ("1", 1),
            ("1234", 1234),
            ("007", 7),
            ("4294967295", u32::MAX),
        ];
        for &(given, id) in cases {
            // Read and never run, since running it waits for whatever process has the id.
            let args = [OsString::from("--after-update"), OsString::from(given)];
            let mode = mode_of(&args);
            assert!(
                matches!(mode, Some(Ok(Mode::AfterUpdate { pid })) if pid == id),
                "{given}: {mode:?}"
            );
        }
        // This process's own id names no launcher it could wait for, and is refused before
        // anything is looked at.
        let own = std::process::id().to_string();
        let ran = run_release(&["--after-update", own.as_str()]);
        assert_eq!(ran.code, Some(EXIT_USAGE), "{ran:?}");
        assert_eq!(ran.err, format!("{USAGE}\n"));
        assert_eq!(ran.out, "");
    }

    #[test]
    fn what_became_of_the_previous_launcher_is_printed_answer_by_answer() {
        let answers = [
            (Previous::Gone, "gone"),
            (Previous::Newer, "newer"),
            (
                Previous::Exited {
                    waited: Duration::from_micros(1_500_900),
                },
                "exited after 1500 ms",
            ),
            (
                Previous::Exited {
                    waited: Duration::ZERO,
                },
                "exited after 0 ms",
            ),
            (Previous::Ended, "ended"),
            (
                Previous::Lingering(Linger::NotALauncher),
                "lingering notALauncher",
            ),
            (Previous::Lingering(Linger::NoRight), "lingering noRight"),
            (Previous::Unknown, "unknown"),
        ];
        for (previous, what) in answers {
            let mut out = Vec::new();
            print_previous(previous, &mut out).unwrap();
            assert_eq!(
                String::from_utf8(out).unwrap(),
                format!("previous: {what}\n")
            );
        }
        assert!(print_previous(Previous::Gone, &mut Closed).is_err());
    }

    #[test]
    fn a_sweep_of_the_retired_copies_is_printed_as_its_counts() {
        let names =
            |names: &[&str]| -> Vec<String> { names.iter().map(|&name| name.to_owned()).collect() };
        let cases = [
            (
                RetiredSweep {
                    removed: Vec::new(),
                    kept: Vec::new(),
                    lock_busy: false,
                },
                "removed 0, kept 0",
            ),
            (
                RetiredSweep {
                    removed: names(&["retired-launcher-0.9.0-AbC123xy.exe"]),
                    kept: names(&[
                        "retired-launcher-0.8.0-AbC123xy.exe",
                        "retired-launcher-1.0.0-beta-AbC123xy.exe",
                    ]),
                    lock_busy: false,
                },
                "removed 1, kept 2",
            ),
            (
                RetiredSweep {
                    removed: Vec::new(),
                    kept: Vec::new(),
                    lock_busy: true,
                },
                "lockBusy",
            ),
        ];
        for (swept, what) in cases {
            let mut out = Vec::new();
            print_retired(&swept, &mut out).unwrap();
            assert_eq!(
                String::from_utf8(out).unwrap(),
                format!("retired: {what}\n"),
                "{swept:?}"
            );
        }
        let none = RetiredSweep {
            removed: Vec::new(),
            kept: Vec::new(),
            lock_busy: false,
        };
        assert!(print_retired(&none, &mut Closed).is_err());
    }

    #[test]
    fn after_an_update_the_retired_copies_no_journal_names_are_removed() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, VERSION);
        // A retired copy no journal names, and another job's name that only looks like one.
        let unnamed = retired_copy(platform, "0.8.0");
        let put_aside = || fs::write(state.join(&unnamed), b"an older launcher").unwrap();
        put_aside();
        let not_its_own = format!("retired-n64-{STAGING_RANDOM}");
        fs::write(state.join(&not_its_own), b"").unwrap();
        let ran = after_own_update(&fixture.root);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("previous: newer\nretired: removed 1, kept 0\n", "")
        );
        assert!(!state.join(&unnamed).exists());
        assert!(state.join(&not_its_own).is_file());

        // The copy an update's journal names stays for that update's recovery, and so does the
        // journal.
        put_aside();
        let (journal, _, named) = update_left(&fixture, platform, "committed");
        let ran = after_own_update(&fixture.root);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("previous: newer\nretired: removed 1, kept 1\n", "")
        );
        assert!(!state.join(&unnamed).exists());
        assert!(state.join(&named).is_file());
        assert!(journal.is_file());

        // A report nobody saw fails the run, and the sweep runs all the same.
        put_aside();
        let running = Version::parse(VERSION).unwrap();
        let mut err = Vec::new();
        let code = after_update(
            std::process::id(),
            &fixture.root,
            &running,
            &mut Closed,
            &mut err,
        );
        assert_eq!(code, EXIT_REFUSED);
        assert!(err.is_empty(), "{}", String::from_utf8_lossy(&err));
        assert!(!state.join(&unnamed).exists());
    }

    #[test]
    fn a_busy_lock_leaves_the_retired_copies_to_the_next_start() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, VERSION);
        let retired = retired_copy(platform, PREVIOUS);
        fs::write(state.join(&retired), b"an older launcher").unwrap();
        // Another job's hold is not waited for.
        let lock = InstallFolder::at(&fixture.root).unwrap().lock().unwrap();
        let ran = after_own_update(&fixture.root);
        assert_eq!(ran.code, Some(EXIT_OK), "{ran:?}");
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("previous: newer\nretired: lockBusy\n", "")
        );
        assert!(state.join(&retired).is_file());
        drop(lock);
        let ran = after_own_update(&fixture.root);
        assert_eq!(
            (ran.out.as_str(), ran.err.as_str()),
            ("previous: newer\nretired: removed 1, kept 0\n", "")
        );
        assert!(!state.join(&retired).exists());
    }

    #[test]
    fn a_later_launchers_installation_refuses_the_sweep() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        // An installation of a later release than this launcher.
        let (later, state) = installation(&dir.path().join("later"), platform, "1.0.1");
        let retired = retired_copy(platform, PREVIOUS);
        fs::write(state.join(&retired), b"an older launcher").unwrap();
        let ran = after_own_update(&later.root);
        assert_eq!(ran.code, Some(EXIT_REFUSED), "{ran:?}");
        assert_eq!(ran.out, "previous: newer\n");
        assert_eq!(
            ran.err,
            "error: launcher.older (olderLauncher): This launcher file (1.0.0-beta) is older than \
             the installation (1.0.1)\n"
        );
        assert!(state.join(&retired).is_file());

        // An update's journal that a later launcher stamped.
        let (fixture, state) = installation(&dir.path().join("stamped"), platform, VERSION);
        let (journal, _, named) = update_left(&fixture, platform, "staged");
        let mut stamped: Value = serde_json::from_slice(&fs::read(&journal).unwrap()).unwrap();
        stamped["minLauncher"] = json!(LATER_LAUNCHER);
        fs::write(&journal, pretty_json(&stamped)).unwrap();
        let unnamed = retired_copy(platform, "0.8.0");
        fs::write(state.join(&unnamed), b"an older launcher").unwrap();
        let ran = after_own_update(&fixture.root);
        assert_eq!(ran.code, Some(EXIT_REFUSED), "{ran:?}");
        assert_eq!(ran.out, "previous: newer\n");
        assert_eq!(
            ran.err,
            "error: state.newerLauncher (newerLauncher): A later launcher's state stands in this \
             installation; this launcher changes nothing\n"
        );
        for name in [&named, &unnamed] {
            assert!(state.join(name).is_file(), "{name}");
        }
    }

    #[test]
    fn a_folder_without_a_state_folder_has_nothing_to_sweep() {
        let dir = tempfile::tempdir().unwrap();
        let absent = dir.path().join("absent");
        let file = dir.path().join("file");
        fs::write(&file, b"").unwrap();
        for root in [dir.path(), absent.as_path(), file.as_path()] {
            let ran = after_own_update(root);
            assert_eq!(ran.code, Some(EXIT_OK), "{root:?}: {ran:?}");
            assert_eq!(
                (ran.out.as_str(), ran.err.as_str()),
                ("previous: newer\nretired: removed 0, kept 0\n", ""),
                "{root:?}"
            );
        }
        assert!(!dir.path().join(STATE_DIR).exists());
        assert!(!absent.exists());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_wait_the_system_refuses_is_a_refusal_line_and_sweeps_nothing() {
        let Some(platform) = this_platform() else {
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let (fixture, state) = installation(dir.path(), platform, VERSION);
        let retired = retired_copy(platform, PREVIOUS);
        fs::write(state.join(&retired), b"an older launcher").unwrap();
        // An id past what Linux numbers its processes with, which no process can have.
        let running = Version::parse(VERSION).unwrap();
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let code = after_update(u32::MAX, &fixture.root, &running, &mut out, &mut err);
        assert_eq!(code, EXIT_REFUSED);
        assert!(out.is_empty(), "{}", String::from_utf8_lossy(&out));
        let err = String::from_utf8(err).unwrap();
        assert!(
            err.starts_with("error: io.failed (io): process 4294967295: "),
            "{err}"
        );
        assert_eq!(err.lines().count(), 1, "{err}");
        assert!(state.join(&retired).is_file());
    }
}
