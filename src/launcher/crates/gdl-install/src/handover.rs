//! The hand-over after an update that replaced the installed launcher. The launcher that ran the
//! update tells, by the file it runs and not by its name, whether it was the installed copy, and
//! whether to hand over: then it starts the copy now installed and leaves it the rest, never
//! waiting for it. The new copy, before anything else, waits for the launcher process the update
//! replaced to exit, so that it never takes that process for a running launcher of its own, ending
//! it at the limit only when it is a launcher; and it removes the installation's retired launcher
//! copies that no journal names, which a launcher still running from one keeps on Windows.

use std::collections::BTreeSet;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io;
use std::path::{Component, Path, PathBuf};
use std::process::{self, Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use gdl_files::{
    MAX_JSON_LEN, PENDING_SUFFIX, absolute_lexical, is_absent, read_json_bounded, strip_verbatim,
};
use gdl_release::{Platform, Version, launcher_asset, linux_member};
use serde_json::{Map, Value};

use crate::compat::{JOURNAL_SCHEMA, stamp_of};
use crate::folder::{InstallFolder, JOURNAL_DIR, LockError, STATE_DIR, names_in};
use crate::install::INSTALL_JOURNAL;
#[cfg(any(windows, target_os = "linux"))]
use crate::platform;
use crate::receipt::installed_launcher_name;
use crate::recover::RecoverError;
use crate::update::{UPDATE_JOURNAL, UpdateJournal, is_retired_launcher_name, retired_name};

/// How long the new copy waits for the launcher an update replaced to exit before it ends it.
pub const PREVIOUS_WAIT_LIMIT: Duration = Duration::from_secs(30);

/// How long a launcher ended at the limit is given to be gone.
pub const ENDED_WAIT_LIMIT: Duration = Duration::from_secs(5);

/// How long the new copy tries to remove each retired launcher copy before it leaves one still
/// held to the next start's sweep.
pub const RETIRED_DELETE_WINDOW: Duration = Duration::from_secs(2);

/// How often, within [`RETIRED_DELETE_WINDOW`], a retired copy that could not be removed is tried
/// again.
pub const RETIRED_DELETE_INTERVAL: Duration = Duration::from_millis(50);

/// The option that starts a launcher as the new copy of a hand-over, followed by the id of the
/// process it waits for.
pub const AFTER_UPDATE_OPTION: &str = "--after-update";

/// The exit code a launcher ended at the limit is given on Windows.
#[cfg(windows)]
const ENDED_EXIT_CODE: u32 = 1;

/// What Linux puts after the path of the file a process runs once that file was removed.
#[cfg(target_os = "linux")]
const DELETED_SUFFIX: &str = " (deleted)";

/// Where Linux lists its processes, a folder for each, named by its id.
#[cfg(target_os = "linux")]
const PROCESSES: &str = "/proc";

/// The folder in [`PROCESSES`] of the process that looks into it.
#[cfg(target_os = "linux")]
const OWN_PROCESS: &str = "self";

/// The file in a process's folder that holds its status line.
#[cfg(target_os = "linux")]
const PROCESS_STAT: &str = "stat";

/// The link in a process's folder to the file the process runs.
#[cfg(target_os = "linux")]
const PROCESS_EXECUTABLE: &str = "exe";

/// The field of the status line that holds when the process started, in clock ticks since the
/// system started, counted from 1 as proc_pid_stat(5) counts them.
#[cfg(target_os = "linux")]
const START_TIME_FIELD: usize = 22;

/// The first field of the status line after the command's name, which is the second.
#[cfg(target_os = "linux")]
const FIRST_FIELD_AFTER_NAME: usize = 3;

/// What became of the launcher process an update replaced.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Previous {
    /// No process has its id: it exited, and nothing holds it any longer.
    Gone,
    /// The process that has its id started no earlier than this one, so it is not the one that
    /// started this one: the launcher took its id after it exited, and it was not waited for.
    Newer,
    /// It exited within the limit.
    Exited {
        /// How long it was waited for.
        waited: Duration,
    },
    /// It still ran at the limit, was a launcher, and was ended.
    Ended,
    /// It still ran at the limit, and was left running.
    Lingering(Linger),
    /// Whether it runs cannot be told on this system, and it was not waited for.
    Unknown,
}

/// Why a process still running at the limit was left running.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Linger {
    /// The file it runs is not known to be a launcher's: a program of the player's is never ended.
    NotALauncher,
    /// The system would not let it be ended.
    NoRight,
}

/// Waits up to `limit` for process `pid` to exit, when it was started before this one; past `limit`,
/// ends it when it is a launcher of the installation at `root` or a launcher release file, and
/// leaves it running otherwise. Creation times are as coarse as the system's clock tick: two
/// processes started within one tick compare equal, and the later is not waited for.
///
/// The process is opened first and asked when it started only then, so that a process that took
/// its id after it ended is never waited for, nor ended, in its place: on Windows its handle keeps
/// the id its own, and on Linux its descriptor names it alone. A process the system no longer
/// knows is [`Previous::Gone`]. A Linux kernel without pidfd_open, before 5.3, cannot be asked
/// safely, and the process is not waited for: [`Previous::Unknown`].
pub fn wait_for_previous(pid: u32, limit: Duration, root: &Path) -> io::Result<Previous> {
    previous(pid, limit, root)
}

/// [`wait_for_previous`] on Windows: OpenProcess, GetProcessTimes, WaitForSingleObject, and at the
/// limit QueryFullProcessImageNameW and TerminateProcess.
#[cfg(windows)]
fn previous(pid: u32, limit: Duration, root: &Path) -> io::Result<Previous> {
    let process = match platform::open_process(pid)? {
        platform::OpenedProcess::Open(process) => process,
        platform::OpenedProcess::Gone => return Ok(Previous::Gone),
    };
    let target = platform::process_created(&process)?;
    let own = platform::own_process_created()?;
    if !started_before(target, own) {
        return Ok(Previous::Newer);
    }
    let begun = Instant::now();
    if platform::wait_process(&process, limit)? {
        return Ok(Previous::Exited {
            waited: begun.elapsed(),
        });
    }
    // An image the system will not name is not known to be a launcher's.
    let is_launcher = platform::process_image(&process).is_ok_and(|image| {
        Platform::of_this_build().is_some_and(|platform| {
            is_launcher_image(&image_in_full(&image), &in_full(root), platform)
        })
    });
    if let Err(why) = at_the_limit(is_launcher, process.can_end()) {
        return Ok(Previous::Lingering(why));
    }
    if let Err(refused) = platform::end_process(&process, ENDED_EXIT_CODE) {
        // A process that exited just as the limit came can no longer be ended.
        return if platform::wait_process(&process, Duration::ZERO)? {
            Ok(Previous::Exited {
                waited: begun.elapsed(),
            })
        } else {
            Err(refused)
        };
    }
    if platform::wait_process(&process, ENDED_WAIT_LIMIT)? {
        Ok(Previous::Ended)
    } else {
        Err(not_ended())
    }
}

/// [`wait_for_previous`] on Linux: pidfd_open, the start times in `/proc`, poll, and at the limit
/// the file `/proc` names and pidfd_send_signal.
#[cfg(target_os = "linux")]
fn previous(pid: u32, limit: Duration, root: &Path) -> io::Result<Previous> {
    let pidfd = match platform::pidfd_open(pid)? {
        platform::OpenedPid::Open(pidfd) => pidfd,
        platform::OpenedPid::Gone => return Ok(Previous::Gone),
        platform::OpenedPid::Unsupported => return Ok(Previous::Unknown),
    };
    let process = Path::new(PROCESSES).join(pid.to_string());
    let target = match start_time_at(&process) {
        Ok(target) => target,
        // Reaped since it was opened.
        Err(error) if is_absent(&error) || error.raw_os_error() == Some(libc::ESRCH) => {
            return Ok(Previous::Gone);
        }
        Err(error) => return Err(error),
    };
    let own = start_time_at(&Path::new(PROCESSES).join(OWN_PROCESS))?;
    if !started_before(target, own) {
        return Ok(Previous::Newer);
    }
    let begun = Instant::now();
    if platform::pidfd_wait(&pidfd, limit)? {
        return Ok(Previous::Exited {
            waited: begun.elapsed(),
        });
    }
    // Still running, so the id is still this process's. A file Linux will not name, as it will not
    // for another account's process, is not known to be a launcher's.
    let is_launcher = fs::read_link(process.join(PROCESS_EXECUTABLE)).is_ok_and(|image| {
        Platform::of_this_build().is_some_and(|platform| is_launcher_image(&image, root, platform))
    });
    if let Err(why) = at_the_limit(is_launcher, true) {
        return Ok(Previous::Lingering(why));
    }
    if let Err(refused) = platform::pidfd_kill(&pidfd) {
        return match refused.raw_os_error() {
            Some(libc::EPERM) => Ok(Previous::Lingering(Linger::NoRight)),
            // A process that exited just as the limit came can no longer be signalled.
            Some(libc::ESRCH) => Ok(Previous::Exited {
                waited: begun.elapsed(),
            }),
            _ => Err(refused),
        };
    }
    if platform::pidfd_wait(&pidfd, ENDED_WAIT_LIMIT)? {
        Ok(Previous::Ended)
    } else {
        Err(not_ended())
    }
}

/// [`wait_for_previous`] on a system that cannot be asked whether a process runs.
#[cfg(not(any(windows, target_os = "linux")))]
fn previous(_: u32, _: Duration, _: &Path) -> io::Result<Previous> {
    Ok(Previous::Unknown)
}

/// The error of a launcher that was ended but was not gone within [`ENDED_WAIT_LIMIT`].
#[cfg(any(windows, target_os = "linux"))]
fn not_ended() -> io::Error {
    io::Error::new(
        io::ErrorKind::TimedOut,
        "the previous launcher was ended but did not exit",
    )
}

/// `path` with its names given in full, as the system holds them, so that a short alias compares as
/// the name it stands for; as it is when the system cannot look at it.
#[cfg(windows)]
fn in_full(path: &Path) -> PathBuf {
    platform::long_path(path).unwrap_or_else(|_| path.to_path_buf())
}

/// The file a process runs, `image`, with its folder's names given in full: the file itself may
/// have been renamed since the process started, and the name the system gives may be the old one.
#[cfg(windows)]
fn image_in_full(image: &Path) -> PathBuf {
    match (image.parent(), image.file_name()) {
        (Some(folder), Some(name)) => in_full(folder).join(name),
        _ => image.to_path_buf(),
    }
}

/// When the process whose folder in `/proc` is `process` started, in clock ticks since the system
/// started, from its status line.
#[cfg(target_os = "linux")]
fn start_time_at(process: &Path) -> io::Result<u64> {
    let stat = fs::read_to_string(process.join(PROCESS_STAT))?;
    start_time(&stat).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("{} holds no start time", process.display()),
        )
    })
}

/// The start time a process's status line `stat` holds, as its [`START_TIME_FIELD`]: the fields are
/// counted from the last `)`, which ends the command's name, since the name may hold spaces and
/// brackets of its own.
#[cfg(target_os = "linux")]
fn start_time(stat: &str) -> Option<u64> {
    let (_, after_name) = stat.rsplit_once(')')?;
    after_name
        .split_ascii_whitespace()
        .nth(START_TIME_FIELD - FIRST_FIELD_AFTER_NAME)?
        .parse()
        .ok()
}

/// Whether a process created at `target` was started strictly before one created at `own`.
pub(crate) fn started_before(target: u64, own: u64) -> bool {
    target < own
}

/// What the wait does at its limit: end the process, when it is a launcher and may be ended, or
/// leave it and say why.
pub(crate) fn at_the_limit(is_launcher: bool, can_end: bool) -> Result<(), Linger> {
    if !is_launcher {
        Err(Linger::NotALauncher)
    } else if !can_end {
        Err(Linger::NoRight)
    } else {
        Ok(())
    }
}

/// Whether the image a process runs, as the system names it, is a launcher of the installation at
/// `root` or a launcher release file, for a launcher built for `platform`: the installed launcher
/// copy at `root`'s top, a retired copy an update moved aside directly in its state folder, or a
/// file anywhere named as a release's launcher asset or the Linux archive's member for a version
/// that parses. Paths are compared without their verbatim prefixes, and on Windows without regard
/// to ASCII letter case, as its file systems compare names; on Linux the suffix it puts after a
/// removed file is dropped first.
pub(crate) fn is_launcher_image(image: &Path, root: &Path, platform: Platform) -> bool {
    let image = strip_verbatim(image);
    #[cfg(target_os = "linux")]
    let image = drop_deleted(image);
    let root = strip_verbatim(root);
    let (Some(folder), Some(name)) = (image.parent(), image.file_name()) else {
        return false;
    };
    let Some(name) = name.to_str() else {
        return false;
    };
    (same_path(folder, &root) && same_name(name, installed_launcher_name(platform)))
        || (same_path(folder, &root.join(STATE_DIR)) && is_retired_copy(name, platform))
        || is_release_launcher(name, platform)
}

/// `image` without the suffix Linux puts after a removed file's path.
#[cfg(target_os = "linux")]
fn drop_deleted(image: PathBuf) -> PathBuf {
    use std::ffi::OsString;
    use std::os::unix::ffi::{OsStrExt, OsStringExt};

    match image
        .as_os_str()
        .as_bytes()
        .strip_suffix(DELETED_SUFFIX.as_bytes())
    {
        Some(kept) => PathBuf::from(OsString::from_vec(kept.to_vec())),
        None => image,
    }
}

/// Whether the paths `a` and `b` name the same entry by their names: the same components, each the
/// same name as [`same_name`] compares them.
fn same_path(a: &Path, b: &Path) -> bool {
    let (mut a, mut b) = (a.components(), b.components());
    loop {
        match (a.next(), b.next()) {
            (None, None) => return true,
            (Some(a), Some(b)) if same_component(a, b) => {}
            _ => return false,
        }
    }
}

/// Whether two components of a path are the same, as [`same_name`] compares names.
fn same_component(a: Component<'_>, b: Component<'_>) -> bool {
    same_name(a.as_os_str(), b.as_os_str())
}

/// Whether `a` and `b` are the same name: on Windows without regard to ASCII letter case, as its
/// file systems compare the names the launcher gives; elsewhere byte for byte.
fn same_name(a: impl AsRef<OsStr>, b: impl AsRef<OsStr>) -> bool {
    let (a, b) = (a.as_ref(), b.as_ref());
    if cfg!(windows) {
        a.eq_ignore_ascii_case(b)
    } else {
        a == b
    }
}

/// `name` as a name is compared: in ASCII lower case on Windows, whose file systems ignore it.
fn folded(name: &str) -> String {
    if cfg!(windows) {
        name.to_ascii_lowercase()
    } else {
        name.to_owned()
    }
}

/// Whether `name`, directly in the state folder, is a retired launcher copy for `platform`, as an
/// update names one.
fn is_retired_copy(name: &str, platform: Platform) -> bool {
    is_retired_launcher_name(&folded(name), platform)
}

/// Whether `name` is the name of a release's launcher asset for `platform`, or of the Linux
/// archive's member, for a version that parses. The version follows the asset stem's dash, which
/// is the name's first, and ends at one of the dashes after it; each is tried, since a version may
/// hold dashes itself, and the name must then be the one the release's names give that version.
fn is_release_launcher(name: &str, platform: Platform) -> bool {
    let name = folded(name);
    let Some((_, versioned)) = name.split_once('-') else {
        return false;
    };
    versioned.match_indices('-').any(|(end, _)| {
        Version::parse(&versioned[..end]).is_ok_and(|version| {
            folded(&launcher_asset(&version, platform)) == name
                || folded(&linux_member(&version)) == name
        })
    })
}

/// What a sweep of the retired launcher copies did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RetiredSweep {
    /// The retired copies removed, in byte order.
    pub removed: Vec<String>,
    /// The retired copies left in place, in byte order: those a journal names, or may name when it
    /// cannot be read, and those still held when the window ended, as a launcher still running
    /// from one holds it on Windows.
    pub kept: Vec<String>,
    /// Whether another job held the installation's lock, so that nothing was looked at or removed.
    pub lock_busy: bool,
}

/// Removes the installation's retired launcher copies that no journal names, trying each for
/// `window` every `interval`, under the lock when it is free (`lock_busy` and nothing removed when
/// it is not). A copy is removed only when it is a file directly in the state folder whose name is
/// one an update gives a launcher copy it moves aside, for the platform this launcher is built for:
/// anything else there, another job's retired file among it, stays and is not told. An update's
/// journal names its two retired copies, the installed copy's and, were it taken back, the new
/// one's; a journal that cannot be read, or of any kind but a fresh install's and an update's, may
/// name any, so then every copy stays. An installation without a state folder has nothing to sweep.
pub fn sweep_retired(
    folder: &InstallFolder,
    window: Duration,
    interval: Duration,
) -> Result<RetiredSweep, RecoverError> {
    let mut sweep = RetiredSweep {
        removed: Vec::new(),
        kept: Vec::new(),
        lock_busy: false,
    };
    let Some(platform) = Platform::of_this_build() else {
        return Ok(sweep);
    };
    let _lock = match folder.lock() {
        Ok(lock) => lock,
        Err(LockError::Busy) => {
            sweep.lock_busy = true;
            return Ok(sweep);
        }
        Err(LockError::NoStateFolder { .. }) => return Ok(sweep),
        Err(error) => return Err(RecoverError::Lock(error)),
    };
    let state = folder.state_dir();
    let claimed = Claimed::by_journals(&state, platform);
    let names = names_in(&state).map_err(|source| RecoverError::Io {
        path: state.clone(),
        source,
    })?;
    let mut kept = BTreeSet::new();
    let mut pending = BTreeSet::new();
    for name in names.into_iter().filter_map(|name| name.into_string().ok()) {
        if !is_retired_launcher_name(&name, platform)
            || !fs::symlink_metadata(state.join(&name)).is_ok_and(|entry| entry.is_file())
        {
            continue;
        }
        if claimed.claims(&name) {
            kept.insert(name);
        } else {
            pending.insert(name);
        }
    }
    let mut removed = BTreeSet::new();
    let begun = Instant::now();
    loop {
        pending.retain(|name| match fs::remove_file(state.join(name)) {
            Ok(()) => {
                removed.insert(name.clone());
                false
            }
            // Gone meanwhile, by another hand than this sweep's.
            Err(error) if is_absent(&error) => false,
            Err(_) => true,
        });
        let elapsed = begun.elapsed();
        if pending.is_empty() || elapsed >= window {
            break;
        }
        thread::sleep(interval.min(window - elapsed));
    }
    kept.extend(pending);
    sweep.removed = removed.into_iter().collect();
    sweep.kept = kept.into_iter().collect();
    Ok(sweep)
}

/// The retired launcher copies the journals in a state folder name.
enum Claimed {
    /// These names, and no other.
    Names(BTreeSet<String>),
    /// Any: a journal stands that cannot be read, or that this launcher cannot tell the names of.
    All,
}

impl Claimed {
    /// The retired launcher copies for `platform` that the journals in the state folder `state`
    /// name. A fresh install's journal names none, and a journal's pending file is no journal.
    fn by_journals(state: &Path, platform: Platform) -> Claimed {
        let journals = state.join(JOURNAL_DIR);
        let names = match fs::symlink_metadata(&journals) {
            Err(error) if is_absent(&error) => return Claimed::Names(BTreeSet::new()),
            Ok(entry) if entry.is_dir() => match names_in(&journals) {
                Ok(names) => names,
                Err(_) => return Claimed::All,
            },
            _ => return Claimed::All,
        };
        let mut claimed = BTreeSet::new();
        for name in names {
            let Some(name) = name.to_str() else {
                return Claimed::All;
            };
            if name.ends_with(PENDING_SUFFIX) || name == INSTALL_JOURNAL {
                continue;
            }
            if name != UPDATE_JOURNAL {
                return Claimed::All;
            }
            match update_claims(&journals.join(name), platform) {
                Some(names) => claimed.extend(names),
                None => return Claimed::All,
            }
        }
        Claimed::Names(claimed)
    }

    /// Whether `name` is claimed.
    fn claims(&self, name: &str) -> bool {
        match self {
            Claimed::Names(names) => names.contains(name),
            Claimed::All => true,
        }
    }
}

/// The retired launcher copies for `platform` that the update journal at `path` names: the
/// installed copy's, and the name the new copy is moved aside to when the update is taken back.
/// `None` when the journal cannot be read as one of this launcher's schema.
fn update_claims(path: &Path, platform: Platform) -> Option<[String; 2]> {
    let object: Map<String, Value> = read_json_bounded(path, MAX_JSON_LEN).ok()?;
    if stamp_of(&object).ok()?.schema > JOURNAL_SCHEMA {
        return None;
    }
    let journal = UpdateJournal::from_object(&object).ok()?;
    let new_copy = retired_name(&journal.to, &journal.staging, platform)?;
    Some([journal.retired, new_copy])
}

/// Whether `own`, the running launcher's file, is the installation's launcher copy at `root`, for
/// a launcher built for `platform`: told by the file, not its name, so that a file opened as the
/// launcher started is still told apart once an update has put another at that name. The entry
/// at the copy's name is looked at itself, never through a link there; an installation without a
/// launcher copy has none for the file to be.
pub fn is_installed_copy(own: &File, root: &Path, platform: Platform) -> io::Result<bool> {
    let copy = root.join(installed_launcher_name(platform));
    match is_same_file(own, &copy) {
        Err(error) if is_absent(&error) => Ok(false),
        answer => answer,
    }
}

/// Whether the entry at `path`, itself and not what a link there leads to, is the file `file` has
/// open, by the volume and file ids Windows gives.
#[cfg(windows)]
fn is_same_file(file: &File, path: &Path) -> io::Result<bool> {
    platform::is_same_file(file, path)
}

/// Whether the entry at `path`, itself and not what a link there leads to, is the file `file` has
/// open, by device and inode.
#[cfg(unix)]
fn is_same_file(file: &File, path: &Path) -> io::Result<bool> {
    use std::os::unix::fs::MetadataExt;

    let held = file.metadata()?;
    let entry = fs::symlink_metadata(path)?;
    Ok((held.dev(), held.ino()) == (entry.dev(), entry.ino()))
}

/// Whether a run that updated the installation to `installed` hands over to the launcher copy now
/// installed: when the running launcher was that copy before the update, since the copy at its
/// name is now another file, whatever its version; or when the running launcher is older than
/// what it installed, as a downloaded file that updated the installation from a newer release is.
pub fn hand_over_needed(was_installed_copy: bool, running: &Version, installed: &Version) -> bool {
    was_installed_copy || running < installed
}

/// Starts the launcher copy installed in the installation at `root`, for a launcher built for
/// `platform`, as the new copy of a hand-over: `<root>/<installed launcher> --after-update <the id
/// of this process>`, in `root`, as [`handover_command`] starts it; and gives the id of the process
/// it started. It is never waited for: the caller goes on to exit, and the new copy waits for it.
pub fn hand_over(root: &Path, platform: Platform) -> io::Result<u32> {
    let root = absolute_lexical(root)?;
    let program = root.join(installed_launcher_name(platform));
    let args = [
        OsString::from(AFTER_UPDATE_OPTION),
        OsString::from(process::id().to_string()),
    ];
    let started = handover_command(&program, &root, &args)?.spawn()?;
    Ok(started.id())
}

/// The command [`hand_over`] runs, for `program` and `args`: by the program's absolute path, never
/// through a shell, in `root`, with nothing to read and both output streams this process's own, and
/// with this process's environment as it is; on Linux with no descriptor past the standard three,
/// and on Windows with no console window made, as [`handover_flags`] says. A program given by a
/// relative path is refused, since which folder it would be found from is not certain.
pub(crate) fn handover_command(
    program: &Path,
    root: &Path,
    args: &[OsString],
) -> io::Result<Command> {
    if !program.is_absolute() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "a launcher handed over to is started by its absolute path",
        ));
    }
    let mut command = Command::new(program);
    command
        .args(args)
        .current_dir(root)
        .stdin(Stdio::null())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;

        command.creation_flags(handover_flags(platform::has_console()));
    }
    #[cfg(target_os = "linux")]
    platform::exec_directly(&mut command)?;
    Ok(command)
}

/// The creation flags of a hand-over's child, when this process `has_console` or not: none when it
/// has one, which the child, a console program, then shares; CREATE_NO_WINDOW when it has none, as
/// a test's child or a window program has none, so that the child makes no console window.
#[cfg(windows)]
pub(crate) fn handover_flags(has_console: bool) -> u32 {
    use windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;

    if has_console { 0 } else { CREATE_NO_WINDOW }
}

#[cfg(test)]
mod tests {
    use std::env;
    use std::ffi::OsString;
    use std::fs::{self, File};
    use std::io::{self, BufRead, BufReader, Read, Write};
    use std::path::{Path, PathBuf};
    use std::process::{Child, Command, ExitStatus, Stdio};
    use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
    use std::thread;
    use std::time::{Duration, Instant};

    #[cfg(windows)]
    use gdl_files::strip_verbatim;
    use gdl_files::{RETRY_INTERVAL, RETRY_WINDOW, retry_locked};
    use gdl_fixtures::{
        InstallFixture, RuntimeFiles, append_sections, pretty_json, symlink_file_where_allowed,
        tiny_elf, tiny_pe,
    };
    use gdl_release::{Platform, Version, launcher_asset, linux_member};
    use serde_json::{Value, json};
    use tempfile::TempDir;

    use super::{
        AFTER_UPDATE_OPTION, Linger, Previous, RetiredSweep, at_the_limit, hand_over_needed,
        handover_command, is_installed_copy, is_launcher_image, started_before, sweep_retired,
        wait_for_previous,
    };
    use crate::compat::MIN_LAUNCHER;
    use crate::folder::{InstallFolder, JOURNAL_DIR, STATE_DIR};
    use crate::probe::is_text_busy;
    use crate::receipt::installed_launcher_name;
    use crate::sections::{RUNTIME_SECTION, Sections};
    use crate::test_platform::{other_platform, this_platform};
    use crate::update::{UPDATE_JOURNAL, UPDATE_STAGING_PREFIX};

    /// The variable that makes a child of this test program print [`READY`] and then sleep for the
    /// milliseconds it holds. Unset, as in a run of the tests, [`handover_sleeper`] does nothing.
    const SLEEP: &str = "GDL_HANDOVER_SLEEP";

    /// The variable that makes a child of this test program wait for a previous launcher, as
    /// `<pid>:<limit in milliseconds>:<installation folder>`, and print what became of it after
    /// [`ANSWER`]. Unset, as in a run of the tests, [`handover_waiter`] does nothing.
    const WAIT: &str = "GDL_HANDOVER_WAIT";

    /// The test a sleeping child runs alone.
    const SLEEPER: &str = "handover::tests::handover_sleeper";

    /// The test a waiting child runs alone.
    const WAITER: &str = "handover::tests::handover_waiter";

    /// The line a sleeping child prints once it runs.
    const READY: &str = "ready";

    /// What starts the line a waiting child prints its answer on.
    const ANSWER: &str = "previous: ";

    /// The test a reporting child runs alone, whose name among the child's arguments is also what
    /// makes it report: a hand-over's command runs its program with this process's environment as
    /// it is, so no variable can be set for that child alone. Run without it, as in a run of the
    /// tests, [`handover_reporter`] does nothing.
    #[cfg(target_os = "linux")]
    const REPORTER: &str = "handover::tests::handover_reporter";

    /// What starts the line a reporting child lists its open descriptors on.
    #[cfg(target_os = "linux")]
    const DESCRIPTORS: &str = "descriptors: ";

    /// What starts the line a reporting child names what its standard input reads on.
    #[cfg(target_os = "linux")]
    const INPUT: &str = "input: ";

    /// What starts the line a reporting child names the folder it runs in on.
    #[cfg(target_os = "linux")]
    const FOLDER: &str = "folder: ";

    /// Where Linux lists the descriptors a process has open, as links to what each is open on.
    #[cfg(target_os = "linux")]
    const OWN_DESCRIPTORS: &str = "/proc/self/fd";

    /// What a standard input that reads nothing is open on.
    #[cfg(target_os = "linux")]
    const NOTHING_TO_READ: &str = "/dev/null";

    /// The descriptor of standard input.
    #[cfg(target_os = "linux")]
    const STANDARD_INPUT: u32 = 0;

    /// The descriptors a program a hand-over started may hold: standard input, output and error.
    #[cfg(target_os = "linux")]
    const STANDARD_DESCRIPTORS: &str = "0 1 2";

    /// The variable that makes a copy of this test program, started at an installation's launcher
    /// name under a filtered token, hand over as an updated launcher does: it renames its own
    /// running file to the retired name the variable holds, and the new copy it starts, which
    /// inherits the variable, waits for it. Unset, as in a run of the tests, [`filtered_old_copy`]
    /// and [`filtered_new_copy`] do nothing.
    #[cfg(windows)]
    const FILTERED: &str = "GDL_HANDOVER_FILTERED";

    /// The variable that names the log both copies of the filtered-token hand-over write to, which
    /// the old copy reads to learn that the new one waits for it.
    #[cfg(windows)]
    const FILTERED_LOG: &str = "GDL_HANDOVER_FILTERED_LOG";

    /// The test the old copy under a filtered token runs alone.
    #[cfg(windows)]
    const FILTERED_OLD_COPY: &str = "handover::tests::filtered_old_copy";

    /// The test the new copy the old one starts runs alone.
    #[cfg(windows)]
    const FILTERED_NEW_COPY: &str = "handover::tests::filtered_new_copy";

    /// What starts each line the old copy under a filtered token prints.
    #[cfg(windows)]
    const OLD_COPY: &str = "old copy ";

    /// What starts each line the new copy it starts prints.
    #[cfg(windows)]
    const NEW_COPY: &str = "new copy ";

    /// What follows a copy's start on the line that says whether its token holds the
    /// Administrators group to grant.
    #[cfg(windows)]
    const ADMINISTRATORS: &str = "administrators: ";

    /// What follows a copy's start on the line that gives its token's integrity level.
    #[cfg(windows)]
    const INTEGRITY: &str = "integrity: ";

    /// What follows the old copy's start on the line that gives the id of the new copy it started.
    #[cfg(windows)]
    const HANDED_OVER: &str = "handedOver: ";

    /// What follows the new copy's start on the line it prints just before it waits for the old
    /// copy, which the line ends with the id of.
    #[cfg(windows)]
    const WAITING: &str = "waiting for ";

    /// How often the old copy of the filtered-token hand-over reads the log again for the new
    /// copy's line that it waits.
    #[cfg(windows)]
    const LOG_POLL: Duration = Duration::from_millis(50);

    /// What follows the new copy's start on the line that tells what its sweep of the retired
    /// copies did.
    #[cfg(windows)]
    const SWEPT: &str = "retired: ";

    /// How a waiter's answer begins when the process it waited for exited within the limit.
    const EXITED: &str = "exited ";

    /// The name of the log both copies of a filtered hand-over write their output to.
    #[cfg(windows)]
    const HANDOVER_LOG: &str = "handover.log";

    /// How long the new copy of a filtered hand-over is given to end once the test is done with
    /// it, and then to be gone once ended.
    #[cfg(windows)]
    const NEW_COPY_END: Duration = Duration::from_secs(5);

    /// How long a waiter is started after the process it waits for printed that it runs: more than
    /// two clock ticks of either system, so that the two never share a creation time.
    const START_TICK_GAP: Duration = Duration::from_millis(50);

    /// How long a child is given to say what it is to say, or to end, before the test ends it and
    /// fails: long enough for a scanner's first look at a new copy of the test program.
    const CHILD_DEADLINE: Duration = Duration::from_secs(60);

    /// How long a sleeper that a waiter is to find exited sleeps.
    const BRIEF_SLEEP: Duration = Duration::from_secs(2);

    /// The least a waiter for a [`BRIEF_SLEEP`] is to have waited.
    const LEAST_WAITED: Duration = Duration::from_secs(1);

    /// How long a sleeper that is to be found still running at a waiter's limit sleeps.
    const LONG_SLEEP: Duration = Duration::from_secs(30);

    /// The limit of a waiter that is to find its sleeper still running.
    const SHORT_LIMIT: Duration = Duration::from_millis(500);

    /// How long an exited child's id is asked after until the system no longer knows it.
    const GONE_WITHIN: Duration = Duration::from_secs(10);

    /// How long a retired copy held open is tried, in a test.
    const HELD_WINDOW: Duration = Duration::from_millis(200);

    /// The launcher's version, which the installations the tests sweep are of.
    const INSTALLED: &str = "1.0.0-beta";

    /// The version the hand-written update journals update to.
    const NEXT: &str = "1.0.0-beta.1";

    /// The staging the hand-written update journals name.
    const STAGING: &str = "stage-AbC123xy";

    /// Writes `line` on a line of its own, past the test harness's capture of what a test prints
    /// and after the test's name the harness has begun a line with.
    fn write_line(line: &str) {
        let mut out = io::stdout().lock();
        writeln!(out, "\n{line}")
            .and_then(|()| out.flush())
            .expect("a child's line is written");
    }

    /// A sleeping child's part of the hand-over tests, which does nothing unless [`SLEEP`] is set:
    /// prints [`READY`], then sleeps as long as it says, standing for a launcher that has yet to
    /// exit.
    #[test]
    fn handover_sleeper() {
        let Ok(millis) = env::var(SLEEP) else {
            return;
        };
        let millis: u64 = millis.parse().expect("a sleep is in milliseconds");
        write_line(READY);
        thread::sleep(Duration::from_millis(millis));
    }

    /// A waiting child's part of the hand-over tests, which does nothing unless [`WAIT`] is set:
    /// waits for the process it names as a new launcher copy waits for the one it replaced, and
    /// prints what became of it.
    #[test]
    fn handover_waiter() {
        let Ok(given) = env::var(WAIT) else {
            return;
        };
        let mut parts = given.splitn(3, ':');
        let mut next = || {
            parts
                .next()
                .expect("a wait names a pid, a limit and a folder")
        };
        let pid = next().parse().expect("a pid is a number");
        let limit = Duration::from_millis(next().parse().expect("a limit is in milliseconds"));
        let root = PathBuf::from(next());
        let answer = wait_for_previous(pid, limit, &root);
        write_line(&format!("{ANSWER}{}", answer_text(&answer)));
    }

    /// What a waiting child prints of `answer`.
    fn answer_text(answer: &io::Result<Previous>) -> String {
        match answer {
            Ok(Previous::Gone) => "gone".to_owned(),
            Ok(Previous::Newer) => "newer".to_owned(),
            Ok(Previous::Exited { waited }) => format!("{EXITED}{}", waited.as_millis()),
            Ok(Previous::Ended) => "ended".to_owned(),
            Ok(Previous::Lingering(Linger::NotALauncher)) => "lingering notALauncher".to_owned(),
            Ok(Previous::Lingering(Linger::NoRight)) => "lingering noRight".to_owned(),
            Ok(Previous::Unknown) => "unknown".to_owned(),
            Err(error) => format!("error {error}"),
        }
    }

    /// A reporting child's part of the hand-over tests, which does nothing unless the child was
    /// started to run it alone, by its name as [`REPORTER`] gives it: prints the descriptors the
    /// child has open, the listing's own left out, what its standard input reads, and the folder
    /// it runs in, as a launcher a hand-over started finds them.
    #[cfg(target_os = "linux")]
    #[test]
    fn handover_reporter() {
        if !env::args().skip(1).any(|argument| argument == REPORTER) {
            return;
        }
        let listing = Path::new(OWN_DESCRIPTORS);
        // The descriptor the listing reads through is open on the listed folder itself.
        let listed = fs::canonicalize(listing).unwrap();
        let mut descriptors = Vec::new();
        for entry in fs::read_dir(listing).unwrap() {
            let name = entry.unwrap().file_name();
            if fs::read_link(listing.join(&name)).is_ok_and(|target| target == listed) {
                continue;
            }
            let descriptor: u32 = name.to_str().unwrap().parse().unwrap();
            descriptors.push(descriptor);
        }
        descriptors.sort_unstable();
        let descriptors: Vec<String> = descriptors.iter().map(u32::to_string).collect();
        write_line(&format!("{DESCRIPTORS}{}", descriptors.join(" ")));
        let input = fs::read_link(listing.join(STANDARD_INPUT.to_string())).unwrap();
        write_line(&format!("{INPUT}{}", input.display()));
        let folder = env::current_dir().unwrap();
        write_line(&format!("{FOLDER}{}", folder.display()));
    }

    /// The old copy's part of the filtered-token hand-over, which does nothing unless [`FILTERED`]
    /// is set: run at an installation's launcher name under a filtered token, it says what its
    /// token holds, renames its own running file aside to the retired name [`FILTERED`] holds,
    /// places a new copy at its name, starts that copy by the hand-over's command, to wait for this
    /// process, and ends a while after the log [`FILTERED_LOG`] names says the new copy waits, so
    /// that the new copy waits for a process that still runs.
    #[cfg(windows)]
    #[test]
    fn filtered_old_copy() {
        let Ok(retired_name) = env::var(FILTERED) else {
            return;
        };
        let log = PathBuf::from(env::var_os(FILTERED_LOG).expect("the log is named"));
        let own = env::current_exe().unwrap();
        // Only a copy at an installation's launcher name renames itself.
        assert_eq!(
            own.file_name().and_then(|name| name.to_str()),
            Some(installed_launcher_name(this_platform())),
            "{}",
            own.display()
        );
        let root = own.parent().unwrap();
        report_token(OLD_COPY);
        let retired_copy = root.join(STATE_DIR).join(retired_name);
        fs::rename(&own, &retired_copy).unwrap();
        fs::copy(&retired_copy, &own).unwrap();
        let args = [
            "--exact".to_owned(),
            FILTERED_NEW_COPY.to_owned(),
            "--test-threads".to_owned(),
            "1".to_owned(),
            std::process::id().to_string(),
        ]
        .map(OsString::from);
        let new_copy = start_unwaited(handover_command(&own, root, &args).unwrap()).unwrap();
        write_line(&format!("{OLD_COPY}{HANDED_OVER}{new_copy}"));
        // A launcher takes a moment to end after it hands over, as a window takes to close: here
        // from when the new copy waits, so that it waits for a process still running.
        let waiting = format!("{NEW_COPY}{WAITING}{}", std::process::id());
        let begun = Instant::now();
        while !log_lines(&log).contains(&waiting) {
            assert!(
                begun.elapsed() < CHILD_DEADLINE,
                "no line {waiting:?}: {:#?}",
                log_lines(&log)
            );
            thread::sleep(LOG_POLL);
        }
        thread::sleep(BRIEF_SLEEP);
    }

    /// The new copy's part of the filtered-token hand-over, which does nothing unless [`FILTERED`]
    /// is set: started by the old copy, with the old copy's id as its last argument, it says what
    /// its token holds and that it waits, waits for the old copy as a new launcher copy waits for
    /// the one an update replaced, sweeps the installation's retired copies, and says what each
    /// did.
    #[cfg(windows)]
    #[test]
    fn filtered_new_copy() {
        if env::var_os(FILTERED).is_none() {
            return;
        }
        let previous: u32 = env::args()
            .next_back()
            .and_then(|pid| pid.parse().ok())
            .expect("the old copy's id is the last argument");
        let own = env::current_exe().unwrap();
        let root = own.parent().unwrap();
        report_token(NEW_COPY);
        write_line(&format!("{NEW_COPY}{WAITING}{previous}"));
        let answer = wait_for_previous(previous, super::PREVIOUS_WAIT_LIMIT, root);
        write_line(&format!("{NEW_COPY}{ANSWER}{}", answer_text(&answer)));
        let folder = InstallFolder::at(root).unwrap();
        let sweep = sweep_retired(
            &folder,
            super::RETIRED_DELETE_WINDOW,
            super::RETIRED_DELETE_INTERVAL,
        )
        .unwrap();
        write_line(&format!(
            "{NEW_COPY}{SWEPT}removed {:?}, kept {:?}, lockBusy {}",
            sweep.removed, sweep.kept, sweep.lock_busy
        ));
    }

    /// Starts `command` and gives the id of the process it started, never waiting for it, as a
    /// hand-over leaves the copy it starts.
    #[cfg(windows)]
    fn start_unwaited(mut command: Command) -> io::Result<u32> {
        let started = command.spawn()?;
        Ok(started.id())
    }

    /// Prints, after `role`, whether this process's token holds the Administrators group to
    /// grant, and its integrity level.
    #[cfg(windows)]
    fn report_token(role: &str) {
        let administrator = crate::platform::token_is_administrator().unwrap();
        write_line(&format!("{role}{ADMINISTRATORS}{administrator}"));
        let integrity = crate::platform::token_integrity().unwrap();
        write_line(&format!("{role}{INTEGRITY}{integrity}"));
    }

    /// A child of this test program, or of a copy of it, whose output is read as it comes; ended
    /// when dropped, unless it has ended by itself, so that none outlives its test.
    struct Started {
        /// The child.
        child: Child,
        /// The lines it prints, on either stream, as they come.
        lines: Receiver<String>,
        /// What it printed so far, for a failure's message.
        seen: Vec<String>,
    }

    impl Started {
        /// Starts `program` running the test `test` alone, with the variable `variable` set to
        /// `value` and the other child's unset, nothing to read and on Windows without a window,
        /// as [`spawn`](Self::spawn) starts it.
        fn start(program: &Path, test: &str, variable: &str, value: &str) -> Started {
            let mut command = Command::new(program);
            command
                .args(["--exact", test, "--test-threads", "1"])
                .env_remove(SLEEP)
                .env_remove(WAIT)
                .env(variable, value)
                .stdin(Stdio::null());
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                use windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;

                command.creation_flags(CREATE_NO_WINDOW);
            }
            Started::spawn(command)
        }

        /// Starts `command` with both its output streams read as they come; tried again while
        /// Linux refuses a copy just written for [`is_text_busy`].
        fn spawn(mut command: Command) -> Started {
            command.stdout(Stdio::piped()).stderr(Stdio::piped());
            let begun = Instant::now();
            let mut child = loop {
                match command.spawn() {
                    Ok(child) => break child,
                    Err(error) if is_text_busy(&error) && begun.elapsed() < RETRY_WINDOW => {
                        thread::sleep(RETRY_INTERVAL);
                    }
                    Err(error) => {
                        panic!("{}: {error}", Path::new(command.get_program()).display())
                    }
                }
            };
            let (sender, lines) = mpsc::channel();
            let stdout = child.stdout.take().expect("the child's output is piped");
            let stderr = child.stderr.take().expect("the child's errors are piped");
            for stream in [Box::new(stdout) as Box<dyn Read + Send>, Box::new(stderr)] {
                let sender = sender.clone();
                thread::spawn(move || {
                    for line in BufReader::new(stream).lines() {
                        let Ok(line) = line else { break };
                        if sender.send(line).is_err() {
                            break;
                        }
                    }
                });
            }
            Started {
                child,
                lines,
                seen: Vec::new(),
            }
        }

        /// Starts this test program as a sleeper of `sleep`, and gives it once it printed that it
        /// runs.
        fn sleeper(sleep: Duration) -> Started {
            Started::sleeper_at(&env::current_exe().unwrap(), sleep)
        }

        /// Starts `program`, a copy of this test program, as a sleeper of `sleep`, and gives it
        /// once it printed that it runs.
        fn sleeper_at(program: &Path, sleep: Duration) -> Started {
            let millis = sleep.as_millis().to_string();
            let mut sleeper = Started::start(program, SLEEPER, SLEEP, &millis);
            sleeper.line_starting(READY);
            sleeper
        }

        /// Starts this test program as a waiter for `previous` up to `limit` in the installation
        /// at `root`, once [`START_TICK_GAP`] has passed, and gives what it printed of it.
        fn wait_for(previous: &Started, limit: Duration, root: &Path) -> String {
            thread::sleep(START_TICK_GAP);
            let given = format!(
                "{}:{}:{}",
                previous.child.id(),
                limit.as_millis(),
                root.display()
            );
            let mut waiter = Started::start(&env::current_exe().unwrap(), WAITER, WAIT, &given);
            let answer = waiter.line_starting(ANSWER);
            let status = waiter.ended();
            assert!(status.success(), "{status}: {:?}", waiter.seen);
            answer[ANSWER.len()..].to_owned()
        }

        /// The id of the child.
        fn pid(&self) -> u32 {
            self.child.id()
        }

        /// The first line from here on that starts with `start`, within [`CHILD_DEADLINE`].
        fn line_starting(&mut self, start: &str) -> String {
            let deadline = Instant::now() + CHILD_DEADLINE;
            loop {
                let left = deadline.saturating_duration_since(Instant::now());
                match self.lines.recv_timeout(left) {
                    Ok(line) => {
                        self.seen.push(line.clone());
                        if line.starts_with(start) {
                            return line;
                        }
                    }
                    Err(RecvTimeoutError::Timeout) => {
                        panic!("no line {start:?} within the deadline: {:?}", self.seen)
                    }
                    Err(RecvTimeoutError::Disconnected) => {
                        panic!("the child ended without a line {start:?}: {:?}", self.seen)
                    }
                }
            }
        }

        /// How the child ended, waited for up to [`CHILD_DEADLINE`].
        fn ended(&mut self) -> ExitStatus {
            self.ended_within(CHILD_DEADLINE)
                .unwrap_or_else(|| panic!("the child did not end: {:?}", self.seen))
        }

        /// How the child ended, when it ends within `within`.
        fn ended_within(&mut self, within: Duration) -> Option<ExitStatus> {
            let deadline = Instant::now() + within;
            loop {
                if let Some(status) = self.child.try_wait().unwrap() {
                    return Some(status);
                }
                if Instant::now() >= deadline {
                    return None;
                }
                thread::sleep(RETRY_INTERVAL);
            }
        }
    }

    impl Drop for Started {
        fn drop(&mut self) {
            if matches!(self.child.try_wait(), Ok(None)) {
                let _ = self.child.kill();
            }
            let _ = self.child.wait();
        }
    }

    /// An installation's folder of the test's own, with its state folder.
    fn installation() -> TempDir {
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir(dir.path().join(STATE_DIR)).unwrap();
        dir
    }

    /// A copy of this test program as the installed launcher copy at the top of the installation
    /// at `root`.
    fn launcher_copy_in(root: &Path) -> PathBuf {
        let copy = root.join(installed_launcher_name(this_platform()));
        fs::copy(env::current_exe().unwrap(), &copy).unwrap();
        copy
    }

    /// Removes the copy of this test program at `path` once the child that ran it has ended,
    /// waiting out a moment in which Windows, or a scanner, still holds the file of a program that
    /// ended, so that no copy outlives its test.
    fn remove_copy(path: &Path) {
        retry_locked(|| fs::remove_file(path))
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
    }

    /// Checks that `sleeper` was ended by the waiter: gone, with the code a launcher ended at the
    /// limit is given, or by SIGKILL on Linux.
    #[track_caller]
    fn assert_ended(sleeper: &mut Started) {
        let status = sleeper
            .ended_within(Duration::from_secs(5))
            .expect("the ended launcher is gone");
        #[cfg(windows)]
        assert_eq!(
            status.code(),
            Some(i32::try_from(super::ENDED_EXIT_CODE).unwrap()),
            "{status}"
        );
        #[cfg(unix)]
        {
            use std::os::unix::process::ExitStatusExt;
            assert_eq!(status.signal(), Some(libc::SIGKILL), "{status}");
        }
    }

    #[test]
    fn a_predecessor_of_the_same_tick_is_not_waited_for() {
        // The rule is strict: a process created in the same tick as the waiter is not known to
        // have started before it, so it is never waited for.
        let tick = 133_000_000_000_000_000;
        assert!(!started_before(tick, tick));
        assert!(started_before(tick, tick + 1));
        assert!(!started_before(tick + 1, tick));
    }

    #[test]
    fn the_limit_without_the_right_to_end_leaves_it() {
        assert_eq!(at_the_limit(true, true), Ok(()));
        assert_eq!(at_the_limit(true, false), Err(Linger::NoRight));
        assert_eq!(at_the_limit(false, true), Err(Linger::NotALauncher));
        assert_eq!(at_the_limit(false, false), Err(Linger::NotALauncher));
    }

    #[test]
    fn launcher_images_are_known_by_their_names() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("Gauntlet Dark Legacy");
        let state = root.join(STATE_DIR);
        let elsewhere = dir.path().join("Downloads");
        let version = Version::parse(INSTALLED).unwrap();
        for platform in Platform::ALL {
            let launchers = [
                root.join(installed_launcher_name(platform)),
                state.join(retired(platform, "1.0.0-beta", "AbC123xy")),
                state.join(retired(platform, "0.9.0+build.7", "Zz9Yy8Xx")),
                elsewhere.join(launcher_asset(&version, platform)),
                elsewhere.join(launcher_asset(
                    &Version::parse("1.2.3-rc.1-x+meta").unwrap(),
                    platform,
                )),
                elsewhere.join(linux_member(&version)),
                root.join(linux_member(&version)),
            ];
            for image in &launchers {
                assert!(
                    is_launcher_image(image, &root, platform),
                    "{}: {}",
                    platform.as_str(),
                    image.display()
                );
            }
            let other_root = dir.path().join("Another");
            let others = [
                elsewhere.join("notepad.exe"),
                root.join("gauntlet.exe"),
                root.join(platform.executable()),
                state.join("retired-launcher-x"),
                state.join(format!(
                    "retired-launcher-1.0.0-beta-AbC123x{}",
                    suffix(platform)
                )),
                state.join(retired(
                    other_platform_of(platform),
                    "1.0.0-beta",
                    "AbC123xy",
                )),
                state.join(retired_n64()),
                root.join(retired(platform, "1.0.0-beta", "AbC123xy")),
                state
                    .join("journal")
                    .join(retired(platform, "1.0.0-beta", "AbC123xy")),
                other_root.join(installed_launcher_name(platform)),
                other_root
                    .join(STATE_DIR)
                    .join(retired(platform, "1.0.0-beta", "AbC123xy")),
                root.join(STATE_DIR).join(installed_launcher_name(platform)),
                elsewhere.join("GauntletDarkLegacy-1.0.0-beta-launcher"),
                elsewhere.join("GauntletDarkLegacy-1.0-windows-x64-launcher.exe"),
                elsewhere.join("GauntletDarkLegacy--windows-x64-launcher.exe"),
                elsewhere.join("GauntletDarkLegacy-1.0.0-beta-windows-x64-setup.exe"),
                elsewhere.join("GauntletDarkLegacy-4.0.0-alpha.3-linux-x64-setup.tar.gz"),
                elsewhere.join("launcher (1).exe"),
            ];
            for image in &others {
                assert!(
                    !is_launcher_image(image, &root, platform),
                    "{}: {}",
                    platform.as_str(),
                    image.display()
                );
            }
        }
        let platform = this_platform();
        let copy = root.join(installed_launcher_name(platform));
        assert!(is_launcher_image(&copy, &root, platform));
        #[cfg(windows)]
        {
            // The installed copy through other spellings of the same names: a verbatim prefix on
            // either side.
            let verbatim = PathBuf::from(format!(r"\\?\{}", copy.display()));
            assert!(is_launcher_image(&verbatim, &root, platform));
            let verbatim_root = PathBuf::from(format!(r"\\?\{}", root.display()));
            assert!(is_launcher_image(&copy, &verbatim_root, platform));
            // Windows' file systems ignore letter case, so any spelling names the same file.
            let upper = |path: &Path| PathBuf::from(path.to_string_lossy().to_uppercase());
            assert!(is_launcher_image(&upper(&copy), &root, platform));
            assert!(is_launcher_image(&copy, &upper(&root), platform));
            let retired_copy = state.join(retired(platform, "1.0.0-beta", "AbC123xy"));
            assert!(is_launcher_image(&upper(&retired_copy), &root, platform));
            let asset = elsewhere.join(launcher_asset(&version, platform));
            assert!(is_launcher_image(&upper(&asset), &root, platform));
        }
        #[cfg(target_os = "linux")]
        {
            // Linux keeps letter case apart, and names a removed file with a suffix after it.
            let upper = root.join(installed_launcher_name(platform).to_uppercase());
            assert!(!is_launcher_image(&upper, &root, platform));
            let removed = state.join(format!(
                "{}{}",
                retired(platform, "1.0.0-beta", "AbC123xy"),
                super::DELETED_SUFFIX
            ));
            assert!(is_launcher_image(&removed, &root, platform));
            let removed_member = elsewhere.join(format!(
                "{}{}",
                linux_member(&version),
                super::DELETED_SUFFIX
            ));
            assert!(is_launcher_image(&removed_member, &root, platform));
        }
    }

    /// The suffix a launcher program for `platform` carries.
    fn suffix(platform: Platform) -> &'static str {
        match platform {
            Platform::WindowsX64 => ".exe",
            Platform::LinuxX64 => "",
        }
    }

    /// The platform a release is built for that is not `platform`.
    fn other_platform_of(platform: Platform) -> Platform {
        Platform::ALL
            .into_iter()
            .find(|&other| other != platform)
            .unwrap()
    }

    /// The name an update gives the launcher copy of `version` for `platform` that it moves aside,
    /// with the random part `random`.
    fn retired(platform: Platform, version: &str, random: &str) -> String {
        format!("retired-launcher-{version}-{random}{}", suffix(platform))
    }

    /// A name an expansion's change gives what it puts aside.
    fn retired_n64() -> String {
        "retired-n64-AbC123xy".to_owned()
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_start_time_is_read_past_the_command_name() {
        // The command's name may hold spaces and brackets of its own; the fields count from the
        // last bracket that closes it.
        let fields_after = "S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 123456 19 20";
        let stat = format!("4242 (a) (name) {fields_after}");
        assert_eq!(super::start_time(&stat), Some(123_456));
        assert_eq!(
            super::start_time(&format!("1 (sh) {fields_after}")),
            Some(123_456)
        );
        assert_eq!(super::start_time("4242 (cut short) S 1 2"), None);
        assert_eq!(super::start_time("no name at all"), None);
        // This process's own line is read.
        let own = fs::read_to_string("/proc/self/stat").unwrap();
        assert!(super::start_time(&own).is_some(), "{own}");
    }

    #[test]
    fn the_previous_launcher_is_waited_for() {
        let _one = crate::one_test_child();
        let dir = installation();
        let mut sleeper = Started::sleeper(BRIEF_SLEEP);
        let answer = Started::wait_for(&sleeper, super::PREVIOUS_WAIT_LIMIT, dir.path());
        let waited = answer
            .strip_prefix(EXITED)
            .unwrap_or_else(|| panic!("{answer}"))
            .parse()
            .unwrap();
        assert!(
            Duration::from_millis(waited) >= LEAST_WAITED,
            "waited {waited} ms"
        );
        assert!(sleeper.ended().success());
    }

    #[test]
    fn a_process_started_after_the_waiter_is_not_waited_for() {
        // The test process waits for a child of its own, which started after it, as a process
        // that took the id of a launcher that exited would have: it is not waited for.
        let _one = crate::one_test_child();
        let dir = installation();
        let mut sleeper = Started::sleeper(LONG_SLEEP);
        let begun = Instant::now();
        let answer = wait_for_previous(sleeper.pid(), LONG_SLEEP, dir.path()).unwrap();
        assert_eq!(answer, Previous::Newer);
        assert!(
            begun.elapsed() < Duration::from_secs(5),
            "{:?}",
            begun.elapsed()
        );
        assert!(sleeper.ended_within(Duration::ZERO).is_none());
    }

    #[test]
    fn a_gone_process_is_gone() {
        let _one = crate::one_test_child();
        let dir = installation();
        let mut child = Started::sleeper(Duration::ZERO);
        let pid = child.pid();
        assert!(child.ended().success());
        drop(child);
        // Reaped, and its handle dropped. On Windows another program may hold the process that
        // ended for a moment, a scanner among them, and the system knows its id until it lets go;
        // a process known so is this test's child, which started after the test.
        let begun = Instant::now();
        loop {
            match wait_for_previous(pid, Duration::ZERO, dir.path()).unwrap() {
                Previous::Gone => break,
                Previous::Newer if begun.elapsed() < GONE_WITHIN => {
                    thread::sleep(RETRY_INTERVAL);
                }
                other => panic!("{other:?} after {:?}", begun.elapsed()),
            }
        }
    }

    #[test]
    fn a_lingering_previous_launcher_is_ended_at_the_limit() {
        let _one = crate::one_test_child();
        let dir = installation();
        let copy = launcher_copy_in(dir.path());
        let mut sleeper = Started::sleeper_at(&copy, LONG_SLEEP);
        let answer = Started::wait_for(&sleeper, SHORT_LIMIT, dir.path());
        assert_eq!(answer, "ended", "{:?}", sleeper.seen);
        assert_ended(&mut sleeper);
        remove_copy(&copy);
    }

    #[test]
    fn a_renamed_previous_launcher_is_still_a_launcher() {
        // An update renames the running copy aside; whichever name the system gives, the one it
        // started from or the one the rename left, the process is a launcher.
        let _one = crate::one_test_child();
        let dir = installation();
        let copy = launcher_copy_in(dir.path());
        let mut sleeper = Started::sleeper_at(&copy, LONG_SLEEP);
        let retired_copy =
            dir.path()
                .join(STATE_DIR)
                .join(retired(this_platform(), "0.9.0", "AbC123xy"));
        fs::rename(&copy, &retired_copy).unwrap();
        let answer = Started::wait_for(&sleeper, SHORT_LIMIT, dir.path());
        assert_eq!(answer, "ended", "{:?}", sleeper.seen);
        assert_ended(&mut sleeper);
        remove_copy(&retired_copy);
    }

    #[test]
    fn a_lingering_process_of_another_image_is_left_running() {
        // The test program under its own name is no launcher: a stray wait never ends a program of
        // the player's.
        let _one = crate::one_test_child();
        let dir = installation();
        let mut sleeper = Started::sleeper(LONG_SLEEP);
        let answer = Started::wait_for(&sleeper, SHORT_LIMIT, dir.path());
        assert_eq!(answer, "lingering notALauncher", "{:?}", sleeper.seen);
        assert!(sleeper.ended_within(Duration::ZERO).is_none());
    }

    /// An installation of the launcher's first release for this platform in a folder of the test's
    /// own, with its receipt.
    fn installed() -> (TempDir, InstallFixture, InstallFolder) {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("install");
        let runtime = RuntimeFiles::standard(this_platform().as_str(), INSTALLED);
        let fixture = InstallFixture::write(&root, runtime).unwrap();
        let folder = InstallFolder::at(&root).unwrap();
        (dir, fixture, folder)
    }

    /// Writes a retired file `name` directly in the state folder of the installation at `root`, and
    /// gives its path.
    fn put_aside(root: &Path, name: &str) -> PathBuf {
        let path = root.join(STATE_DIR).join(name);
        fs::write(&path, b"a launcher copy put aside").unwrap();
        path
    }

    /// The journal, at `phase`, of an update of `fixture`'s installation to [`NEXT`], as the update
    /// writes one for this platform.
    fn update_journal(fixture: &InstallFixture, phase: &str) -> Value {
        let platform = this_platform();
        let random = STAGING.strip_prefix(UPDATE_STAGING_PREFIX).unwrap();
        let mut receipt = fixture.receipt();
        receipt["version"] = json!(NEXT);
        json!({
            "schema": 1,
            "minLauncher": MIN_LAUNCHER,
            "phase": phase,
            "from": INSTALLED,
            "to": NEXT,
            "staging": STAGING,
            "backup": ["VERSION"],
            "publish": ["VERSION"],
            "madeFolders": [],
            "launcher": installed_launcher_name(platform),
            "retired": retired(platform, INSTALLED, random),
            "receipt": receipt,
        })
    }

    /// Writes `journal` as the journal `name` of the installation at `root`.
    fn write_journal(root: &Path, name: &str, journal: &[u8]) {
        let folder = root.join(STATE_DIR).join(JOURNAL_DIR);
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join(name), journal).unwrap();
    }

    /// A sweep that removed `removed` and kept `kept`, the lock free.
    fn swept(removed: &[&str], kept: &[&str]) -> RetiredSweep {
        let names = |names: &[&str]| names.iter().map(|&name| name.to_owned()).collect();
        RetiredSweep {
            removed: names(removed),
            kept: names(kept),
            lock_busy: false,
        }
    }

    #[test]
    fn retired_copies_no_journal_names_are_removed() {
        let (_dir, fixture, folder) = installed();
        let platform = this_platform();
        let first = retired(platform, "0.9.0", "Zz9Yy8Xx");
        let second = retired(platform, INSTALLED, "AbC123xy");
        put_aside(&fixture.root, &first);
        put_aside(&fixture.root, &second);
        let mut both = [first.as_str(), second.as_str()];
        both.sort_unstable();
        let sweep = sweep_retired(&folder, Duration::ZERO, super::RETIRED_DELETE_INTERVAL).unwrap();
        assert_eq!(sweep, swept(&both, &[]));
        for name in both {
            assert!(!fixture.root.join(STATE_DIR).join(name).exists(), "{name}");
        }
        // Nothing is left to sweep.
        let sweep = sweep_retired(&folder, Duration::ZERO, super::RETIRED_DELETE_INTERVAL).unwrap();
        assert_eq!(sweep, swept(&[], &[]));
    }

    #[test]
    fn a_held_retired_copy_is_left_then_removed() {
        let (_dir, fixture, folder) = installed();
        let name = retired(this_platform(), INSTALLED, "AbC123xy");
        let path = put_aside(&fixture.root, &name);
        let held = hold_from_removal(&path);
        let begun = Instant::now();
        let sweep = sweep_retired(&folder, HELD_WINDOW, super::RETIRED_DELETE_INTERVAL).unwrap();
        if cfg!(windows) {
            // A handle that shares no deletion keeps the file, as a running launcher's image does,
            // through the whole window.
            assert_eq!(sweep, swept(&[], &[&name]));
            assert!(begun.elapsed() >= HELD_WINDOW, "{:?}", begun.elapsed());
            assert!(path.exists());
            drop(held);
            let sweep =
                sweep_retired(&folder, HELD_WINDOW, super::RETIRED_DELETE_INTERVAL).unwrap();
            assert_eq!(sweep, swept(&[&name], &[]));
        } else {
            // Linux removes a file that is open, as it removes a running program's file.
            assert_eq!(sweep, swept(&[&name], &[]));
            drop(held);
        }
        assert!(!path.exists());
    }

    #[cfg(windows)]
    #[test]
    fn a_retired_copy_let_go_within_the_window_is_removed_by_the_same_sweep() {
        /// How long into the sweep's window the copy is held, as an old launcher still ending
        /// holds its image: a few of the sweep's tries.
        const HELD_FOR: Duration = Duration::from_millis(300);

        // The new copy sweeps as soon as the old one exited, and Windows may still hold the old
        // one's image a moment: the sweep tries again through its window rather than once.
        let (_dir, fixture, folder) = installed();
        let name = retired(this_platform(), INSTALLED, "AbC123xy");
        let path = put_aside(&fixture.root, &name);
        let held = hold_from_removal(&path);
        let letting_go = thread::spawn(move || {
            thread::sleep(HELD_FOR);
            drop(held);
        });
        let begun = Instant::now();
        let sweep = sweep_retired(
            &folder,
            super::RETIRED_DELETE_WINDOW,
            super::RETIRED_DELETE_INTERVAL,
        )
        .unwrap();
        let took = begun.elapsed();
        letting_go.join().unwrap();
        assert_eq!(sweep, swept(&[&name], &[]));
        assert!(!path.exists());
        // It ended once the copy went, not with its window.
        assert!(took < super::RETIRED_DELETE_WINDOW, "{took:?}");
    }

    /// Opens the file at `path` as a running launcher's image holds its file on Windows, sharing no
    /// deletion; on Linux as any program holds a file.
    fn hold_from_removal(path: &Path) -> fs::File {
        let mut options = fs::OpenOptions::new();
        options.read(true);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;

            /// FILE_SHARE_READ: what a handle that lets others only read beside it shares.
            const SHARE_READ: u32 = 0x1;
            options.share_mode(SHARE_READ);
        }
        options.open(path).unwrap()
    }

    #[test]
    fn the_sweep_leaves_what_a_journal_names_and_names_not_its_own() {
        let (_dir, fixture, folder) = installed();
        let platform = this_platform();
        let random = STAGING.strip_prefix(UPDATE_STAGING_PREFIX).unwrap();
        // The two copies an update's journal names: the installed copy's retired name, and the one
        // the new copy goes to were the update taken back.
        let named = retired(platform, INSTALLED, random);
        let new_copy = retired(platform, NEXT, random);
        let unnamed = retired(platform, "0.9.0", "Zz9Yy8Xx");
        // Names that are not this launcher's retired copies for this platform, and a folder at a
        // retired copy's name.
        let not_its_own = [
            retired_n64(),
            "retired-launcher-x".to_owned(),
            retired(other_platform(), "0.9.0", "Qw3rTy12"),
            format!("{}.txt", retired(platform, "0.9.0", "Qw3rTy12")),
        ];
        let folder_named = retired(platform, "0.9.1", "Pp0Oo9Ii");
        for name in [&named, &new_copy, &unnamed]
            .into_iter()
            .chain(&not_its_own)
        {
            put_aside(&fixture.root, name);
        }
        fs::create_dir(fixture.root.join(STATE_DIR).join(&folder_named)).unwrap();
        write_journal(
            &fixture.root,
            UPDATE_JOURNAL,
            &pretty_json(&update_journal(&fixture, "committed")),
        );
        let sweep = sweep_retired(&folder, Duration::ZERO, super::RETIRED_DELETE_INTERVAL).unwrap();
        let mut kept = [named.as_str(), new_copy.as_str()];
        kept.sort_unstable();
        assert_eq!(sweep, swept(&[&unnamed], &kept));
        let state = fixture.root.join(STATE_DIR);
        for name in kept
            .iter()
            .copied()
            .chain(not_its_own.iter().map(String::as_str))
        {
            assert!(state.join(name).is_file(), "{name}");
        }
        assert!(state.join(&folder_named).is_dir());
        assert!(!state.join(&unnamed).exists());

        // A journal that cannot be read as an update's, or a journal of a kind this launcher cannot
        // tell the names of, may name any copy, and every one stays.
        put_aside(&fixture.root, &unnamed);
        let unreadable: [(&str, &[u8]); 3] = [
            (UPDATE_JOURNAL, b"{ not json"),
            (
                UPDATE_JOURNAL,
                br#"{"schema": 2, "minLauncher": "1.0.0-beta"}"#,
            ),
            ("expansion-n64.json", b"{}"),
        ];
        for (name, journal) in unreadable {
            fs::remove_dir_all(state.join(JOURNAL_DIR)).unwrap();
            write_journal(&fixture.root, name, journal);
            let sweep =
                sweep_retired(&folder, Duration::ZERO, super::RETIRED_DELETE_INTERVAL).unwrap();
            let mut all = [named.as_str(), new_copy.as_str(), unnamed.as_str()];
            all.sort_unstable();
            assert_eq!(sweep, swept(&[], &all), "{name}");
        }

        // A fresh install's journal names none, and a journal's pending file is no journal.
        fs::remove_dir_all(state.join(JOURNAL_DIR)).unwrap();
        write_journal(&fixture.root, "install.json", b"{}");
        write_journal(&fixture.root, "update.json.pending", b"{ cut sho");
        let sweep = sweep_retired(&folder, Duration::ZERO, super::RETIRED_DELETE_INTERVAL).unwrap();
        let mut all = [named.as_str(), new_copy.as_str(), unnamed.as_str()];
        all.sort_unstable();
        assert_eq!(sweep, swept(&all, &[]));
        for name in not_its_own.iter() {
            assert!(state.join(name).is_file(), "{name}");
        }
    }

    #[test]
    fn a_busy_lock_defers_the_sweep() {
        let (_dir, fixture, folder) = installed();
        let name = retired(this_platform(), INSTALLED, "AbC123xy");
        let path = put_aside(&fixture.root, &name);
        let lock = folder.lock().unwrap();
        let sweep = sweep_retired(&folder, HELD_WINDOW, super::RETIRED_DELETE_INTERVAL).unwrap();
        assert_eq!(
            sweep,
            RetiredSweep {
                removed: Vec::new(),
                kept: Vec::new(),
                lock_busy: true,
            }
        );
        assert!(path.is_file());
        drop(lock);
        let sweep = sweep_retired(&folder, HELD_WINDOW, super::RETIRED_DELETE_INTERVAL).unwrap();
        assert_eq!(sweep, swept(&[&name], &[]));
    }

    #[test]
    fn an_installation_without_a_state_folder_has_nothing_to_sweep() {
        let dir = tempfile::tempdir().unwrap();
        let folder = InstallFolder::at(dir.path()).unwrap();
        let sweep = sweep_retired(&folder, HELD_WINDOW, super::RETIRED_DELETE_INTERVAL).unwrap();
        assert_eq!(sweep, swept(&[], &[]));
        assert!(!dir.path().join(STATE_DIR).exists());
    }

    /// A launcher file for this platform: a tiny program of its kind, carrying a runtime.
    fn launcher_file() -> Vec<u8> {
        let platform = this_platform();
        let mut file = match platform {
            Platform::WindowsX64 => tiny_pe(),
            Platform::LinuxX64 => tiny_elf(),
        };
        let runtime = RuntimeFiles::standard(platform.as_str(), INSTALLED).zip();
        append_sections(&mut file, &[(RUNTIME_SECTION, &runtime)], 0);
        file
    }

    #[test]
    fn the_installed_copy_is_known_by_its_file_not_its_name() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("Gauntlet Dark Legacy");
        let elsewhere = dir.path().join("Downloads");
        fs::create_dir(&root).unwrap();
        fs::create_dir(&elsewhere).unwrap();
        let platform = this_platform();
        let copy = root.join(installed_launcher_name(platform));
        fs::write(&copy, launcher_file()).unwrap();

        // The installed copy, its sections found as a launcher finds its own as it starts.
        let sections = Sections::of_path(&copy).unwrap().unwrap();
        assert!(is_installed_copy(sections.file(), &root, platform).unwrap());
        // The same file asked about through another spelling of its path: in upper case, which
        // Windows' file systems take for the same names; through a hard link's path on Linux.
        #[cfg(windows)]
        {
            let upper = PathBuf::from(root.to_string_lossy().to_uppercase());
            assert!(is_installed_copy(sections.file(), &upper, platform).unwrap());
        }
        #[cfg(unix)]
        {
            let linked = elsewhere.join("linked");
            fs::hard_link(&copy, &linked).unwrap();
            let own = File::open(&linked).unwrap();
            assert!(is_installed_copy(&own, &root, platform).unwrap());
        }
        // A copy of the same bytes elsewhere, as a downloaded launcher is, is another file.
        let version = Version::parse(INSTALLED).unwrap();
        let downloaded = elsewhere.join(launcher_asset(&version, platform));
        fs::copy(&copy, &downloaded).unwrap();
        let own = File::open(&downloaded).unwrap();
        assert!(!is_installed_copy(&own, &root, platform).unwrap());
        // An installation without a launcher copy has none for the file to be.
        assert!(!is_installed_copy(sections.file(), &elsewhere, platform).unwrap());
        // Once an update has put another file at the copy's name, the file opened before is no
        // longer the installed copy, though it is still the same file.
        let retired_copy = dir.path().join("retired");
        fs::rename(&copy, &retired_copy).unwrap();
        fs::copy(&downloaded, &copy).unwrap();
        assert!(!is_installed_copy(sections.file(), &root, platform).unwrap());
        drop(sections);

        // A link at the copy's name is not the file it leads to.
        let linked_root = dir.path().join("Linked");
        fs::create_dir(&linked_root).unwrap();
        let link = linked_root.join(installed_launcher_name(platform));
        if !symlink_file_where_allowed(&link, &retired_copy) {
            eprintln!("skipped: this account may not make a symbolic link to a file");
            return;
        }
        let own = File::open(&retired_copy).unwrap();
        assert!(!is_installed_copy(&own, &linked_root, platform).unwrap());
    }

    #[test]
    fn a_hand_over_is_needed_by_the_installed_copy_or_an_older_file() {
        let older = Version::parse(INSTALLED).unwrap();
        let installed = Version::parse(NEXT).unwrap();
        // The installed copy hands over, older or not: the copy at its name is another file now.
        assert!(hand_over_needed(true, &installed, &installed));
        assert!(hand_over_needed(true, &older, &installed));
        // Another file hands over when it is older than what it installed, and not otherwise.
        assert!(hand_over_needed(false, &older, &installed));
        assert!(!hand_over_needed(false, &installed, &installed));
    }

    #[test]
    fn the_hand_over_command_starts_its_program_by_its_path_in_the_root() {
        let dir = installation();
        let root = dir.path();
        let program = root.join(installed_launcher_name(this_platform()));
        let args = [OsString::from(AFTER_UPDATE_OPTION), OsString::from("4242")];
        let command = handover_command(&program, root, &args).unwrap();
        assert_eq!(command.get_program(), program.as_os_str());
        let given: Vec<_> = command.get_args().collect();
        assert_eq!(
            given,
            args.iter().map(OsString::as_os_str).collect::<Vec<_>>()
        );
        assert_eq!(command.get_current_dir(), Some(root));
        assert_eq!(command.get_envs().count(), 0);
        // Which folder a relative path would be found from is not certain.
        let relative = Path::new(installed_launcher_name(this_platform()));
        let error = handover_command(relative, root, &args).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{error}");
    }

    #[cfg(windows)]
    #[test]
    fn a_hand_over_without_a_console_makes_no_window() {
        use windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;

        // The command takes `handover_flags(platform::has_console())`; std's command gives no way
        // to read its flags back, so the rule is held here.
        assert_eq!(
            super::handover_flags(false) & CREATE_NO_WINDOW,
            CREATE_NO_WINDOW
        );
        assert_eq!(super::handover_flags(true) & CREATE_NO_WINDOW, 0);
    }

    #[cfg(windows)]
    #[test]
    fn a_hand_over_under_a_filtered_token_renames_waits_and_removes() {
        use windows_sys::Win32::Foundation::ERROR_PRIVILEGE_NOT_HELD;

        // As a standard user's session runs it: the old copy, and the new one it starts, which
        // inherits its token, hold the Administrators group only to deny and run at Medium
        // integrity, as UAC filters an administrator's token; the running copy's rename, the wait
        // for it and the removal of its retired file go through all the same.
        let _one = crate::one_test_child();
        let dir = installation();
        let root = dir.path();
        let copy = launcher_copy_in(root);
        let retired_name = retired(this_platform(), "0.9.0", "AbC123xy");
        let logs = tempfile::tempdir().unwrap();
        let log = logs.path().join(HANDOVER_LOG);
        let output = fs::OpenOptions::new()
            .append(true)
            .create_new(true)
            .open(&log)
            .unwrap();
        let args = ["--exact", FILTERED_OLD_COPY, "--test-threads", "1"].map(OsString::from);
        let env = [
            (OsString::from(FILTERED), OsString::from(&retired_name)),
            (OsString::from(FILTERED_LOG), log.clone().into_os_string()),
        ];
        let old_copy = match crate::platform::spawn_filtered(&copy, &args, &env, &output) {
            Ok(old_copy) => old_copy,
            Err(error) if error.raw_os_error() == Some(ERROR_PRIVILEGE_NOT_HELD.cast_signed()) => {
                eprintln!("skipped: this account may not start a program under another token");
                remove_copy(&copy);
                return;
            }
            Err(error) => panic!("{}: {error}", copy.display()),
        };
        drop(output);
        let ended = old_copy.wait(CHILD_DEADLINE);
        // Held from here on, so that a failure below still ends the new copy.
        let new_copy = NewCopy::named_in(&log_lines(&log), &copy);
        let code = ended.unwrap_or_else(|error| panic!("{error}: {:#?}", log_lines(&log)));
        assert_eq!(code, 0, "{:#?}", log_lines(&log));
        assert!(
            new_copy.ended_within(CHILD_DEADLINE),
            "{:#?}",
            log_lines(&log)
        );
        drop(new_copy);

        let lines = log_lines(&log);
        for role in [OLD_COPY, NEW_COPY] {
            assert_eq!(
                line_after(&lines, &format!("{role}{ADMINISTRATORS}")),
                "false"
            );
            assert_eq!(
                line_after(&lines, &format!("{role}{INTEGRITY}")),
                crate::platform::MEDIUM_INTEGRITY.to_string()
            );
        }
        // The new copy waited for the old one while it still ran, not for one already gone.
        let waited: u64 = line_after(&lines, &format!("{NEW_COPY}{ANSWER}"))
            .strip_prefix(EXITED)
            .and_then(|millis| millis.parse().ok())
            .unwrap_or_else(|| panic!("{lines:#?}"));
        assert!(
            Duration::from_millis(waited) >= LEAST_WAITED,
            "waited {waited} ms: {lines:#?}"
        );
        assert_eq!(
            line_after(&lines, &format!("{NEW_COPY}{SWEPT}")),
            format!("removed [{retired_name:?}], kept [], lockBusy false")
        );
        // The new copy stands at the launcher's name, and no retired copy is left.
        assert_eq!(
            fs::read(&copy).unwrap(),
            fs::read(env::current_exe().unwrap()).unwrap()
        );
        let state = root.join(STATE_DIR);
        let left: Vec<_> = fs::read_dir(&state)
            .unwrap()
            .map(|entry| entry.unwrap().file_name())
            .filter(|name| name.to_string_lossy().starts_with("retired-"))
            .collect();
        assert!(left.is_empty(), "{left:?}");
        remove_copy(&copy);
    }

    /// The lines of the log at `log` so far, blank ones left out.
    #[cfg(windows)]
    fn log_lines(log: &Path) -> Vec<String> {
        String::from_utf8_lossy(&fs::read(log).unwrap_or_default())
            .lines()
            .filter(|line| !line.is_empty())
            .map(str::to_owned)
            .collect()
    }

    /// What follows `start` on the first of `lines` that starts with it.
    #[cfg(windows)]
    #[track_caller]
    fn line_after<'a>(lines: &'a [String], start: &str) -> &'a str {
        lines
            .iter()
            .find_map(|line| line.strip_prefix(start))
            .unwrap_or_else(|| panic!("no line {start:?}: {lines:#?}"))
    }

    /// The new copy of a filtered hand-over, opened by the id the old copy gave, while it runs
    /// from the installation's launcher copy: given a moment to end and ended otherwise when
    /// dropped, so that none outlives its test.
    #[cfg(windows)]
    struct NewCopy(Option<crate::platform::ProcessHandle>);

    #[cfg(windows)]
    impl NewCopy {
        /// The new copy the old copy named among `lines` as the one it started, when it still runs
        /// from `copy`: none when the old copy named none, when no process has the id, or when the
        /// one that has it runs another file, as a process that took the id after the copy ended
        /// does.
        fn named_in(lines: &[String], copy: &Path) -> NewCopy {
            use crate::platform::{OpenedProcess, open_process, process_image};

            let started = format!("{OLD_COPY}{HANDED_OVER}");
            let pid = lines
                .iter()
                .find_map(|line| line.strip_prefix(&started)?.parse().ok());
            let Some(Ok(OpenedProcess::Open(process))) = pid.map(open_process) else {
                return NewCopy(None);
            };
            let runs_the_copy = process_image(&process).is_ok_and(|image| {
                super::same_path(
                    &strip_verbatim(&super::image_in_full(&image)),
                    &strip_verbatim(&super::in_full(copy)),
                )
            });
            NewCopy(runs_the_copy.then_some(process))
        }

        /// Whether it has ended within `within`, or was not running.
        fn ended_within(&self, within: Duration) -> bool {
            self.0.as_ref().is_none_or(|process| {
                crate::platform::wait_process(process, within).unwrap_or(false)
            })
        }
    }

    #[cfg(windows)]
    impl Drop for NewCopy {
        fn drop(&mut self) {
            if let Some(process) = &self.0
                && !self.ended_within(NEW_COPY_END)
            {
                let _ = crate::platform::end_process(process, super::ENDED_EXIT_CODE);
                let _ = crate::platform::wait_process(process, NEW_COPY_END);
            }
        }
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_handed_over_child_sees_only_its_standard_descriptors_and_runs_in_the_root() {
        let _one = crate::one_test_child();
        let dir = installation();
        let root = dir.path();
        // A descriptor this process holds without close-on-exec, as a library may leave one, which
        // a program started from here would otherwise inherit.
        let held = tempfile::tempfile().unwrap();
        let leaked = crate::platform::leak_descriptor(&held).unwrap();
        let args = ["--exact", REPORTER, "--test-threads", "1"].map(OsString::from);
        let command = handover_command(&env::current_exe().unwrap(), root, &args).unwrap();
        let mut child = Started::spawn(command);
        let descriptors = child.line_starting(DESCRIPTORS);
        assert_eq!(
            &descriptors[DESCRIPTORS.len()..],
            STANDARD_DESCRIPTORS,
            "{:?}",
            child.seen
        );
        let input = child.line_starting(INPUT);
        assert_eq!(&input[INPUT.len()..], NOTHING_TO_READ);
        let folder = child.line_starting(FOLDER);
        assert_eq!(
            Path::new(&folder[FOLDER.len()..]),
            fs::canonicalize(root).unwrap()
        );
        let status = child.ended();
        assert!(status.success(), "{status}: {:?}", child.seen);
        drop(leaked);
    }
}
