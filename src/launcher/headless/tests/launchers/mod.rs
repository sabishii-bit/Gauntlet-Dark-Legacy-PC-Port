//! What the test programs that run launcher copies of the headless program share: a launcher file
//! of the program's own carrying a runtime, an installation of an older release whose launcher copy
//! is such a file, and a run of a launcher copy whose output, and that of any copy it hands over
//! to, which shares its streams, is read as it comes against a deadline, past which both are ended.

use std::ffi::OsStr;
use std::fs;
use std::io::{self, BufRead, BufReader, Read};
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::{Mutex, MutexGuard, PoisonError};
use std::thread;
use std::time::{Duration, Instant};

use gdl_files::{RETRY_INTERVAL, RETRY_WINDOW};
use gdl_fixtures::{FixtureParts, InstallFixture, RuntimeFiles, append_sections, tpl};
use gdl_install::{RUNTIME_SECTION, installed_launcher_name};
use gdl_release::Platform;

#[cfg(windows)]
use crate::common::CREATE_NO_WINDOW;
use crate::common::PROGRAM;

/// The release of the installations the tests update and recover: older than the program.
pub const OLDER: &str = "0.9.0";

/// What starts the line a launcher writes once it has started the copy it hands over to, which
/// the line ends with the id of.
const HANDED_OVER: &str = "handedOver: ";

/// What starts the line a new copy writes once the launcher it waited for exited, before the
/// milliseconds it waited.
const WAITED_START: &str = "previous: exited after ";

/// What ends that line, after the milliseconds.
const WAITED_END: &str = " ms";

/// What starts the lines a job's progress is written on.
const PROGRESS_LINES: [&str; 2] = ["phase: ", "progress: "];

/// How long a run, and any copy it hands over to, is given to write all it writes and end, past
/// which both are ended and the test fails: long enough for a scanner's first look at each new
/// copy of the program.
const RUN_DEADLINE: Duration = Duration::from_secs(60);

/// The encoding of the disc icon the installations carry, RGB5A3: the icon tests' TPL, which
/// decodes, so that an update that stages the executable on Windows sets the icon into it.
const ICON_ENCODING: u32 = 5;

/// The side of the icon's one square image.
const ICON_SIDE: u16 = 8;

/// The installation's folder in the folder a test lays it out in.
const INSTALLATION: &str = "install";

/// The variable that names Windows' own folder.
#[cfg(windows)]
const SYSTEM_ROOT: &str = "SystemRoot";

/// Where Windows' own programs lie in its folder.
#[cfg(windows)]
const SYSTEM_PROGRAMS: &str = "System32";

/// The system's program that ends a process by its id and the name of the file it runs.
#[cfg(windows)]
const TASKKILL: &str = "taskkill.exe";

/// Where Linux lists its processes, a folder for each, named by its id.
#[cfg(unix)]
const PROCESSES: &str = "/proc";

/// The link in a process's folder to the file the process runs.
#[cfg(unix)]
const PROCESS_EXECUTABLE: &str = "exe";

/// What Linux puts after the name of the file a process runs once that file was removed.
#[cfg(unix)]
const DELETED_SUFFIX: &str = " (deleted)";

/// The shell, whose `kill` ends a process by its id.
#[cfg(unix)]
const SHELL: &str = "/bin/sh";

/// The mode an install gives the programs it places on Linux, the launcher copy among them: one
/// that lets them run.
#[cfg(unix)]
const PROGRAM_MODE: u32 = 0o755;

/// Held while a test runs launcher copies that update, recover or hand over, so that no two such
/// runs of two tests run beside one another.
static ONE_RUN: Mutex<()> = Mutex::new(());

/// Waits until no other test runs launcher copies, whatever the last that ran them did, and holds
/// the others off until what it gives is dropped.
pub fn one_run_at_a_time() -> MutexGuard<'static, ()> {
    ONE_RUN.lock().unwrap_or_else(PoisonError::into_inner)
}

/// The platform this program was built for, whose launcher copies the tests run; `None`, the
/// reason printed, on a system no release is built for.
pub fn this_platform() -> Option<Platform> {
    let platform = Platform::of_this_build();
    if platform.is_none() {
        eprintln!("skipped: no release is built for this system");
    }
    platform
}

/// A launcher file for `platform` of the program's own carrying the standard runtime of
/// `version`, whose executable is the program too, as a release appends the runtime to the
/// launcher's program; and that runtime. The executable is stored in the runtime's archive rather
/// than deflated, so that its megabytes build fast; the release record names its bytes.
pub fn launcher_file(platform: Platform, version: &str) -> (RuntimeFiles, Vec<u8>) {
    let program = fs::read(PROGRAM).unwrap();
    let mut runtime = RuntimeFiles::standard(platform.as_str(), version);
    let zip = runtime.zip_with_program(&program);
    let mut launcher = program;
    append_sections(&mut launcher, &[(RUNTIME_SECTION, &zip)], 0);
    (runtime, launcher)
}

/// An installation of [`OLDER`]'s standard runtime for `platform`, in a folder of its own in
/// `dir`, whose launcher copy holds `launcher` and runs, and whose disc icon decodes.
pub fn installation(dir: &Path, platform: Platform, launcher: &[u8]) -> InstallFixture {
    let icon = tpl(ICON_ENCODING, ICON_SIDE, ICON_SIDE);
    let parts = FixtureParts {
        icon: Some(&icon),
        launcher: Some(launcher),
    };
    let runtime = RuntimeFiles::standard(platform.as_str(), OLDER);
    let fixture = InstallFixture::write_with(&dir.join(INSTALLATION), runtime, parts).unwrap();
    let_run(&fixture.root.join(installed_launcher_name(platform)));
    fixture
}

/// Writes `bytes`, a launcher file, to the file at `path`, making the folders above it, and lets
/// it run as an install lets the launcher copy run.
pub fn write_launcher(path: &Path, bytes: &[u8]) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, bytes).unwrap();
    let_run(path);
}

/// Lets the program at `path` run, as an install lets the programs it places run: on Linux by its
/// mode.
#[cfg(unix)]
fn let_run(path: &Path) {
    use std::os::unix::fs::PermissionsExt;

    fs::set_permissions(path, fs::Permissions::from_mode(PROGRAM_MODE)).unwrap();
}

/// Lets the program at `path` run, as an install lets the programs it places run: Windows runs any
/// program's file as it is.
#[cfg(not(unix))]
fn let_run(_: &Path) {}

/// The lines of a run's output but those that tell its job's progress: its phases and their
/// shares done.
pub fn past_progress(out: &str) -> Vec<&str> {
    out.lines()
        .filter(|line| !PROGRESS_LINES.iter().any(|start| line.starts_with(start)))
        .collect()
}

/// How long a new copy waited for the launcher it replaced, in milliseconds, as its line
/// `previous: exited after <ms> ms` gives it; `None` for any other line.
pub fn waited(line: &str) -> Option<u64> {
    line.strip_prefix(WAITED_START)?
        .strip_suffix(WAITED_END)?
        .parse()
        .ok()
}

/// The stream a run wrote a line on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stream {
    /// Its output.
    Out,
    /// Its error stream.
    Err,
}

/// What a run wrote next.
#[derive(Debug, PartialEq, Eq)]
pub enum Next {
    /// A line, on one of its streams.
    Line(Stream, String),
    /// Nothing more: every process that held its streams closed them.
    Ended,
    /// Nothing within the time it was given.
    Quiet,
}

/// What a reading thread heard on the stream it reads.
enum Heard {
    /// A line.
    Line(Stream, String),
    /// The stream's end.
    Closed,
}

/// What a run wrote, whole, and how the launcher it started ended.
#[derive(Debug)]
pub struct Finished {
    /// How the launcher ended.
    pub status: ExitStatus,
    /// What it, and any copy it handed over to, wrote on the output, each line ended.
    pub out: String,
    /// What they wrote on the error stream, each line ended.
    pub err: String,
    /// The id of the copy it handed over to, when it handed over.
    pub handed_over: Option<u32>,
}

/// A launcher copy started by a test, without a console window on Windows and with nothing to
/// read, whose output and error streams are read as they come. A copy it hands over to inherits
/// both streams, so they end once both copies closed them. Dropped before that, as a test that
/// failed or passed its deadline drops it, both copies are ended, so that none outlives the test.
pub struct Launched {
    /// The launcher the test started.
    child: Child,
    /// What the threads reading its streams heard.
    heard: Receiver<Heard>,
    /// How many of its streams are still open.
    open: usize,
    /// When the run is to have ended.
    deadline: Instant,
    /// The lines of its output so far.
    out: Vec<String>,
    /// The lines of its error stream so far.
    err: Vec<String>,
    /// Every line so far, on either stream in turn, for a failure's message.
    seen: Vec<String>,
    /// The id of the copy it handed over to, once its line came.
    handed_over: Option<u32>,
    /// Whether both streams ended and the launcher exited.
    finished: bool,
}

impl Launched {
    /// Starts `program` with `args`; tried again while Linux refuses a program just written as
    /// busy, as a child another test starts may hold it for a moment, every [`RETRY_INTERVAL`] for
    /// up to [`RETRY_WINDOW`].
    pub fn start<A: AsRef<OsStr>>(program: &Path, args: &[A]) -> Launched {
        let mut command = Command::new(program);
        command
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(CREATE_NO_WINDOW);
        }
        let begun = Instant::now();
        let mut child = loop {
            match command.spawn() {
                Ok(child) => break child,
                Err(error)
                    if error.kind() == io::ErrorKind::ExecutableFileBusy
                        && begun.elapsed() < RETRY_WINDOW =>
                {
                    thread::sleep(RETRY_INTERVAL);
                }
                Err(error) => panic!("{}: {error}", program.display()),
            }
        };
        let (sender, heard) = mpsc::channel();
        let out = child.stdout.take().expect("the output is piped");
        let err = child.stderr.take().expect("the error stream is piped");
        read_lines(out, Stream::Out, sender.clone());
        read_lines(err, Stream::Err, sender);
        Launched {
            child,
            heard,
            open: 2,
            deadline: Instant::now() + RUN_DEADLINE,
            out: Vec::new(),
            err: Vec::new(),
            seen: Vec::new(),
            handed_over: None,
            finished: false,
        }
    }

    /// The id of the launcher the test started.
    pub fn pid(&self) -> u32 {
        self.child.id()
    }

    /// What the run writes next, waited for until `until`; [`Next::Quiet`] when nothing comes by
    /// then. Panics when the run's deadline passes first.
    pub fn next_until(&mut self, until: Instant) -> Next {
        while self.open > 0 {
            let bound = until.min(self.deadline);
            match self
                .heard
                .recv_timeout(bound.saturating_duration_since(Instant::now()))
            {
                Ok(Heard::Line(stream, line)) => {
                    self.note(stream, &line);
                    return Next::Line(stream, line);
                }
                Ok(Heard::Closed) => self.open -= 1,
                Err(RecvTimeoutError::Timeout) if until < self.deadline => return Next::Quiet,
                Err(RecvTimeoutError::Timeout) => {
                    panic!("the run did not end within its deadline: {:#?}", self.seen)
                }
                // Each reader tells its stream's end before it lets go of its sender.
                Err(RecvTimeoutError::Disconnected) => self.open = 0,
            }
        }
        Next::Ended
    }

    /// Reads the run to its end, within its deadline: every line both copies write, until both
    /// streams end, then how the launcher the test started exited.
    pub fn finish(&mut self) -> Finished {
        while self.next_until(self.deadline) != Next::Ended {}
        let status = loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                break status;
            }
            assert!(
                Instant::now() < self.deadline,
                "the launcher did not exit within its deadline: {:#?}",
                self.seen
            );
            thread::sleep(RETRY_INTERVAL);
        };
        self.finished = true;
        let ended = |lines: &[String]| lines.iter().map(|line| format!("{line}\n")).collect();
        Finished {
            status,
            out: ended(&self.out),
            err: ended(&self.err),
            handed_over: self.handed_over,
        }
    }

    /// Keeps `line`, which came on `stream`, and the id of the copy handed over to that it names.
    fn note(&mut self, stream: Stream, line: &str) {
        self.seen.push(format!("{stream:?}: {line}"));
        match stream {
            Stream::Out => {
                if let Some(pid) = line.strip_prefix(HANDED_OVER) {
                    self.handed_over = pid.parse().ok();
                }
                self.out.push(line.to_owned());
            }
            Stream::Err => self.err.push(line.to_owned()),
        }
    }
}

impl Drop for Launched {
    fn drop(&mut self) {
        if self.finished {
            return;
        }
        if matches!(self.child.try_wait(), Ok(None)) {
            let _ = self.child.kill();
        }
        let _ = self.child.wait();
        // A line that came but was not yet read may name the copy handed over to.
        while let Ok(heard) = self.heard.try_recv() {
            if let Heard::Line(stream, line) = heard {
                self.note(stream, &line);
            }
        }
        if let Some(pid) = self.handed_over {
            end_handed_over(pid);
        }
    }
}

/// Reads `stream`, the run's `which`, on a thread of its own, sending each line to `heard` as it
/// comes and then the stream's end.
fn read_lines(stream: impl Read + Send + 'static, which: Stream, heard: Sender<Heard>) {
    thread::spawn(move || {
        for line in BufReader::new(stream).lines() {
            let Ok(line) = line else { break };
            if heard.send(Heard::Line(which, line)).is_err() {
                return;
            }
        }
        let _ = heard.send(Heard::Closed);
    });
}

/// Ends the process `pid` a run handed over to, which the test holds no handle to, while it still
/// runs a file of the installed launcher copy's name, so that a process that took its id since is
/// left alone: through the system's taskkill, by both its id and that name.
#[cfg(windows)]
fn end_handed_over(pid: u32) {
    use std::os::windows::process::CommandExt;

    let Some(system) = std::env::var_os(SYSTEM_ROOT) else {
        return;
    };
    let image = installed_launcher_name(Platform::WindowsX64);
    let _ = Command::new(Path::new(&system).join(SYSTEM_PROGRAMS).join(TASKKILL))
        .args(["/F", "/T", "/FI"])
        .arg(format!("PID eq {pid}"))
        .arg("/FI")
        .arg(format!("IMAGENAME eq {image}"))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .creation_flags(CREATE_NO_WINDOW)
        .status();
}

/// Ends the process `pid` a run handed over to, which the test holds no handle to, while it still
/// runs a file of the installed launcher copy's name, as `/proc` names it, so that a process that
/// took its id since is left alone: through the shell's kill.
#[cfg(unix)]
fn end_handed_over(pid: u32) {
    let Some(platform) = Platform::of_this_build() else {
        return;
    };
    let link = Path::new(PROCESSES)
        .join(pid.to_string())
        .join(PROCESS_EXECUTABLE);
    let image = fs::read_link(link).ok();
    let name = image
        .as_deref()
        .and_then(Path::file_name)
        .and_then(OsStr::to_str);
    let runs_the_copy = name.is_some_and(|name| {
        name.strip_suffix(DELETED_SUFFIX).unwrap_or(name) == installed_launcher_name(platform)
    });
    if runs_the_copy {
        let _ = Command::new(SHELL)
            .arg("-c")
            .arg(format!("kill -9 {pid}"))
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}
