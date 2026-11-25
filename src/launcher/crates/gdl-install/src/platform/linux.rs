//! The Linux calls: starting a program with execv from a hook that runs in the child before it
//! starts, once close_range, or fcntl one descriptor at a time, has marked every descriptor past
//! the standard streams close-on-exec, so that the program inherits none of them; and another
//! process, opened by its id as a descriptor with pidfd_open, to be waited for with poll and ended
//! with pidfd_send_signal, so that a process that took the id of one that ended is never waited
//! for or signalled in its place.

use std::ffi::{CString, c_char, c_int, c_uint};
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::process::CommandExt;
use std::process::Command;
use std::time::{Duration, Instant};

use crate::probe::MAX_MARKED_DESCRIPTORS;

/// The first descriptor past standard input, output and error.
const FIRST_INHERITED: c_int = 3;

/// The lowest descriptor [`leak_descriptor`] gives, well past any the test process has open.
#[cfg(test)]
const LEAKED_FROM: c_int = 10;

/// How a hook marks the descriptors past the standard streams.
#[derive(Clone, Copy)]
enum Marking {
    /// With close_range, which kernels from 5.11 on take, and one at a time where it is refused.
    Range,
    /// One at a time alone, as on a kernel that refuses close_range.
    #[cfg(test)]
    EachDescriptor,
}

/// Makes `command` start its program with no descriptor past its standard streams, whatever this
/// process holds open without close-on-exec, and as the system loads a program: by execv, never
/// handed to a shell. A hook that runs in the child after its streams and folder are set marks
/// every descriptor from 3 up close-on-exec and then starts the program itself, so the system
/// closes them as the program starts. Marking rather than closing keeps the descriptor through
/// which a child that fails to start tells its parent why. The program is started with execv
/// because a command with a hook is otherwise started with execvp, which hands a file that holds
/// no program to `/bin/sh` to run as a script; with execv such a file is refused.
///
/// The program and its arguments are taken as they stand: call this once they are set. The
/// program runs with this process's environment and names itself by its path: a command whose
/// environment was changed through `env`, `envs` or `env_remove` is refused, while `env_clear`
/// and `CommandExt::arg0`, which a command does not show, are not honoured, so a caller must not
/// use them. A program or argument that holds a NUL never reaches the hook: std refuses such a
/// command as it starts it.
pub(crate) fn exec_directly(command: &mut Command) -> io::Result<()> {
    install_hook(command, Marking::Range)
}

/// What [`exec_directly`] does, with every descriptor marked one at a time, as on a kernel that
/// refuses close_range.
#[cfg(test)]
pub(crate) fn exec_directly_marking_each(command: &mut Command) -> io::Result<()> {
    install_hook(command, Marking::EachDescriptor)
}

/// Sets the hook that marks the descriptors as `marking` says and starts `command`'s program.
fn install_hook(command: &mut Command, marking: Marking) -> io::Result<()> {
    if command.get_envs().next().is_some() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "a program started without inherited descriptors takes this process's environment",
        ));
    }
    let program = Program::of(command)?;
    let limit = descriptor_limit();
    // SAFETY: the hook runs in the child between fork and exec, where only calls safe in a
    // signal handler may be made: it calls close_range, fcntl and execv, which are, and reads
    // errno; it allocates nothing, takes no lock and touches only what it owns, the program's
    // strings made before the fork.
    unsafe {
        command.pre_exec(move || {
            mark_inherited(limit, marking)?;
            Err(program.exec())
        });
    }
    Ok(())
}

/// A program and its arguments as execv takes them: NUL-terminated strings, and a list of
/// pointers to them that a null ends.
struct Program {
    /// The program's path, which is also its first argument.
    path: CString,
    /// The arguments after the first, which `argv` points into.
    _args: Vec<CString>,
    /// Pointers to the path and to each argument, then a null.
    argv: Vec<*const c_char>,
}

// SAFETY: `argv` points only into strings this value owns, whose bytes stay where they are while
// it lives, since moving a CString moves only its pointer; nothing writes through the pointers.
unsafe impl Send for Program {}
// SAFETY: the same; the value is only read.
unsafe impl Sync for Program {}

impl Program {
    /// `command`'s program and arguments, refused when one holds a NUL, which std's command,
    /// keeping a stand-in for such a string, never gives.
    fn of(command: &Command) -> io::Result<Program> {
        let c_string = |text: &std::ffi::OsStr| {
            CString::new(text.as_bytes()).map_err(|_| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "a program or argument given to Linux holds a NUL",
                )
            })
        };
        let path = c_string(command.get_program())?;
        let args = command
            .get_args()
            .map(c_string)
            .collect::<io::Result<Vec<_>>>()?;
        let mut argv = Vec::with_capacity(args.len() + 2);
        argv.push(path.as_ptr());
        argv.extend(args.iter().map(|arg| arg.as_ptr()));
        argv.push(std::ptr::null());
        Ok(Program {
            path,
            _args: args,
            argv,
        })
    }

    /// Replaces the calling process with the program; returns only when the system refused, with
    /// why.
    fn exec(&self) -> io::Error {
        // SAFETY: the path and every argument are NUL-terminated strings this value owns, and
        // `argv` points at each of them in turn, then at a null, as execv requires; execv only
        // reads them, and returns only when it fails.
        unsafe { libc::execv(self.path.as_ptr(), self.argv.as_ptr()) };
        io::Error::last_os_error()
    }
}

/// The highest descriptor number a hook marks one at a time, plus one: this process's soft limit
/// on open files, read before the fork, at most [`MAX_MARKED_DESCRIPTORS`].
fn descriptor_limit() -> c_int {
    let mut limit = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    // SAFETY: the call writes the limit into `limit`, a local that outlives it.
    let read = unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut limit) };
    let soft = if read == 0 {
        limit.rlim_cur.min(MAX_MARKED_DESCRIPTORS)
    } else {
        MAX_MARKED_DESCRIPTORS
    };
    c_int::try_from(soft).unwrap_or(c_int::MAX)
}

/// Marks every open descriptor from [`FIRST_INHERITED`] up close-on-exec, as `marking` says: with
/// close_range when the kernel takes it, else each below `limit` with fcntl, passing over those
/// that are not open. Runs in the child before its program starts.
fn mark_inherited(limit: c_int, marking: Marking) -> io::Result<()> {
    if matches!(marking, Marking::Range) {
        // SAFETY: close_range with CLOSE_RANGE_CLOEXEC only sets the close-on-exec flag of the
        // descriptors in the range, and takes only numbers; a kernel that does not know it, or
        // its flag, refuses it and changes nothing.
        let marked = unsafe {
            libc::syscall(
                libc::SYS_close_range,
                FIRST_INHERITED.cast_unsigned(),
                c_uint::MAX,
                libc::CLOSE_RANGE_CLOEXEC,
            )
        };
        if marked == 0 {
            return Ok(());
        }
    }
    for descriptor in FIRST_INHERITED..limit {
        // SAFETY: F_SETFD only sets the descriptor's flags, of which FD_CLOEXEC is the one there
        // is, and fails with EBADF when the descriptor is not open.
        let set = unsafe { libc::fcntl(descriptor, libc::F_SETFD, libc::FD_CLOEXEC) };
        if set == -1 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() != Some(libc::EBADF) {
                return Err(error);
            }
        }
    }
    Ok(())
}

/// What opening a process by its id as a descriptor found.
pub(crate) enum OpenedPid {
    /// The process's descriptor, which stays the process's whatever later takes its id.
    Open(OwnedFd),
    /// No process has the id: it ended, and was reaped.
    Gone,
    /// The kernel has no pidfd_open, as kernels before 5.3 do not.
    Unsupported,
}

/// Opens the process whose id is `pid` as a descriptor, as pidfd_open opens one: ESRCH, no such
/// process, is [`OpenedPid::Gone`], and ENOSYS [`OpenedPid::Unsupported`]; any other refusal is the
/// kernel's own error.
pub(crate) fn pidfd_open(pid: u32) -> io::Result<OpenedPid> {
    let pid = libc::pid_t::try_from(pid).map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "a process id is past what Linux numbers processes with",
        )
    })?;
    // SAFETY: the call takes only numbers, and a descriptor it gives is this process's own, which
    // no other value owns.
    let opened = unsafe { libc::syscall(libc::SYS_pidfd_open, pid, 0) };
    if opened == -1 {
        let error = io::Error::last_os_error();
        return match error.raw_os_error() {
            Some(libc::ESRCH) => Ok(OpenedPid::Gone),
            Some(libc::ENOSYS) => Ok(OpenedPid::Unsupported),
            _ => Err(error),
        };
    }
    let descriptor = c_int::try_from(opened).map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "pidfd_open gave a descriptor past any descriptor's number",
        )
    })?;
    // SAFETY: the descriptor was just opened, and is owned by nothing else, so the value made from
    // it alone closes it.
    Ok(OpenedPid::Open(unsafe { OwnedFd::from_raw_fd(descriptor) }))
}

/// Waits up to `limit` for the process `pidfd` opened to end, as poll tells of one: `true` when it
/// has ended, `false` when it still runs at the limit. A wait a signal cuts short goes on for what
/// is left of the limit.
pub(crate) fn pidfd_wait(pidfd: &OwnedFd, limit: Duration) -> io::Result<bool> {
    let deadline = Instant::now() + limit;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        // Rounded up, so that a wait never ends before its limit.
        let millis = c_int::try_from(left.as_micros().div_ceil(1000)).unwrap_or(c_int::MAX);
        let mut watched = libc::pollfd {
            fd: pidfd.as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        };
        // SAFETY: the call reads and writes one pollfd, a local that outlives it, which names a
        // descriptor open through the call.
        let ready = unsafe { libc::poll(&mut watched, 1, millis) };
        if ready > 0 {
            return Ok(true);
        }
        if ready == 0 {
            if Instant::now() >= deadline {
                return Ok(false);
            }
            continue;
        }
        let error = io::Error::last_os_error();
        if error.raw_os_error() != Some(libc::EINTR) {
            return Err(error);
        }
    }
}

/// Ends the process `pidfd` opened with SIGKILL, as pidfd_send_signal sends a signal to the very
/// process the descriptor names. The kernel ends it after the call returns: [`pidfd_wait`] tells
/// when it has.
pub(crate) fn pidfd_kill(pidfd: &OwnedFd) -> io::Result<()> {
    // SAFETY: the call takes a descriptor open through it, a signal's number, no information about
    // the signal, which a null asks for, and no flags; it reaches only the process the descriptor
    // names.
    let sent = unsafe {
        libc::syscall(
            libc::SYS_pidfd_send_signal,
            pidfd.as_raw_fd(),
            libc::SIGKILL,
            std::ptr::null::<libc::siginfo_t>(),
            0_u32,
        )
    };
    if sent == -1 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// A copy of `file`'s descriptor without close-on-exec, numbered [`LEAKED_FROM`] or above, as a
/// library that opens a file without the flag leaves one: every program this process starts
/// inherits it unless it is marked.
#[cfg(test)]
pub(crate) fn leak_descriptor(file: &std::fs::File) -> io::Result<OwnedFd> {
    // SAFETY: F_DUPFD makes a new descriptor for the file `file` holds open, without
    // close-on-exec, and takes only numbers.
    let copy = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_DUPFD, LEAKED_FROM) };
    if copy == -1 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: `copy` is a descriptor just made, which nothing else owns or closes.
    Ok(unsafe { OwnedFd::from_raw_fd(copy) })
}

#[cfg(test)]
mod tests {
    use std::io;
    use std::process::{Command, Stdio};

    use super::exec_directly;

    /// A program of the system's own that does nothing and succeeds.
    const DOES_NOTHING: &str = "/bin/true";

    /// A variable the test changes for a command, which nothing reads.
    const VARIABLE: &str = "GDL_EXEC_DIRECTLY_TEST";

    #[test]
    fn a_changed_environment_is_refused() {
        // The program would run with this process's environment, not the one asked for, so a
        // command that sets or removes a variable is refused rather than started without it.
        let mut set = Command::new(DOES_NOTHING);
        set.env(VARIABLE, "1");
        let mut removed = Command::new(DOES_NOTHING);
        removed.env_remove(VARIABLE);
        for (what, mut command) in [("set", set), ("removed", removed)] {
            let error = exec_directly(&mut command).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{what}: {error}");
        }
        // A command that leaves the environment alone is started.
        let mut command = Command::new(DOES_NOTHING);
        command.stdin(Stdio::null());
        exec_directly(&mut command).unwrap();
        let status = command.status().unwrap();
        assert!(status.success(), "{status:?}");
    }
}
