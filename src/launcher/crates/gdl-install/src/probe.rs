//! Probes of an installation's game, made before the launcher changes the installation: whether
//! the game is running from it, so that no file is changed or replaced under a running game, and
//! whether the system runs the game's executable at all, which the game shows by printing its
//! version.

use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender};
use std::thread;
use std::time::{Duration, Instant};

use gdl_files::{Cancel, RETRY_INTERVAL, RETRY_WINDOW};

#[cfg(target_os = "linux")]
use gdl_files::is_absent;

#[cfg(any(windows, target_os = "linux"))]
use crate::platform;

/// How long the game is given to answer when it is asked for its version.
pub const GAME_PROBE_LIMIT: Duration = Duration::from_secs(30);

/// The most bytes kept of each of a probed program's output streams: the first of its standard
/// output, which holds its answer, and the last of its standard error, where a program that fails
/// says why last.
pub const MAX_PROBE_OUTPUT: usize = 64 << 10;

/// The argument that asks the game for its version, which it prints alone on its first line before
/// it ends, and before any window opens.
const VERSION_ARGUMENT: &str = "--version";

/// How often a probe looks whether its program has ended.
const POLL_INTERVAL: Duration = Duration::from_millis(50);

/// How long a program's output is still read for once the program has ended. What it wrote is read
/// at once, unless a program it started holds its pipes open, which a probe does not wait for.
const DRAIN_LIMIT: Duration = Duration::from_secs(1);

/// How many bytes a probe reads from a pipe at a time.
const READ_LEN: usize = 8 << 10;

/// How many chunks read from a program's pipes may wait to be kept, so that what a probe holds
/// stays bounded however fast the program writes.
const CHUNKS_IN_FLIGHT: usize = 16;

/// The most descriptors a probed program's start marks one at a time, where the kernel cannot
/// mark them all in one call: it follows the soft limit on open files up to this many.
#[cfg(target_os = "linux")]
pub(crate) const MAX_MARKED_DESCRIPTORS: u64 = 65_536;

/// The share mode of an open that lets no other handle on the file exist beside it.
#[cfg(windows)]
const NO_SHARING: u32 = 0;

/// What Windows refuses an open with when nothing is at the path.
#[cfg(windows)]
const ERROR_FILE_NOT_FOUND: i32 = 2;

/// What Windows refuses an open with when a folder on the path is missing, or is a file.
#[cfg(windows)]
const ERROR_PATH_NOT_FOUND: i32 = 3;

/// What Windows refuses an open to write with when the file is read-only or the account may not
/// write it, and when a folder is opened as a file.
#[cfg(windows)]
const ERROR_ACCESS_DENIED: i32 = 5;

/// What Windows refuses an open that shares nothing with while another handle holds the file, as
/// the system holds a running program's executable.
#[cfg(windows)]
const ERROR_SHARING_VIOLATION: i32 = 32;

/// Where Linux lists its processes, a folder for each, named by its id.
#[cfg(target_os = "linux")]
const PROCESSES: &str = "/proc";

/// The link in a process's folder to the file the process runs.
#[cfg(target_os = "linux")]
const PROCESS_EXECUTABLE: &str = "exe";

/// Checks that the game whose executable is `executable` is closed, so that the launcher never
/// changes or replaces an installation's files under a running game.
///
/// On Windows the executable is opened to write, sharing nothing, which the system refuses while
/// any handle holds the file, as it holds a running program's: [`GameUseError::Running`]. An
/// executable that may not be opened to write at all is [`GameUseError::NotWritable`] instead, so
/// that a read-only file is never waited on as though the game would close. On Linux, where a
/// running program's file can be renamed and replaced, every process this account may look at is
/// asked which file it runs, and one that runs the executable's device and inode is the game; a
/// process that ends meanwhile, or that belongs to another account, is passed over. On both
/// systems, nothing at the path, or a folder there, is [`GameUseError::Missing`].
pub fn require_game_closed(executable: &Path) -> Result<(), GameUseError> {
    require_closed(executable)
}

/// Whether anything holds the executable, found by opening it to write while sharing nothing.
#[cfg(windows)]
fn require_closed(executable: &Path) -> Result<(), GameUseError> {
    use std::fs::OpenOptions;
    use std::os::windows::fs::OpenOptionsExt;

    let refusal = match OpenOptions::new()
        .write(true)
        .share_mode(NO_SHARING)
        .open(executable)
    {
        // Closed at once: the open only asked whether anything else holds the file.
        Ok(_) => return Ok(()),
        Err(refusal) => refusal,
    };
    Err(match refusal.raw_os_error() {
        Some(ERROR_SHARING_VIOLATION) => GameUseError::Running,
        Some(ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND) => GameUseError::Missing,
        Some(ERROR_ACCESS_DENIED) if is_folder(executable) => GameUseError::Missing,
        Some(ERROR_ACCESS_DENIED) => GameUseError::NotWritable {
            path: executable.to_path_buf(),
        },
        _ => GameUseError::Io {
            path: executable.to_path_buf(),
            source: refusal,
        },
    })
}

/// Whether a folder is at `path`, through a link to one too, as an open of the path follows it.
#[cfg(windows)]
fn is_folder(path: &Path) -> bool {
    std::fs::metadata(path).is_ok_and(|metadata| metadata.is_dir())
}

/// Whether a process runs the executable, found by asking every process this account may look at
/// which file it runs.
#[cfg(target_os = "linux")]
fn require_closed(executable: &Path) -> Result<(), GameUseError> {
    use std::fs;
    use std::os::unix::fs::MetadataExt;

    let game = match fs::metadata(executable) {
        Ok(metadata) if metadata.is_file() => metadata,
        Ok(_) => return Err(GameUseError::Missing),
        Err(error) if is_absent(&error) => return Err(GameUseError::Missing),
        Err(source) => {
            return Err(GameUseError::Io {
                path: executable.to_path_buf(),
                source,
            });
        }
    };
    let processes = Path::new(PROCESSES);
    let unlisted = |source| GameUseError::Io {
        path: processes.to_path_buf(),
        source,
    };
    for entry in fs::read_dir(processes).map_err(unlisted)? {
        let entry = entry.map_err(unlisted)?;
        if !is_process_id(&entry.file_name()) {
            continue;
        }
        let runs = entry.path().join(PROCESS_EXECUTABLE);
        match fs::metadata(&runs) {
            Ok(metadata) if (metadata.dev(), metadata.ino()) == (game.dev(), game.ino()) => {
                return Err(GameUseError::Running);
            }
            Ok(_) => {}
            Err(error) if is_passed_over(&error) => {}
            Err(source) => return Err(GameUseError::Io { path: runs, source }),
        }
    }
    Ok(())
}

/// Whether `name`, an entry of [`PROCESSES`], names a process: its id, in decimal digits.
#[cfg(target_os = "linux")]
fn is_process_id(name: &std::ffi::OsStr) -> bool {
    let name = name.as_encoded_bytes();
    !name.is_empty() && name.iter().all(u8::is_ascii_digit)
}

/// Whether asking a process which file it runs failed only because the process ended meanwhile,
/// or because it belongs to another account, which this one may not look into.
#[cfg(target_os = "linux")]
fn is_passed_over(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::NotFound | io::ErrorKind::PermissionDenied
    ) || error.raw_os_error() == Some(libc::ESRCH)
}

/// Whether the game is running cannot be told on this system, which lists no process's file, so
/// it is never taken to be closed.
#[cfg(not(any(windows, target_os = "linux")))]
fn require_closed(executable: &Path) -> Result<(), GameUseError> {
    Err(GameUseError::Io {
        path: executable.to_path_buf(),
        source: io::Error::new(
            io::ErrorKind::Unsupported,
            "this system lists no process's executable",
        ),
    })
}

/// What a program answered when it was probed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VersionProbe {
    /// How the program ended.
    pub status: ExitStatus,
    /// The first line the program wrote to its standard output, without its line ending and with
    /// any bytes that are not UTF-8 replaced, or `None` when it wrote nothing there. Only the first
    /// [`MAX_PROBE_OUTPUT`] bytes are read for it.
    pub first_line: Option<String>,
    /// The last [`MAX_PROBE_OUTPUT`] bytes the program wrote to its standard error, with any bytes
    /// that are not UTF-8 replaced: where a program that fails, or that the system cannot load,
    /// says why.
    pub stderr_tail: String,
}

impl VersionProbe {
    /// Whether the program ended with exit code 0, as a game that answers does.
    pub fn succeeded(&self) -> bool {
        self.status.success()
    }
}

/// Runs the game whose executable is `executable` with `--version`, which it answers with its
/// version alone on its first line before any window opens, to learn whether the system runs the
/// game at all: [`probe_game_version_with`] for a probe nobody cancels.
pub fn probe_game_version(executable: &Path) -> Result<VersionProbe, GameStartError> {
    probe_game_version_with(executable, &Cancel::default())
}

/// Runs the game whose executable is `executable` with `--version`, as [`probe_game_version`]
/// does, until `cancel` asks the probe to stop.
///
/// The path must be absolute, since the game is run by it and never looked for along `PATH`. It
/// runs as a program, not as a command for a shell; only a batch file, which an installation's
/// executable never is, would Windows run through `cmd.exe`, and on Linux a file that holds no
/// program is refused rather than run as a script. The game runs in its own folder, with nothing
/// to read; on Windows without a console window, and with the system's error dialogs turned into
/// errors, so that a game that cannot load ends at once rather than wait on a dialog nobody sees;
/// on Linux with no descriptor of the launcher's past its standard streams. A start that Linux
/// refuses because the file is open to write somewhere, as for a moment after it was written, is
/// tried again for the time the launcher retries a refused file operation for. The game is given
/// [`GAME_PROBE_LIMIT`] to end; past that it is stopped and the probe is
/// [`GameStartError::TimedOut`]. A cancel is looked at before each start and while the game runs,
/// as often as the probe looks whether it has ended; the game is then stopped and the probe is
/// [`GameStartError::Cancelled`]. Of what the game writes, the first [`MAX_PROBE_OUTPUT`] bytes of
/// its standard output are kept, for its first line, and the last as many of its standard error;
/// the rest is read and dropped, so that the game never waits on a full pipe.
pub fn probe_game_version_with(
    executable: &Path,
    cancel: &Cancel,
) -> Result<VersionProbe, GameStartError> {
    run_probe(executable, &[VERSION_ARGUMENT], GAME_PROBE_LIMIT, cancel)
}

/// Runs `program` with `args` as the game is run when it is asked for its version, given `limit`
/// to end, until `cancel` asks it to stop.
pub(crate) fn run_probe(
    program: &Path,
    args: &[&str],
    limit: Duration,
    cancel: &Cancel,
) -> Result<VersionProbe, GameStartError> {
    let mut command = probe_command(program, args)?;
    #[cfg(windows)]
    platform::fail_critical_errors();
    let mut child = start(&mut command, cancel)?;
    let deadline = Instant::now() + limit;
    match follow(&mut child, deadline, cancel) {
        Ok(Followed::Ended(probe)) => Ok(probe),
        Ok(Followed::TimedOut) => {
            stop(&mut child)?;
            Err(GameStartError::TimedOut)
        }
        Ok(Followed::Cancelled) => {
            stop(&mut child)?;
            Err(GameStartError::Cancelled)
        }
        Err(error) => {
            // A probe that fails never leaves its program running.
            let _ = stop(&mut child);
            Err(error)
        }
    }
}

/// Starts `command`, looking at `cancel` before each attempt, and tries again every
/// [`RETRY_INTERVAL`] for up to [`RETRY_WINDOW`] while the system refuses the program for
/// [`is_text_busy`].
fn start(command: &mut Command, cancel: &Cancel) -> Result<Child, GameStartError> {
    let begun = Instant::now();
    loop {
        cancel.check().map_err(|_| GameStartError::Cancelled)?;
        match command.spawn() {
            Ok(child) => return Ok(child),
            Err(error) if is_text_busy(&error) && begun.elapsed() < RETRY_WINDOW => {
                thread::sleep(RETRY_INTERVAL);
            }
            Err(error) => return Err(GameStartError::Start(error)),
        }
    }
}

/// Whether Linux refused to start a program because its file is open to write, which a moment's
/// wait cures: a file just written may still be held by a scanner, or by a program another thread
/// of this process is starting, which holds every descriptor the process has open for a moment.
#[cfg(target_os = "linux")]
fn is_text_busy(error: &io::Error) -> bool {
    error.raw_os_error() == Some(libc::ETXTBSY)
}

/// Whether the system refused to start a program because its file is open to write, which only
/// Linux refuses a start for.
#[cfg(not(target_os = "linux"))]
fn is_text_busy(_: &io::Error) -> bool {
    false
}

/// The command that runs `program` with `args` for a probe: by its absolute path, in its own
/// folder, with nothing to read and both output streams piped back; on Windows without a console
/// window, and on Linux with no descriptor past the standard three and never through a shell.
fn probe_command(program: &Path, args: &[&str]) -> Result<Command, GameStartError> {
    if !program.is_absolute() {
        return Err(GameStartError::NotAbsolute);
    }
    let Some(folder) = program.parent() else {
        // Only a root has no folder above it, and a root is no program.
        return Err(GameStartError::Start(io::Error::new(
            io::ErrorKind::InvalidInput,
            "a root folder is no program",
        )));
    };
    let mut command = Command::new(program);
    command
        .args(args)
        .current_dir(folder)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);
    }
    #[cfg(target_os = "linux")]
    platform::exec_directly(&mut command).map_err(GameStartError::Start)?;
    Ok(command)
}

/// How following a probed program ended.
enum Followed {
    /// The program ended, and answered this.
    Ended(VersionProbe),
    /// The program's time was up before it ended.
    TimedOut,
    /// The probe was asked to stop before the program ended.
    Cancelled,
}

/// Reads what `child` writes until it ends, and gives what it answered; or that `deadline`
/// passed, or `cancel` asked the probe to stop, first.
fn follow(
    child: &mut Child,
    deadline: Instant,
    cancel: &Cancel,
) -> Result<Followed, GameStartError> {
    let mut output = Output::start(child)?;
    loop {
        if let Some(status) = child.try_wait().map_err(GameStartError::Io)? {
            output.drain(Instant::now() + DRAIN_LIMIT)?;
            return Ok(Followed::Ended(output.into_probe(status)));
        }
        if cancel.is_cancelled() {
            return Ok(Followed::Cancelled);
        }
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return Ok(Followed::TimedOut);
        }
        output.take(left.min(POLL_INTERVAL))?;
    }
}

/// Stops `child` and waits until it is gone.
fn stop(child: &mut Child) -> Result<(), GameStartError> {
    child.kill().map_err(GameStartError::Io)?;
    child.wait().map(drop).map_err(GameStartError::Io)
}

/// What a probed program writes, read by a thread for each of its pipes and kept to its bounds.
struct Output {
    /// The chunks the threads send, in the order they read them.
    chunks: Receiver<Chunk>,
    /// Whether a thread may still send a chunk: none can once both have reached the end of their
    /// pipes.
    open: bool,
    /// The first bytes of the program's standard output.
    head: Vec<u8>,
    /// The last bytes of its standard error.
    tail: Vec<u8>,
}

/// One read from one of a probed program's pipes.
struct Chunk {
    /// The stream the pipe carries.
    stream: Stream,
    /// The bytes read, or why the read failed.
    bytes: io::Result<Vec<u8>>,
}

/// One of a program's output streams.
#[derive(Clone, Copy)]
enum Stream {
    /// Its standard output.
    Stdout,
    /// Its standard error.
    Stderr,
}

impl Output {
    /// Starts a thread reading each of `child`'s output pipes.
    fn start(child: &mut Child) -> Result<Output, GameStartError> {
        let (sender, chunks) = mpsc::sync_channel(CHUNKS_IN_FLIGHT);
        if let Some(pipe) = child.stdout.take() {
            spawn_reader(pipe, Stream::Stdout, sender.clone())?;
        }
        if let Some(pipe) = child.stderr.take() {
            spawn_reader(pipe, Stream::Stderr, sender)?;
        }
        Ok(Output {
            chunks,
            open: true,
            head: Vec::new(),
            tail: Vec::new(),
        })
    }

    /// Keeps what the threads send for up to `pause`, or waits `pause` out once both have ended.
    fn take(&mut self, pause: Duration) -> Result<(), GameStartError> {
        if !self.open {
            thread::sleep(pause);
            return Ok(());
        }
        match self.chunks.recv_timeout(pause) {
            Ok(chunk) => self.keep(chunk),
            Err(RecvTimeoutError::Timeout) => Ok(()),
            Err(RecvTimeoutError::Disconnected) => {
                self.open = false;
                Ok(())
            }
        }
    }

    /// Keeps what the threads send until both have ended, or until `until` passes.
    fn drain(&mut self, until: Instant) -> Result<(), GameStartError> {
        while self.open {
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            self.take(left)?;
        }
        Ok(())
    }

    /// Keeps what of `chunk` falls within its stream's bounds.
    fn keep(&mut self, chunk: Chunk) -> Result<(), GameStartError> {
        let bytes = chunk.bytes.map_err(GameStartError::Io)?;
        match chunk.stream {
            Stream::Stdout => keep_head(&mut self.head, &bytes, MAX_PROBE_OUTPUT),
            Stream::Stderr => keep_tail(&mut self.tail, &bytes, MAX_PROBE_OUTPUT),
        }
        Ok(())
    }

    /// What the program answered, now that it has ended with `status`.
    fn into_probe(self, status: ExitStatus) -> VersionProbe {
        VersionProbe {
            status,
            first_line: first_line(&self.head),
            stderr_tail: String::from_utf8_lossy(&self.tail).into_owned(),
        }
    }
}

/// Starts a thread that reads `pipe`, which carries `stream`, to its end and sends what it reads
/// through `chunks`.
fn spawn_reader(
    pipe: impl Read + Send + 'static,
    stream: Stream,
    chunks: SyncSender<Chunk>,
) -> Result<(), GameStartError> {
    thread::Builder::new()
        .spawn(move || read_pipe(pipe, stream, &chunks))
        .map(drop)
        .map_err(GameStartError::Io)
}

/// Reads `pipe`, which carries `stream`, to its end, sending each chunk it reads through `chunks`,
/// and stops early when a read fails or nothing receives the chunks any more.
fn read_pipe(mut pipe: impl Read, stream: Stream, chunks: &SyncSender<Chunk>) {
    let mut buffer = vec![0; READ_LEN];
    loop {
        let bytes = match pipe.read(&mut buffer) {
            Ok(0) => return,
            Ok(read) => Ok(buffer[..read].to_vec()),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => Err(error),
        };
        let failed = bytes.is_err();
        if chunks.send(Chunk { stream, bytes }).is_err() || failed {
            return;
        }
    }
}

/// Adds to `kept` what of `bytes` falls within the first `limit` bytes of a stream that `kept`
/// holds the start of.
fn keep_head(kept: &mut Vec<u8>, bytes: &[u8], limit: usize) {
    let room = limit.saturating_sub(kept.len());
    kept.extend_from_slice(&bytes[..bytes.len().min(room)]);
}

/// Adds `bytes` to `kept`, which holds the last `limit` bytes of a stream.
fn keep_tail(kept: &mut Vec<u8>, bytes: &[u8], limit: usize) {
    kept.extend_from_slice(bytes);
    let excess = kept.len().saturating_sub(limit);
    kept.drain(..excess);
}

/// The first line of `output`, without the line feed that ends it or a carriage return before
/// that, or `None` when `output` is empty.
fn first_line(output: &[u8]) -> Option<String> {
    if output.is_empty() {
        return None;
    }
    let line = match output.iter().position(|&byte| byte == b'\n') {
        Some(end) => &output[..end],
        None => output,
    };
    let line = line.strip_suffix(b"\r").unwrap_or(line);
    Some(String::from_utf8_lossy(line).into_owned())
}

/// Why the game counts as in use, or cannot be checked, as technical detail for logs. What a
/// player reads is chosen from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum GameUseError {
    /// The game is running from the installation.
    #[error("Close the game before changing its installation")]
    Running,
    /// Nothing is at the executable's path, or a folder is.
    #[error("The installed executable is missing")]
    Missing,
    /// The system refuses to open the executable at `path` to write: it is read-only, or the
    /// account may not write it. Only Windows asks.
    #[error("The installed executable is not writable: {}", .path.display())]
    NotWritable {
        /// The executable.
        path: PathBuf,
    },
    /// The system refused to look at `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The path looked at: the executable, or on Linux the list of processes or a process's
        /// link to its file.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
}

/// Why the game did not answer when it was asked for its version, as technical detail for logs.
/// What a player reads is chosen from it elsewhere.
#[derive(Debug, thiserror::Error)]
pub enum GameStartError {
    /// The executable's path is not absolute, so what it names would depend on the launcher's own
    /// working folder.
    #[error("The game's executable path is not absolute")]
    NotAbsolute,
    /// The system did not start the executable: it is not there, or not a program this system
    /// runs.
    #[error("The game did not start: {0}")]
    Start(#[source] io::Error),
    /// The game had not ended when its time was up, and was stopped.
    #[error("The game did not answer in time and was stopped")]
    TimedOut,
    /// The probe was asked to stop: the game was stopped, or never started.
    #[error("The game's version probe was cancelled")]
    Cancelled,
    /// The system failed to tell whether the game had ended, to stop it, or to give what it wrote.
    #[error("The game's run could not be followed: {0}")]
    Io(#[source] io::Error),
}

impl GameStartError {
    /// The failure's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        match self {
            GameStartError::NotAbsolute => "notAbsolute",
            GameStartError::Start(_) => "start",
            GameStartError::TimedOut => "timedOut",
            GameStartError::Cancelled => "cancelled",
            GameStartError::Io(_) => "io",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};
    use std::process::{Child, Command, Stdio};
    use std::thread;
    use std::time::{Duration, Instant};

    use gdl_files::{Cancel, RETRY_INTERVAL, RETRY_WINDOW};
    use gdl_fixtures::{folder_names, link_folder, symlink_file_where_allowed};
    #[cfg(windows)]
    use windows_sys::Win32::System::Diagnostics::Debug::SEM_FAILCRITICALERRORS;

    #[cfg(windows)]
    use super::NO_SHARING;
    use super::{
        DRAIN_LIMIT, GAME_PROBE_LIMIT, GameStartError, GameUseError, MAX_PROBE_OUTPUT,
        VERSION_ARGUMENT, VersionProbe, first_line, is_text_busy, keep_head, keep_tail,
        probe_game_version, probe_game_version_with, require_game_closed, run_probe,
    };
    #[cfg(windows)]
    use crate::platform::{error_mode, refuse_fault_dialogs};
    #[cfg(target_os = "linux")]
    use crate::platform::{exec_directly_marking_each, leak_descriptor};

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The name of the game's executable on this system.
    const EXECUTABLE: &str = if cfg!(windows) {
        "gauntlet.exe"
    } else {
        "gauntlet"
    };

    /// What a test's stand-in for the game holds: no real program.
    const GAME_BYTES: &[u8] = b"not a real executable";

    /// A system program that waits half a minute, unless it is stopped.
    #[cfg(windows)]
    const WAITER: &str = "PING.EXE";

    /// The arguments that make [`WAITER`] wait: thirty echo requests to this machine, a second
    /// apart.
    #[cfg(windows)]
    const WAITER_ARGS: &[&str] = &["-n", "30", "127.0.0.1"];

    /// A system program that waits half a minute, unless it is stopped.
    #[cfg(unix)]
    const WAITER: &str = "sleep";

    /// The arguments that make [`WAITER`] wait.
    #[cfg(unix)]
    const WAITER_ARGS: &[&str] = &["30"];

    /// How long [`WAITER`] runs when nothing stops it, at the least.
    const WAITER_RUN: Duration = Duration::from_secs(29);

    /// The time a probe that must be stopped is given.
    const SHORT_LIMIT: Duration = Duration::from_secs(1);

    /// How long a test lets a probed program run before it cancels the probe.
    const CANCEL_AFTER: Duration = Duration::from_millis(100);

    /// How soon a cancelled probe must have stopped its program and ended.
    const CANCELLED_WITHIN: Duration = Duration::from_secs(2);

    /// How soon a probe of a file that holds no program must have failed.
    const FAILS_WITHIN: Duration = Duration::from_secs(5);

    /// How long a test holds a program's file open to write while it is probed.
    #[cfg(target_os = "linux")]
    const BUSY_FOR: Duration = Duration::from_millis(300);

    /// How long a program left running by a probed one holds the probe's pipes, unless it is
    /// stopped: far longer than a probe reads a program's output for once the program has ended.
    const LEFT_RUNNING: Duration = Duration::from_secs(20);

    /// The arguments that make [`WAITER`] run for [`LEFT_RUNNING`]: an echo request a second,
    /// the first at once.
    #[cfg(windows)]
    const LEFT_RUNNING_ARGS: &[&str] = &["-n", "21", "127.0.0.1"];

    /// The arguments that make [`WAITER`] run for [`LEFT_RUNNING`].
    #[cfg(unix)]
    const LEFT_RUNNING_ARGS: &[&str] = &["20"];

    /// The name of a script that writes back the arguments it is run with.
    #[cfg(windows)]
    const ECHO_SCRIPT: &str = "answers.bat";

    /// What that script holds.
    #[cfg(windows)]
    const ECHO_SCRIPT_TEXT: &[u8] = b"@echo %*\r\n";

    /// The name of a script that writes back the arguments it is run with.
    #[cfg(unix)]
    const ECHO_SCRIPT: &str = "answers";

    /// What that script holds.
    #[cfg(unix)]
    const ECHO_SCRIPT_TEXT: &[u8] = b"#!/bin/sh\necho \"$@\"\n";

    /// The mode of a program its owner writes and anyone runs.
    #[cfg(unix)]
    const RUNNABLE: u32 = 0o755;

    /// How many bytes a test program writes to each of its output streams: more than a probe
    /// keeps of either.
    const BIG_OUTPUT: usize = 70_000;

    /// The line a test program writes first.
    const FIRST_LINE: &str = "1.0.0-beta";

    /// The line a test program writes last.
    const LAST_LINE: &[u8] = b"the last line\n";

    /// MAX_PATH: the most UTF-16 units a plain Windows path may hold, its NUL included.
    #[cfg(windows)]
    const MAX_PATH: usize = 260;

    /// A folder under an installation's root deep enough that the game's executable in it lies
    /// past MAX_PATH under the longest root: that of the deepest file the launcher stages.
    #[cfg(windows)]
    const DEEP_FOLDER: &str =
        ".gdl-launcher/stage-n64-AbC123xy/n64/extracted/native/levels/levelt1/rombackup_3_20_00";

    /// The share mode of a handle that lets anything else open the file beside it, std's own:
    /// FILE_SHARE_READ, FILE_SHARE_WRITE and FILE_SHARE_DELETE.
    #[cfg(windows)]
    const SHARING_EVERYTHING: u32 = 0x7;

    /// The variable that makes a run of this test program play its part in
    /// `critical_errors_reach_the_process`: the probe made from the system's default error mode,
    /// or the program it probes. Unset, as in a run of the tests, neither part does anything.
    #[cfg(windows)]
    const ERROR_MODE_CHILD: &str = "GDL_PROBE_ERROR_MODE_CHILD";

    /// The test that a child run of this program, started in the default error mode, runs alone.
    #[cfg(windows)]
    const PROBING_TEST: &str = "probe::tests::probing_from_the_default_error_mode";

    /// The test that a run of this program started by that child runs alone, to tell the error
    /// mode it started in.
    #[cfg(windows)]
    const REPORTING_TEST: &str = "probe::tests::reporting_the_error_mode";

    /// The exit code of a run that started with critical errors failing calls.
    #[cfg(windows)]
    const FAILS_CRITICAL_ERRORS: i32 = 72;

    /// The exit code of a run that started with critical errors shown as dialogs.
    #[cfg(windows)]
    const SHOWS_CRITICAL_ERRORS: i32 = 73;

    /// The exit code of a child run whose every check of the error mode held.
    #[cfg(windows)]
    const ERROR_MODE_CHECKED: i32 = 74;

    /// A program of the system's own, by its name in the folder that holds them.
    #[cfg(windows)]
    fn system_program(name: &str) -> PathBuf {
        let root = std::env::var_os("SystemRoot").expect("Windows names its folder in SystemRoot");
        PathBuf::from(root).join("System32").join(name)
    }

    /// A program of the system's own, by its name in the folder that holds them.
    #[cfg(unix)]
    fn system_program(name: &str) -> PathBuf {
        Path::new("/bin").join(name)
    }

    /// The system's shell and the arguments that make it run the command `words`, as one line.
    #[cfg(windows)]
    fn shell(words: &[&str]) -> (PathBuf, Vec<String>) {
        let mut args = vec!["/d".to_owned(), "/c".to_owned()];
        args.extend(words.iter().map(|&word| word.to_owned()));
        (system_program("cmd.exe"), args)
    }

    /// The system's shell and the arguments that make it run the command `words`, as one line.
    #[cfg(unix)]
    fn shell(words: &[&str]) -> (PathBuf, Vec<String>) {
        (system_program("sh"), vec!["-c".to_owned(), words.join(" ")])
    }

    /// The system's shell and the arguments that make it write the file at `path` to its standard
    /// output, then to its standard error.
    #[cfg(windows)]
    fn file_writer(path: &Path) -> (PathBuf, Vec<String>) {
        let path = path.to_str().unwrap();
        shell(&["type", path, "&", "type", path, ">&2"])
    }

    /// The system's shell and the arguments that make it write the file at `path` to its standard
    /// output, then to its standard error.
    #[cfg(unix)]
    fn file_writer(path: &Path) -> (PathBuf, Vec<String>) {
        let script = r#"cat "$1" && cat "$1" >&2"#;
        let args = ["-c", script, "sh", path.to_str().unwrap()];
        (
            system_program("sh"),
            args.iter().map(|&arg| arg.to_owned()).collect(),
        )
    }

    /// Writes `bytes` to `path` as a program this account may run.
    fn write_program(path: &Path, bytes: &[u8]) {
        fs::write(path, bytes).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(path, fs::Permissions::from_mode(RUNNABLE)).unwrap();
        }
    }

    /// Probes `program` with `args` as the launcher does, with a cancel nobody makes.
    fn probe(
        program: &Path,
        args: &[&str],
        limit: Duration,
    ) -> Result<VersionProbe, GameStartError> {
        run_probe(program, args, limit, &Cancel::default())
    }

    /// Probes the program `program` with `args`, which must answer.
    #[track_caller]
    fn answer_of(program: &Path, args: &[String]) -> VersionProbe {
        let args: Vec<&str> = args.iter().map(String::as_str).collect();
        probe(program, &args, GAME_PROBE_LIMIT)
            .unwrap_or_else(|error| panic!("{} {args:?}: {error}", program.display()))
    }

    /// Probes the system's shell running the command `words`, which must answer.
    #[track_caller]
    fn shell_answer(words: &[&str]) -> VersionProbe {
        let (program, args) = shell(words);
        answer_of(&program, &args)
    }

    /// A program a test started, stopped and waited for when this is dropped, so that a test that
    /// fails leaves nothing running.
    struct Started(Child);

    impl Drop for Started {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }

    /// Starts `program` with `args` as std starts a program, with nothing to read or write and on
    /// Windows without a console window, trying again while the file is refused for
    /// [`is_text_busy`].
    fn start(program: &Path, args: &[&str]) -> Started {
        let mut command = Command::new(program);
        command
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);
        }
        let begun = Instant::now();
        loop {
            match command.spawn() {
                Ok(child) => return Started(child),
                Err(error) if is_text_busy(&error) && begun.elapsed() < RETRY_WINDOW => {
                    thread::sleep(RETRY_INTERVAL);
                }
                Err(error) => panic!("{}: {error}", program.display()),
            }
        }
    }

    /// A copy in `dir` of [`WAITER`], which nothing but the test runs.
    fn copy_of_waiter(dir: &Path) -> PathBuf {
        let copy = dir.join(WAITER);
        fs::copy(system_program(WAITER), &copy).unwrap();
        copy
    }

    /// Checks that the game at `path` is found running.
    #[track_caller]
    fn assert_running(path: &Path) {
        match require_game_closed(path) {
            Err(GameUseError::Running) => {}
            other => panic!("{}: {other:?}", path.display()),
        }
    }

    /// Checks that the game at `path` is found missing.
    #[track_caller]
    fn assert_missing(path: &Path) {
        match require_game_closed(path) {
            Err(GameUseError::Missing) => {}
            other => panic!("{}: {other:?}", path.display()),
        }
    }

    /// Checks that the game at `path` is found closed once nothing else holds the file, within the
    /// time the launcher retries a file operation for: on Windows a scanner may hold a file that
    /// was just written or run for a moment, and the file counts as in use while it does.
    #[track_caller]
    fn assert_closed(path: &Path) {
        let begun = Instant::now();
        loop {
            match require_game_closed(path) {
                Ok(()) => return,
                Err(GameUseError::Running) if begun.elapsed() < RETRY_WINDOW => {
                    thread::sleep(RETRY_INTERVAL);
                }
                other => panic!("{}: {other:?}", path.display()),
            }
        }
    }

    /// Opens `path` to read, sharing as `share` says, once nothing holds it that shares nothing, as
    /// a scanner may for a moment.
    #[cfg(windows)]
    fn hold(path: &Path, share: u32) -> fs::File {
        use std::os::windows::fs::OpenOptionsExt;
        let mut options = fs::OpenOptions::new();
        options.read(true).share_mode(share);
        gdl_files::retry_locked(|| options.open(path))
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    #[cfg(windows)]
    #[test]
    fn a_held_executable_is_running() {
        let dir = tempfile::tempdir().unwrap();
        let executable = dir.path().join(EXECUTABLE);
        fs::write(&executable, GAME_BYTES).unwrap();
        assert_closed(&executable);
        // Held as the system holds a running program's file, and as any open holds it: whatever it
        // shares, a handle on the file means that something uses the game.
        for share in [NO_SHARING, SHARING_EVERYTHING] {
            let held = hold(&executable, share);
            assert_running(&executable);
            drop(held);
            assert_closed(&executable);
        }
        // Looking changed nothing.
        assert_eq!(fs::read(&executable).unwrap(), GAME_BYTES);
        assert_eq!(folder_names(dir.path()).unwrap(), [EXECUTABLE]);
    }

    #[test]
    fn a_running_copy_is_running() {
        let dir = tempfile::tempdir().unwrap();
        let copy = copy_of_waiter(dir.path());
        assert_closed(&copy);
        let running = start(&copy, WAITER_ARGS);
        assert_running(&copy);
        // Stopped, the copy is closed again.
        drop(running);
        assert_closed(&copy);
    }

    #[test]
    fn a_missing_executable_is_missing() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("file.txt");
        fs::write(&file, b"not a folder").unwrap();
        let folder = dir.path().join("folder");
        fs::create_dir(&folder).unwrap();
        let linked = dir.path().join("linked");
        link_folder(&linked, &folder).unwrap();
        // Nothing there, at the top or below a missing folder; a name under a file, where nothing
        // can be; and a folder in the executable's place, itself or through a link to it.
        for path in [
            dir.path().join(EXECUTABLE),
            dir.path().join("missing").join(EXECUTABLE),
            file.join(EXECUTABLE),
            folder.clone(),
            linked.clone(),
        ] {
            assert_missing(&path);
        }
        // Nothing was made by looking.
        assert_eq!(
            folder_names(dir.path()).unwrap(),
            ["file.txt", "folder", "linked"]
        );
        assert!(folder_names(&folder).unwrap().is_empty());

        // A link to nothing, where this account may make one.
        let nowhere = dir.path().join("nowhere");
        let dangling = dir.path().join(EXECUTABLE);
        if !symlink_file_where_allowed(&dangling, &nowhere) {
            eprintln!("skipped the link to nothing: this account may not make one");
            return;
        }
        assert_missing(&dangling);
        assert!(fs::symlink_metadata(&nowhere).is_err());
    }

    #[cfg(windows)]
    #[test]
    fn a_read_only_executable_is_not_writable() {
        let dir = tempfile::tempdir().unwrap();
        let executable = dir.path().join(EXECUTABLE);
        fs::write(&executable, GAME_BYTES).unwrap();
        assert_closed(&executable);
        let writable = fs::metadata(&executable).unwrap().permissions();
        let mut read_only = writable.clone();
        read_only.set_readonly(true);
        fs::set_permissions(&executable, read_only).unwrap();
        let found = require_game_closed(&executable);
        // Made writable again before anything is checked, so that the folder can be removed.
        fs::set_permissions(&executable, writable).unwrap();
        match found {
            Err(GameUseError::NotWritable { path }) => assert_eq!(path, executable),
            other => panic!("{other:?}"),
        }
        assert_closed(&executable);
    }

    #[cfg(windows)]
    #[test]
    fn the_closed_game_probe_works_past_max_path() {
        let base = tempfile::tempdir().unwrap();
        let root = gdl_fixtures::long_root(base.path(), gdl_files::MAX_INSTALL_ROOT_UTF16).unwrap();
        let folder = root.join(DEEP_FOLDER);
        fs::create_dir_all(&folder).unwrap();
        let executable = folder.join(EXECUTABLE);
        fs::write(&executable, GAME_BYTES).unwrap();
        let units = gdl_files::root_utf16_len(&executable).unwrap();
        assert!(units > MAX_PATH, "{units}");

        assert_closed(&executable);
        let held = hold(&executable, NO_SHARING);
        assert_running(&executable);
        drop(held);
        assert_closed(&executable);
        gdl_files::retry_locked(|| fs::remove_file(&executable)).unwrap();
        assert_missing(&executable);
        assert_missing(&folder);
    }

    #[test]
    fn run_probe_reports_status_and_first_line() {
        // The shell's echo on Windows, the system's own program elsewhere.
        let answer = if cfg!(windows) {
            shell_answer(&["echo", FIRST_LINE])
        } else {
            answer_of(&system_program("echo"), &[FIRST_LINE.to_owned()])
        };
        assert!(answer.succeeded(), "{answer:?}");
        assert_eq!(answer.status.code(), Some(0));
        assert_eq!(answer.first_line.as_deref(), Some(FIRST_LINE));
        assert_eq!(answer.stderr_tail, "");

        let failed = shell_answer(&["exit", "3"]);
        assert!(!failed.succeeded(), "{failed:?}");
        assert_eq!(failed.status.code(), Some(3));
        assert_eq!(failed.first_line, None);
        assert_eq!(failed.stderr_tail, "");

        let complaint = shell_answer(&["echo", "failed", ">&2"]);
        assert!(complaint.succeeded(), "{complaint:?}");
        assert_eq!(complaint.first_line, None);
        assert_eq!(complaint.stderr_tail.trim_end(), "failed");
    }

    #[test]
    fn run_probe_runs_in_the_programs_folder_with_nothing_to_read() {
        // The shell's own folder, where it is started, as it names it.
        let shown = if cfg!(windows) {
            shell_answer(&["cd"])
        } else {
            shell_answer(&["pwd", "-P"])
        };
        let shown = shown.first_line.unwrap();
        let (program, _) = shell(&[]);
        let folder = program.parent().unwrap();
        if cfg!(windows) {
            assert!(
                shown.eq_ignore_ascii_case(folder.to_str().unwrap()),
                "{shown}"
            );
        } else {
            assert_eq!(Path::new(&shown), fs::canonicalize(folder).unwrap());
        }

        // A program that reads until its input ends finds it ended at once.
        let reader = system_program(if cfg!(windows) { "sort.exe" } else { "cat" });
        let read = answer_of(&reader, &[]);
        assert!(read.succeeded(), "{read:?}");
        assert_eq!(read.first_line, None);

        // On Linux the input is the null device itself, whatever this process reads from.
        #[cfg(target_os = "linux")]
        {
            const NULL_DEVICE: &str = "/dev/null";
            let input = shell_answer(&["readlink", "/proc/self/fd/0"]);
            assert_eq!(input.first_line.as_deref(), Some(NULL_DEVICE), "{input:?}");
            let own = fs::read_link("/proc/self/fd/0");
            if own.is_ok_and(|own| own == Path::new(NULL_DEVICE)) {
                eprintln!(
                    "only half checked: this process reads from the null device too, so an input \
                     it passed on would look the same"
                );
            }
        }
    }

    #[test]
    fn run_probe_bounds_what_it_keeps() {
        let dir = tempfile::tempdir().unwrap();
        // A first line, then more than is kept of either stream, ending in a last line.
        let lines = dir.path().join("lines.txt");
        let mut text = format!("{FIRST_LINE}\n").into_bytes();
        text.resize(BIG_OUTPUT - LAST_LINE.len(), b'x');
        text.extend_from_slice(LAST_LINE);
        assert_eq!(text.len(), BIG_OUTPUT);
        fs::write(&lines, &text).unwrap();
        // A first line longer than is kept.
        let line = dir.path().join("line.txt");
        fs::write(&line, vec![b'y'; BIG_OUTPUT]).unwrap();

        // Each is written whole to both streams in turn, which ends only when both are read as the
        // program writes them.
        for (written, first) in [
            (&lines, FIRST_LINE.to_owned()),
            (&line, "y".repeat(MAX_PROBE_OUTPUT)),
        ] {
            let bytes = fs::read(written).unwrap();
            let (program, args) = file_writer(written);
            let answer = answer_of(&program, &args);
            let shown = written.display();
            assert!(answer.succeeded(), "{shown}: {:?}", answer.status);
            assert!(answer.first_line == Some(first), "{shown}");
            assert_eq!(answer.stderr_tail.len(), MAX_PROBE_OUTPUT, "{shown}");
            assert!(bytes.ends_with(answer.stderr_tail.as_bytes()), "{shown}");
        }
    }

    #[test]
    fn run_probe_times_out_and_kills() {
        let dir = tempfile::tempdir().unwrap();
        let copy = copy_of_waiter(dir.path());
        let begun = Instant::now();
        match probe(&copy, WAITER_ARGS, SHORT_LIMIT) {
            Err(GameStartError::TimedOut) => {}
            other => panic!("{other:?}"),
        }
        let waited = begun.elapsed();
        assert!(waited >= SHORT_LIMIT, "{waited:?}");
        assert!(waited < WAITER_RUN, "{waited:?}");
        // Stopped rather than left to wait its time out: nothing runs the copy any more.
        assert_closed(&copy);
    }

    /// Probes the system's shell starting a copy of [`WAITER`] from `dir` that holds the shell's
    /// output pipes for [`LEFT_RUNNING`], then answering and ending, and stops the copy once the
    /// probe is over. Gives the answer and how long the probe took, or `None` where cmd would read
    /// the copy's path otherwise than it is written.
    #[cfg(windows)]
    fn answer_leaving_a_program_running(dir: &Path) -> Option<(VersionProbe, Duration)> {
        use std::os::windows::process::CommandExt;

        /// What cmd reads as a path, quoted or not, as it is written.
        fn plain(path: &str) -> bool {
            path.chars()
                .all(|c| c.is_ascii_alphanumeric() || " :\\._-~".contains(c))
        }

        // A name of its own, by which it alone is stopped.
        let name = format!("left-running-{}.exe", std::process::id());
        let copy = dir.join(&name);
        let path = copy.to_str().filter(|path| plain(path))?;
        fs::copy(system_program(WAITER), &copy).unwrap();
        let mut words = vec!["start", "", "/b", path];
        words.extend(LEFT_RUNNING_ARGS);
        words.extend([">NUL", "&", "echo", FIRST_LINE]);
        let begun = Instant::now();
        let answer = shell_answer(&words);
        let waited = begun.elapsed();
        let _ = Command::new(system_program("taskkill.exe"))
            .args(["/F", "/IM", &name])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW)
            .status();
        // Its file can go once it no longer runs.
        gdl_files::retry_locked(|| fs::remove_file(&copy))
            .unwrap_or_else(|error| panic!("{name} still runs: {error}"));
        Some((answer, waited))
    }

    /// Probes the system's shell starting [`WAITER`], which holds the shell's output pipes for
    /// [`LEFT_RUNNING`], then answering and ending, and stops it once the probe is over. Gives the
    /// answer and how long the probe took.
    #[cfg(unix)]
    fn answer_leaving_a_program_running(_: &Path) -> Option<(VersionProbe, Duration)> {
        // The shell names what it left running on its standard error, so that it can be stopped.
        let mut words = vec![WAITER];
        words.extend(LEFT_RUNNING_ARGS);
        words.extend(["&", "echo", FIRST_LINE, ";", "echo", "$!", ">&2"]);
        let begun = Instant::now();
        let answer = shell_answer(&words);
        let waited = begun.elapsed();
        let left = answer.stderr_tail.trim();
        if !left.is_empty() && left.bytes().all(|byte| byte.is_ascii_digit()) {
            let _ = Command::new(system_program("sh"))
                .args(["-c", &format!("kill {left}")])
                .status();
        }
        Some((answer, waited))
    }

    #[test]
    fn a_cancel_stops_the_game() {
        let dir = tempfile::tempdir().unwrap();
        let copy = copy_of_waiter(dir.path());
        let cancel = Cancel::default();
        let begun = Instant::now();
        let result = thread::scope(|scope| {
            scope.spawn(|| {
                thread::sleep(CANCEL_AFTER);
                cancel.cancel();
            });
            run_probe(&copy, WAITER_ARGS, GAME_PROBE_LIMIT, &cancel)
        });
        let waited = begun.elapsed();
        match result {
            Err(GameStartError::Cancelled) => {}
            other => panic!("{other:?}"),
        }
        assert!(waited >= CANCEL_AFTER, "{waited:?}");
        assert!(waited < CANCELLED_WITHIN, "{waited:?}");
        // Stopped rather than left to wait its time out: nothing runs the copy any more.
        assert_closed(&copy);
    }

    /// The system's shell and the arguments that make it write a line to a new file at `path`.
    #[cfg(windows)]
    fn marker_writer(path: &Path) -> (PathBuf, Vec<String>) {
        shell(&["echo", "ran", ">", path.to_str().unwrap()])
    }

    /// The system's shell and the arguments that make it write a line to a new file at `path`.
    #[cfg(unix)]
    fn marker_writer(path: &Path) -> (PathBuf, Vec<String>) {
        let args = ["-c", r#"echo ran > "$1""#, "sh", path.to_str().unwrap()];
        (
            system_program("sh"),
            args.iter().map(|&arg| arg.to_owned()).collect(),
        )
    }

    #[test]
    fn a_cancelled_probe_starts_nothing() {
        let dir = tempfile::tempdir().unwrap();
        // The cancel is looked at before the system is asked to start anything: a file it would
        // refuse to start is never offered to it.
        let garbage = dir.path().join(EXECUTABLE);
        write_program(&garbage, GAME_BYTES);
        let cancel = Cancel::default();
        cancel.cancel();
        for result in [
            run_probe(&garbage, &[], GAME_PROBE_LIMIT, &cancel),
            probe_game_version_with(&garbage, &cancel),
        ] {
            match result {
                Err(GameStartError::Cancelled) => {}
                other => panic!("{other:?}"),
            }
        }
        // Nor is a program that would run.
        let marker = dir.path().join("ran.txt");
        let (program, args) = marker_writer(&marker);
        let args: Vec<&str> = args.iter().map(String::as_str).collect();
        match run_probe(&program, &args, GAME_PROBE_LIMIT, &cancel) {
            Err(GameStartError::Cancelled) => {}
            other => panic!("{other:?}"),
        }
        assert!(!marker.exists());
        // Not cancelled, the same probe writes the file.
        let answer = probe(&program, &args, GAME_PROBE_LIMIT).unwrap();
        assert!(answer.succeeded(), "{answer:?}");
        assert!(marker.exists());
    }

    #[test]
    fn a_nul_in_the_program_or_an_argument_starts_nothing() {
        // A program or argument that holds a NUL could only be cut short by the system, and so
        // name another: the command is refused as it starts, before anything runs.
        let dir = tempfile::tempdir().unwrap();
        let marker = dir.path().join("ran.txt");
        let (program, args) = marker_writer(&marker);
        let mut args: Vec<&str> = args.iter().map(String::as_str).collect();
        args.push("a\0b");
        let named = dir.path().join("gaunt\0let");
        for (what, result) in [
            ("an argument", probe(&program, &args, GAME_PROBE_LIMIT)),
            ("the program", probe_game_version(&named)),
        ] {
            match result {
                Err(GameStartError::Start(error)) => {
                    assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{what}: {error}");
                }
                other => panic!("{what}: {other:?}"),
            }
        }
        assert!(!marker.exists());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_busy_file_is_tried_again() {
        use std::io::Write;
        use std::os::unix::fs::PermissionsExt;

        let dir = tempfile::tempdir().unwrap();
        let script = dir.path().join(ECHO_SCRIPT);
        let mut writer = fs::File::create(&script).unwrap();
        writer.write_all(ECHO_SCRIPT_TEXT).unwrap();
        fs::set_permissions(&script, fs::Permissions::from_mode(RUNNABLE)).unwrap();
        // Open to write, the file is refused a start.
        match Command::new(&script)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            Err(error) if is_text_busy(&error) => {}
            Err(error) => panic!("{}: {error}", script.display()),
            Ok(child) => {
                drop(Started(child));
                eprintln!("skipped: this kernel starts a program whose file is open to write");
                return;
            }
        }
        // Closed a moment into the probe, the file is started once it is.
        let begun = Instant::now();
        let answer = thread::scope(|scope| {
            scope.spawn(move || {
                thread::sleep(BUSY_FOR);
                drop(writer);
            });
            probe_game_version(&script)
        });
        let waited = begun.elapsed();
        let answer = answer.unwrap();
        assert!(answer.succeeded(), "{answer:?}");
        assert_eq!(answer.first_line.as_deref(), Some(VERSION_ARGUMENT));
        assert!(waited >= BUSY_FOR, "{waited:?}");
        assert!(waited < RETRY_WINDOW, "{waited:?}");
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_probed_program_inherits_no_descriptor_past_the_standard_three() {
        use std::os::fd::AsRawFd;

        let dir = tempfile::tempdir().unwrap();
        let file = fs::File::create(dir.path().join("held.txt")).unwrap();
        let leaked = leak_descriptor(&file).unwrap();
        let shown = format!("/proc/self/fd/{}", leaked.as_raw_fd());
        let sh = system_program("sh");
        // A program std starts inherits it, as a program started from the launcher's window would
        // inherit what a library opened without close-on-exec.
        let inherited = Command::new(&sh)
            .args(["-c", &format!("test -e {shown}")])
            .stdin(Stdio::null())
            .status()
            .unwrap();
        assert!(inherited.success(), "the descriptor is not inherited");
        // A probed program does not, and keeps its standard streams.
        let check = format!(
            "test -e /proc/self/fd/0 && test -e /proc/self/fd/1 && test -e /proc/self/fd/2 \
             && test ! -e {shown}"
        );
        let answer = probe(&sh, &["-c", &check], GAME_PROBE_LIMIT).unwrap();
        assert!(answer.succeeded(), "{answer:?}");
        // Nor where the kernel refuses close_range and each descriptor is marked in turn.
        let mut command = Command::new(&sh);
        command.args(["-c", &check]).stdin(Stdio::null());
        exec_directly_marking_each(&mut command).unwrap();
        let marked = command.status().unwrap();
        assert!(marked.success(), "{marked:?}");
        drop(leaked);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_file_that_holds_no_program_is_never_run_by_a_shell() {
        let dir = tempfile::tempdir().unwrap();
        let marker = dir.path().join("ran.txt");
        let executable = dir.path().join(EXECUTABLE);
        // A shell's command, with neither a program's header nor a line naming an interpreter.
        let command = format!("touch '{}'\n", marker.to_str().unwrap());
        write_program(&executable, command.as_bytes());
        match probe_game_version(&executable) {
            Err(GameStartError::Start(error)) => {
                assert_eq!(error.raw_os_error(), Some(libc::ENOEXEC), "{error}");
            }
            other => panic!("{other:?}"),
        }
        assert!(!marker.exists());
        // A shell given the file runs it, and makes the marker.
        let status = Command::new(system_program("sh"))
            .arg(&executable)
            .stdin(Stdio::null())
            .status()
            .unwrap();
        assert!(status.success(), "{status:?}");
        assert!(marker.exists());
    }

    /// The arguments that make this test program run the test `name` alone.
    #[cfg(windows)]
    fn only_test(name: &str) -> [&str; 4] {
        ["--exact", name, "--test-threads", "1"]
    }

    #[cfg(windows)]
    #[test]
    fn critical_errors_reach_the_process() {
        use std::os::windows::process::CommandExt;
        use windows_sys::Win32::System::Threading::{CREATE_DEFAULT_ERROR_MODE, CREATE_NO_WINDOW};

        // A program inherits its error mode from what starts it, and the shells and runners the
        // tests run under fail critical errors already, so the probe is made in a child of this
        // program started in the system's default mode, which shows them, as a launcher started
        // from Explorer is.
        let output = Command::new(std::env::current_exe().unwrap())
            .args(only_test(PROBING_TEST))
            .env(ERROR_MODE_CHILD, "1")
            .creation_flags(CREATE_DEFAULT_ERROR_MODE | CREATE_NO_WINDOW)
            .stdin(Stdio::null())
            .output()
            .unwrap();
        assert_eq!(
            output.status.code(),
            Some(ERROR_MODE_CHECKED),
            "{}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }

    /// The child's part of `critical_errors_reach_the_process`.
    #[cfg(windows)]
    #[test]
    fn probing_from_the_default_error_mode() {
        use std::os::windows::process::CommandExt;
        use windows_sys::Win32::System::Threading::CREATE_NO_WINDOW;

        if std::env::var_os(ERROR_MODE_CHILD).is_none() {
            return;
        }
        refuse_fault_dialogs();
        let started = error_mode();
        assert_eq!(started & SEM_FAILCRITICALERRORS, 0, "{started:#x}");
        // A program this one starts as std starts one inherits that mode.
        let this = std::env::current_exe().unwrap();
        let plain = Command::new(&this)
            .args(only_test(REPORTING_TEST))
            .creation_flags(CREATE_NO_WINDOW)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .unwrap();
        assert_eq!(plain.code(), Some(SHOWS_CRITICAL_ERRORS), "{plain:?}");
        // A probed program starts with critical errors failing, and the probe leaves the rest of
        // the process's mode as it was.
        let answer = probe(&this, &only_test(REPORTING_TEST), GAME_PROBE_LIMIT).unwrap();
        assert_eq!(
            answer.status.code(),
            Some(FAILS_CRITICAL_ERRORS),
            "{answer:?}"
        );
        assert_eq!(error_mode(), started | SEM_FAILCRITICALERRORS);
        std::process::exit(ERROR_MODE_CHECKED);
    }

    /// The probed program's part of `critical_errors_reach_the_process`, which tells by its exit
    /// code the error mode it started in.
    #[cfg(windows)]
    #[test]
    fn reporting_the_error_mode() {
        if std::env::var_os(ERROR_MODE_CHILD).is_none() {
            return;
        }
        let fails = error_mode() & SEM_FAILCRITICALERRORS != 0;
        std::process::exit(if fails {
            FAILS_CRITICAL_ERRORS
        } else {
            SHOWS_CRITICAL_ERRORS
        });
    }

    #[test]
    fn a_binary_that_cannot_load_fails_quickly() {
        let dir = tempfile::tempdir().unwrap();
        let executable = dir.path().join(EXECUTABLE);
        write_program(&executable, GAME_BYTES);
        let begun = Instant::now();
        match probe_game_version(&executable) {
            Err(GameStartError::Start(_)) => {}
            other => panic!("{other:?}"),
        }
        let waited = begun.elapsed();
        assert!(waited < FAILS_WITHIN, "{waited:?}");
    }

    #[test]
    fn a_program_left_running_does_not_keep_the_probe_waiting() {
        let dir = tempfile::tempdir().unwrap();
        let Some((answer, waited)) = answer_leaving_a_program_running(dir.path()) else {
            eprintln!(
                "skipped: cmd would read the test folder's path otherwise than it is written"
            );
            return;
        };
        assert!(answer.succeeded(), "{answer:?}");
        assert_eq!(answer.first_line.as_deref(), Some(FIRST_LINE));
        // What the shell wrote was read for a moment after it ended, and then no longer.
        assert!(waited >= DRAIN_LIMIT, "{waited:?}");
        assert!(waited < LEFT_RUNNING / 2, "{waited:?}");
    }

    #[test]
    fn a_garbage_program_does_not_succeed() {
        let dir = tempfile::tempdir().unwrap();
        let garbage = dir.path().join(EXECUTABLE);
        write_program(&garbage, GAME_BYTES);
        // A file that holds no program, probed and asked for its version, and a folder: the
        // system refuses to start each, and nothing else is asked to run it.
        for (what, result) in [
            (
                "probed",
                probe(&garbage, &[VERSION_ARGUMENT], GAME_PROBE_LIMIT),
            ),
            ("asked for its version", probe_game_version(&garbage)),
            ("a folder", probe(dir.path(), &[], GAME_PROBE_LIMIT)),
        ] {
            match result {
                Err(GameStartError::Start(_)) => {}
                other => panic!("{what}: {other:?}"),
            }
        }
        // A root, which has no folder to run in.
        let root = dir.path().ancestors().last().unwrap();
        match probe(root, &[], GAME_PROBE_LIMIT) {
            Err(GameStartError::Start(error)) => {
                assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn relative_paths_are_refused() {
        // Among them names that a search of PATH would find, which is never made.
        let mut relative = vec![
            "",
            "gauntlet",
            EXECUTABLE,
            "./gauntlet",
            "../gauntlet",
            "game/gauntlet",
        ];
        if cfg!(windows) {
            relative.extend([r"\gauntlet.exe", "C:gauntlet.exe", "cmd.exe", "cmd"]);
        } else {
            relative.extend(["sh", "echo"]);
        }
        for path in relative.into_iter().map(Path::new) {
            for result in [
                run_probe(path, &[], GAME_PROBE_LIMIT, &Cancel::default()),
                probe_game_version(path),
                probe_game_version_with(path, &Cancel::default()),
            ] {
                match result {
                    Err(GameStartError::NotAbsolute) => {}
                    other => panic!("{}: {other:?}", path.display()),
                }
            }
        }
    }

    #[test]
    fn the_version_probe_asks_for_the_version() {
        // A program that writes back its arguments answers with what it was asked.
        let dir = tempfile::tempdir().unwrap();
        let script = dir.path().join(ECHO_SCRIPT);
        write_program(&script, ECHO_SCRIPT_TEXT);
        for answer in [
            probe_game_version(&script),
            probe_game_version_with(&script, &Cancel::default()),
        ] {
            let answer = answer.unwrap();
            assert!(answer.succeeded(), "{answer:?}");
            assert_eq!(answer.first_line.as_deref(), Some(VERSION_ARGUMENT));
        }
    }

    #[test]
    fn first_lines_lose_only_their_line_ending() {
        let cases: [(&[u8], Option<&str>); 12] = [
            (b"", None),
            (b"\n", Some("")),
            (b"\r\n", Some("")),
            (b"1.0.0-beta", Some("1.0.0-beta")),
            (b"1.0.0-beta\n", Some("1.0.0-beta")),
            (b"1.0.0-beta\r\n", Some("1.0.0-beta")),
            (b"1.0.0-beta\r\nmore\r\n", Some("1.0.0-beta")),
            (b"1.0.0-beta\r", Some("1.0.0-beta")),
            (b" 1.0.0-beta \t\n", Some(" 1.0.0-beta \t")),
            (b"a\rb\n", Some("a\rb")),
            (b"\n1.0.0-beta\n", Some("")),
            (b"\xff1.0\n", Some("\u{fffd}1.0")),
        ];
        for (output, line) in cases {
            assert_eq!(first_line(output).as_deref(), line, "{output:?}");
        }
    }

    #[test]
    fn output_is_kept_to_its_bounds() {
        const LIMIT: usize = 4;
        let reads: [(&[u8], &[u8], &[u8]); 7] = [
            (b"", b"", b""),
            (b"ab", b"ab", b"ab"),
            (b"cd", b"abcd", b"abcd"),
            (b"e", b"abcd", b"bcde"),
            (b"", b"abcd", b"bcde"),
            (b"fghijk", b"abcd", b"hijk"),
            (b"l", b"abcd", b"ijkl"),
        ];
        let mut head = Vec::new();
        let mut tail = Vec::new();
        for (read, kept_head, kept_tail) in reads {
            keep_head(&mut head, read, LIMIT);
            keep_tail(&mut tail, read, LIMIT);
            assert_eq!(head, kept_head, "{read:?}");
            assert_eq!(tail, kept_tail, "{read:?}");
        }
        // A first read longer than the bound keeps its start, and its end.
        let mut head = Vec::new();
        let mut tail = Vec::new();
        keep_head(&mut head, b"abcdef", LIMIT);
        keep_tail(&mut tail, b"abcdef", LIMIT);
        assert_eq!(head, b"abcd");
        assert_eq!(tail, b"cdef");
    }

    #[test]
    fn probe_errors_say_what_failed() {
        let path = Path::new("install").join(EXECUTABLE);
        let shown = path.display();
        let denied = || io::Error::new(io::ErrorKind::PermissionDenied, "denied");

        for (error, text) in [
            (
                GameUseError::Running,
                "Close the game before changing its installation".to_owned(),
            ),
            (
                GameUseError::Missing,
                "The installed executable is missing".to_owned(),
            ),
            (
                GameUseError::NotWritable { path: path.clone() },
                format!("The installed executable is not writable: {shown}"),
            ),
            (
                GameUseError::Io {
                    path: path.clone(),
                    source: denied(),
                },
                format!("{shown}: denied"),
            ),
        ] {
            assert_eq!(error.to_string(), text);
            match &error {
                GameUseError::Io { .. } => {
                    assert_eq!(error.source().unwrap().to_string(), "denied");
                }
                _ => assert!(error.source().is_none(), "{text}"),
            }
        }

        for (error, text, rule) in [
            (
                GameStartError::NotAbsolute,
                "The game's executable path is not absolute",
                "notAbsolute",
            ),
            (
                GameStartError::Start(denied()),
                "The game did not start: denied",
                "start",
            ),
            (
                GameStartError::TimedOut,
                "The game did not answer in time and was stopped",
                "timedOut",
            ),
            (
                GameStartError::Cancelled,
                "The game's version probe was cancelled",
                "cancelled",
            ),
            (
                GameStartError::Io(denied()),
                "The game's run could not be followed: denied",
                "io",
            ),
        ] {
            assert_eq!(error.to_string(), text);
            assert_eq!(error.rule(), rule, "{text}");
            match &error {
                GameStartError::Start(_) | GameStartError::Io(_) => {
                    assert_eq!(error.source().unwrap().to_string(), "denied");
                }
                _ => assert!(error.source().is_none(), "{text}"),
            }
        }

        assert!(size_of::<GameUseError>() <= MOST_ERROR_BYTES);
        assert!(size_of::<GameStartError>() <= MOST_ERROR_BYTES);
    }
}
