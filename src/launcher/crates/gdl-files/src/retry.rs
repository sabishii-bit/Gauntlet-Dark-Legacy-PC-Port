//! Retries for the file operations that a passing hold on a file refuses on Windows, where a virus
//! scanner or an indexer may open a file the moment the launcher closes it, and a rename of the
//! file, or of a folder that holds it, fails until it lets go.

use std::io;
use std::thread;
use std::time::{Duration, Instant};

/// How long a refused file operation is tried again for, from before its first attempt.
pub const RETRY_WINDOW: Duration = Duration::from_secs(8);

/// The pause between one refused attempt and the next.
pub const RETRY_INTERVAL: Duration = Duration::from_millis(200);

/// The Windows errors a passing hold on a file gives, which a short wait can cure:
/// ERROR_ACCESS_DENIED (5), which renaming a folder gives while a file in it is open;
/// ERROR_SHARING_VIOLATION (32), which a file gives while another handle on it does not share the
/// access asked for, as a rename asks for deletion; and ERROR_LOCK_VIOLATION (33), which a range
/// of a file gives while another handle has it locked. They are matched by their raw codes, since
/// std gives the last two no kind of their own.
pub const RETRIED_WINDOWS_ERRORS: [i32; 3] = [5, 32, 33];

/// Runs a file operation and, on Windows, runs it again every [`RETRY_INTERVAL`] while it fails
/// with one of [`RETRIED_WINDOWS_ERRORS`], for up to [`RETRY_WINDOW`], after which its last error
/// is returned. Any other error, and every error on other systems, is returned at once. The
/// operation must be one that can run again after it fails; the calling thread waits between
/// attempts.
pub fn retry_locked<T>(op: impl FnMut() -> io::Result<T>) -> io::Result<T> {
    let mut clock = SystemClock {
        start: Instant::now(),
    };
    retry_with(op, &mut clock, cfg!(windows))
}

/// What a retry reads the time from and waits on, so that a test can stand in for the system.
pub(crate) trait RetryClock {
    /// The time passed since a point of the clock's choosing.
    fn now(&mut self) -> Duration;
    /// Waits for `pause` to pass.
    fn sleep(&mut self, pause: Duration);
}

/// Runs `op` as [`retry_locked`] does, on `clock`, and retries only when `windows` says that its
/// errors are Windows errors.
pub(crate) fn retry_with<T>(
    mut op: impl FnMut() -> io::Result<T>,
    clock: &mut dyn RetryClock,
    windows: bool,
) -> io::Result<T> {
    let deadline = clock.now().saturating_add(RETRY_WINDOW);
    loop {
        match op() {
            Err(error) if windows && is_retried(&error) && clock.now() < deadline => {
                clock.sleep(RETRY_INTERVAL);
            }
            result => return result,
        }
    }
}

/// Whether an error is one of [`RETRIED_WINDOWS_ERRORS`].
fn is_retried(error: &io::Error) -> bool {
    error
        .raw_os_error()
        .is_some_and(|code| RETRIED_WINDOWS_ERRORS.contains(&code))
}

/// The system's steady clock, read from when a retry began.
struct SystemClock {
    /// When the retry began.
    start: Instant,
}

impl RetryClock for SystemClock {
    fn now(&mut self) -> Duration {
        self.start.elapsed()
    }

    fn sleep(&mut self, pause: Duration) {
        thread::sleep(pause);
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::io;
    use std::time::{Duration, Instant};

    use super::{
        RETRIED_WINDOWS_ERRORS, RETRY_INTERVAL, RETRY_WINDOW, RetryClock, SystemClock,
        retry_locked, retry_with,
    };

    /// ERROR_ACCESS_DENIED: what renaming a folder gives while a file in it is open.
    const ACCESS_DENIED: i32 = 5;
    /// ERROR_SHARING_VIOLATION: what renaming a file gives while another handle on it does not
    /// share its deletion.
    const SHARING_VIOLATION: i32 = 32;
    /// ERROR_LOCK_VIOLATION: what reading a range of a file gives while another handle has it
    /// locked.
    const LOCK_VIOLATION: i32 = 33;
    /// ERROR_FILE_NOT_FOUND, which no wait cures.
    const FILE_NOT_FOUND: i32 = 2;
    /// ERROR_ALREADY_EXISTS, which no wait cures either.
    const ALREADY_EXISTS: i32 = 183;

    /// The pauses that fit in the window: eight seconds of 200 ms.
    const PAUSES_IN_WINDOW: u32 = 40;

    /// More attempts than a window holds, so that a retry that never ends fails its test instead
    /// of hanging it.
    const MOST_ATTEMPTS: usize = 1000;

    /// Where a test clock's time starts: anywhere but nought, since a retry measures its window
    /// from its first reading of the clock, not from the clock's own zero.
    const CLOCK_START: Duration = Duration::from_secs(1000);

    /// A clock whose time moves only when a test moves it: by each pause asked of it, and by the
    /// time an attempt under test says it took.
    struct TestClock<'a> {
        /// The time now, shared with the attempts.
        time: &'a Cell<Duration>,
        /// The pauses asked for, in order.
        pauses: Vec<Duration>,
    }

    impl RetryClock for TestClock<'_> {
        fn now(&mut self) -> Duration {
            self.time.get()
        }

        fn sleep(&mut self, pause: Duration) {
            self.pauses.push(pause);
            self.time.set(self.time.get() + pause);
        }
    }

    /// What a retry on the test clock did.
    struct Retried<T> {
        /// What the retry returned.
        result: io::Result<T>,
        /// When each attempt began, from the time the retry was asked for.
        starts: Vec<Duration>,
        /// The pauses between attempts.
        pauses: Vec<Duration>,
    }

    /// Retries `attempt` with `retry_with` on a test clock. Each attempt is given its number, from
    /// one, and says how long it took and how it ended.
    fn retry_on_test_clock<T>(
        windows: bool,
        mut attempt: impl FnMut(usize) -> (Duration, io::Result<T>),
    ) -> Retried<T> {
        let time = Cell::new(CLOCK_START);
        let mut clock = TestClock {
            time: &time,
            pauses: Vec::new(),
        };
        let mut starts = Vec::new();
        let result = retry_with(
            || {
                starts.push(time.get() - CLOCK_START);
                assert!(starts.len() <= MOST_ATTEMPTS, "the retries never end");
                let (took, result) = attempt(starts.len());
                time.set(time.get() + took);
                result
            },
            &mut clock,
            windows,
        );
        Retried {
            result,
            starts,
            pauses: clock.pauses,
        }
    }

    /// The error the system reports as raw `code`.
    fn os_error(code: i32) -> io::Error {
        io::Error::from_raw_os_error(code)
    }

    /// An attempt that takes no time and fails with raw `code`.
    fn failing(code: i32) -> (Duration, io::Result<()>) {
        (Duration::ZERO, Err(os_error(code)))
    }

    #[test]
    fn a_transient_lock_is_retried_once() {
        assert_eq!(
            RETRIED_WINDOWS_ERRORS,
            [ACCESS_DENIED, SHARING_VIOLATION, LOCK_VIOLATION]
        );
        assert_eq!(RETRY_INTERVAL, Duration::from_millis(200));
        for code in RETRIED_WINDOWS_ERRORS {
            let retried = retry_on_test_clock(true, |attempt| {
                let result = if attempt == 1 {
                    Err(os_error(code))
                } else {
                    Ok(42)
                };
                (Duration::ZERO, result)
            });
            assert_eq!(retried.result.unwrap(), 42, "error {code}");
            assert_eq!(
                retried.starts,
                [Duration::ZERO, RETRY_INTERVAL],
                "error {code}"
            );
            assert_eq!(retried.pauses, [RETRY_INTERVAL], "error {code}");
        }
    }

    #[test]
    fn a_permanent_lock_ends_at_the_deadline() {
        // An attempt that outlasts the window is the last: the clock moves on 9 s while it runs,
        // as in the release scripts' test of the same rule.
        let retried = retry_on_test_clock(true, |_| {
            (
                Duration::from_secs(9),
                Err::<(), _>(os_error(ACCESS_DENIED)),
            )
        });
        let error = retried.result.unwrap_err();
        assert_eq!(error.raw_os_error(), Some(ACCESS_DENIED));
        assert_eq!(retried.starts, [Duration::ZERO]);
        assert!(retried.pauses.is_empty());

        // Quick attempts go on through the whole window, and one more begins as it closes. Each
        // fails with the next of the retried errors, so the error returned tells which gave it.
        assert_eq!(RETRY_WINDOW, Duration::from_secs(8));
        let kinds = RETRIED_WINDOWS_ERRORS.len();
        let retried = retry_on_test_clock(true, |attempt| {
            failing(RETRIED_WINDOWS_ERRORS[(attempt - 1) % kinds])
        });
        let starts: Vec<Duration> = (0..=PAUSES_IN_WINDOW)
            .map(|pause| RETRY_INTERVAL * pause)
            .collect();
        assert_eq!(starts.last(), Some(&RETRY_WINDOW));
        assert_eq!(retried.starts, starts);
        assert_eq!(retried.pauses, vec![RETRY_INTERVAL; starts.len() - 1]);
        let last = RETRIED_WINDOWS_ERRORS[(starts.len() - 1) % kinds];
        assert_eq!(retried.result.unwrap_err().raw_os_error(), Some(last));
    }

    #[test]
    fn other_errors_and_other_systems_are_not_retried() {
        // Errors no wait cures, and errors that only look like a hold: without a raw code, not
        // even a denial is one.
        let errors: [fn() -> io::Error; 4] = [
            || os_error(FILE_NOT_FOUND),
            || os_error(ALREADY_EXISTS),
            || io::ErrorKind::PermissionDenied.into(),
            || io::ErrorKind::AlreadyExists.into(),
        ];
        for error in errors {
            let expected = error();
            let retried = retry_on_test_clock(true, |_| (Duration::ZERO, Err::<(), _>(error())));
            let returned = retried.result.unwrap_err();
            assert_eq!(
                returned.raw_os_error(),
                expected.raw_os_error(),
                "{expected}"
            );
            assert_eq!(returned.kind(), expected.kind(), "{expected}");
            assert_eq!(retried.starts, [Duration::ZERO], "{expected}");
            assert!(retried.pauses.is_empty(), "{expected}");
        }
        // On other systems these numbers are other errors, and nothing is retried.
        for code in RETRIED_WINDOWS_ERRORS {
            let retried = retry_on_test_clock(false, |_| failing(code));
            assert_eq!(retried.result.unwrap_err().raw_os_error(), Some(code));
            assert_eq!(retried.starts, [Duration::ZERO], "error {code}");
            assert!(retried.pauses.is_empty(), "error {code}");
        }
        // A first attempt that succeeds is the only one, on every system.
        for windows in [true, false] {
            let retried = retry_on_test_clock(windows, |_| (Duration::ZERO, Ok(7)));
            assert_eq!(retried.result.unwrap(), 7);
            assert_eq!(retried.starts, [Duration::ZERO]);
            assert!(retried.pauses.is_empty());
        }
    }

    #[test]
    fn retry_locked_waits_out_a_lock_only_on_windows() {
        let started = Instant::now();
        let mut attempts = 0;
        let result = retry_locked(|| {
            attempts += 1;
            if attempts == 1 {
                Err(os_error(SHARING_VIOLATION))
            } else {
                Ok(42)
            }
        });
        if cfg!(windows) {
            assert_eq!(result.unwrap(), 42);
            assert_eq!(attempts, 2);
            // The system's clock really waits, and never for less than the pause.
            assert!(started.elapsed() >= RETRY_INTERVAL);
        } else {
            // Elsewhere 32 is another error, a broken pipe on Linux.
            let error = result.unwrap_err();
            assert_eq!(error.raw_os_error(), Some(SHARING_VIOLATION));
            assert_eq!(attempts, 1);
        }
    }

    #[test]
    fn the_system_clock_moves_with_its_pauses() {
        let mut clock = SystemClock {
            start: Instant::now(),
        };
        let before = clock.now();
        clock.sleep(RETRY_INTERVAL);
        assert!(clock.now() >= before + RETRY_INTERVAL);
    }

    /// Retries against real holds on files, which only Windows has.
    #[cfg(windows)]
    mod real_holds {
        use std::fs::{self, File, OpenOptions};
        use std::io;
        use std::os::windows::fs::OpenOptionsExt;
        use std::path::Path;
        use std::sync::mpsc;
        use std::thread;

        use super::{ACCESS_DENIED, SHARING_VIOLATION, retry_locked};

        /// Keeps `handle` open on a thread of its own until the first refusal is told on the
        /// sender, or the sender is dropped, then closes it, as a scanner closes a file it has
        /// read.
        fn close_after_refusal(handle: File) -> (mpsc::Sender<()>, thread::JoinHandle<()>) {
            let (refused, release) = mpsc::channel();
            let holder = thread::spawn(move || {
                // A hang-up means no attempt was refused, which the test reports.
                let _ = release.recv();
                drop(handle);
            });
            (refused, holder)
        }

        /// Renames `from` to `to` through `retry_locked`, telling `refused` of the first refusal,
        /// and gives back what it returned with each refusal's raw code.
        fn rename_through_holds(
            from: &Path,
            to: &Path,
            refused: mpsc::Sender<()>,
        ) -> (io::Result<()>, Vec<Option<i32>>) {
            let mut refusals = Vec::new();
            let result = retry_locked(|| {
                let result = fs::rename(from, to);
                if let Err(error) = &result {
                    if refusals.is_empty() {
                        let _ = refused.send(());
                    }
                    refusals.push(error.raw_os_error());
                }
                result
            });
            (result, refusals)
        }

        #[test]
        fn a_file_is_renamed_once_a_handle_that_does_not_share_it_closes() {
            let folder = tempfile::tempdir().unwrap();
            let from = folder.path().join("scanned.json");
            let to = folder.path().join("published.json");
            fs::write(&from, b"{}").unwrap();
            // A handle that shares nothing refuses the rename, whoever asks for it. It is taken
            // once any real scanner of the file just written has let it go.
            let handle =
                retry_locked(|| OpenOptions::new().read(true).share_mode(0).open(&from)).unwrap();
            let (refused, holder) = close_after_refusal(handle);
            let (result, refusals) = rename_through_holds(&from, &to, refused);
            holder.join().unwrap();
            result.unwrap();
            assert_eq!(refusals.first(), Some(&Some(SHARING_VIOLATION)));
            assert_eq!(fs::read(&to).unwrap(), b"{}");
            assert!(!from.exists());
        }

        #[test]
        fn a_folder_is_renamed_once_a_file_in_it_closes() {
            let folder = tempfile::tempdir().unwrap();
            let from = folder.path().join("stage");
            let to = folder.path().join("data");
            fs::create_dir(&from).unwrap();
            fs::write(from.join("config.json"), b"{}").unwrap();
            // Even a handle that shares everything, as std's own do, refuses a rename of the
            // folder above its file.
            let handle = File::open(from.join("config.json")).unwrap();
            let (refused, holder) = close_after_refusal(handle);
            let (result, refusals) = rename_through_holds(&from, &to, refused);
            holder.join().unwrap();
            result.unwrap();
            assert_eq!(refusals.first(), Some(&Some(ACCESS_DENIED)));
            assert_eq!(fs::read(to.join("config.json")).unwrap(), b"{}");
            assert!(!from.exists());
        }
    }
}
