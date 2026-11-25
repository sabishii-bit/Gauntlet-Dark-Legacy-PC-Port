//! Waiting out what holds a job back that the player can end, rather than failing for it: another
//! job's hold on the installation's lock, and a game that runs from the installation. A held job
//! tells its progress once as the wait begins and once as it ends, looks again every poll and at
//! its cancel many times a poll, and changes nothing while it waits; a cancel ends the wait.

use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};

use gdl_files::{Cancelled, Hold, Job};

use crate::folder::{InstallFolder, InstallLock, LockError};
use crate::probe::GameUseError;

/// How often a hold looks again whether what holds the job has ended.
pub const HOLD_POLL: Duration = Duration::from_secs(1);

/// How often, within a poll, a hold looks at the job's cancel.
pub(crate) const HOLD_CANCEL_SLICE: Duration = Duration::from_millis(50);

/// How long a game found running is asked again on Windows before the job holds for it: a scanner
/// may hold an executable that was just closed for a moment, which the closed-game probe cannot
/// tell from the game.
pub(crate) const SCANNER_GRACE: Duration = Duration::from_secs(2);

/// How often the game is asked again within [`SCANNER_GRACE`].
pub(crate) const SCANNER_RETRY: Duration = Duration::from_millis(200);

/// Why taking the lock while holding for it ended without the lock.
#[derive(Debug)]
pub(crate) enum HeldLock {
    /// The lock was refused for another reason than another job's hold.
    Lock(LockError),
    /// The job was asked to stop while it waited.
    Cancelled,
}

/// Why waiting for the game to close ended without it closed.
#[derive(Debug)]
pub(crate) enum HeldGame {
    /// The closed-game probe failed for another reason than a running game.
    Game(GameUseError),
    /// The job was asked to stop while it waited.
    Cancelled,
}

/// A hold of a job's, told to its progress once as it begins and once as it ends.
struct Holding<'a, 'p> {
    /// The job held.
    job: &'a Job<'p>,
    /// What holds it.
    hold: Hold,
    /// Whether the hold has begun and not yet ended.
    held: bool,
}

impl<'a, 'p> Holding<'a, 'p> {
    /// A hold of `job` for `hold`, not yet begun.
    fn new(job: &'a Job<'p>, hold: Hold) -> Self {
        Holding {
            job,
            hold,
            held: false,
        }
    }

    /// The job waits: told once, however many polls it waits.
    fn begin(&mut self) {
        if !self.held {
            self.job.progress.hold(self.hold);
            self.held = true;
        }
    }

    /// The job goes on: told once the hold has begun.
    fn end(&mut self) {
        if self.held {
            self.job.progress.resumed();
            self.held = false;
        }
    }
}

/// Waits `pause`, looking at the job's cancel as it begins and every [`HOLD_CANCEL_SLICE`] within
/// it.
fn wait(job: &Job<'_>, pause: Duration) -> Result<(), Cancelled> {
    let deadline = Instant::now() + pause;
    loop {
        job.cancel.check()?;
        let now = Instant::now();
        if now >= deadline {
            return Ok(());
        }
        thread::sleep((deadline - now).min(HOLD_CANCEL_SLICE));
    }
}

/// Takes the installation's lock in `folder`, holding the job ([`Hold::LockBusy`]) while another
/// job has it, and looking again every `poll`. Any other refusal of the lock ends it at once.
pub(crate) fn lock_holding(
    folder: &InstallFolder,
    job: &Job<'_>,
    poll: Duration,
) -> Result<InstallLock, HeldLock> {
    let mut holding = Holding::new(job, Hold::LockBusy);
    loop {
        match folder.lock() {
            Ok(lock) => {
                holding.end();
                return Ok(lock);
            }
            Err(LockError::Busy) => {
                holding.begin();
                wait(job, poll).map_err(|Cancelled| HeldLock::Cancelled)?;
            }
            Err(error) => return Err(HeldLock::Lock(error)),
        }
    }
}

/// Waits until the game is closed at every place of `executables`, in turn: the installation's
/// executable, and for a recovery the copy an update moved aside. `closed` is the closed-game
/// probe, [`require_game_closed`](crate::require_game_closed) or a test's stand-in. A place with
/// nothing there is passed over. While the game runs at one, the job holds
/// ([`Hold::GameRunning`]), looking again every `poll`; on Windows a game found running is first
/// asked again every [`SCANNER_RETRY`] for up to [`SCANNER_GRACE`], and held for only past that.
/// Any other failure of the probe ends the wait at once.
pub(crate) fn game_closed_holding(
    executables: &[&Path],
    closed: &dyn Fn(&Path) -> Result<(), GameUseError>,
    job: &Job<'_>,
    poll: Duration,
) -> Result<(), HeldGame> {
    let mut holding = Holding::new(job, Hold::GameRunning);
    for executable in executables {
        loop {
            match answer(executable, closed, job, !holding.held)? {
                Ok(()) | Err(GameUseError::Missing) => break,
                Err(GameUseError::Running) => {
                    holding.begin();
                    wait(job, poll).map_err(|Cancelled| HeldGame::Cancelled)?;
                }
                Err(error) => return Err(HeldGame::Game(error)),
            }
        }
    }
    holding.end();
    Ok(())
}

/// What the probe `closed` answers of the game at `executable`. A game found running is asked
/// again within the scanner's grace first, when `grace` and on Windows alone, where a scanner's
/// hold on a just-closed file reads as a running game; the cancel is looked at between.
fn answer(
    executable: &Path,
    closed: &dyn Fn(&Path) -> Result<(), GameUseError>,
    job: &Job<'_>,
    grace: bool,
) -> Result<Result<(), GameUseError>, HeldGame> {
    let first = closed(executable);
    if !(grace && cfg!(windows) && matches!(first, Err(GameUseError::Running))) {
        return Ok(first);
    }
    let deadline = Instant::now() + SCANNER_GRACE;
    while Instant::now() < deadline {
        wait(job, SCANNER_RETRY).map_err(|Cancelled| HeldGame::Cancelled)?;
        match closed(executable) {
            Err(GameUseError::Running) => {}
            other => return Ok(other),
        }
    }
    Ok(Err(GameUseError::Running))
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::sync::Mutex;
    use std::sync::mpsc;
    use std::thread;
    use std::time::{Duration, Instant};

    use gdl_files::{Cancel, Hold, Job, PhaseId, Progress, RelPath};

    use super::{HeldGame, HeldLock, SCANNER_GRACE, game_closed_holding, lock_holding};
    use crate::folder::{InstallFolder, STATE_DIR};
    use crate::probe::{GameUseError, require_game_closed};

    /// How often the tests' holds look again: far more often than a job's, so that they run fast.
    const POLL: Duration = Duration::from_millis(20);

    /// How long another job holds the lock in the lock's test.
    const LOCK_HELD_FOR: Duration = Duration::from_millis(2500);

    /// How long a stand-in game runs past the time a hold would begin: on Windows, past the
    /// scanner's grace.
    fn running_for() -> Duration {
        let past_hold = Duration::from_millis(300);
        if cfg!(windows) {
            SCANNER_GRACE + past_hold
        } else {
            past_hold
        }
    }

    /// What a job told its progress of its holds.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Told {
        Held(Hold),
        Resumed,
    }

    /// A progress that keeps what it is told of holds, and asks the job to stop as a hold begins
    /// when it is given a cancel.
    #[derive(Default)]
    struct Holds<'a> {
        told: Mutex<Vec<Told>>,
        cancel_on_hold: Option<&'a Cancel>,
    }

    impl Holds<'_> {
        fn told(&self) -> Vec<Told> {
            self.told.lock().unwrap().clone()
        }
    }

    impl Progress for Holds<'_> {
        fn phase(&self, _: PhaseId) {}

        fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}

        fn hold(&self, hold: Hold) {
            self.told.lock().unwrap().push(Told::Held(hold));
            if let Some(cancel) = self.cancel_on_hold {
                cancel.cancel();
            }
        }

        fn resumed(&self) {
            self.told.lock().unwrap().push(Told::Resumed);
        }
    }

    /// An installation's folder with an empty state folder, in a folder of the test's own.
    fn installation() -> (tempfile::TempDir, InstallFolder) {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("install");
        fs::create_dir_all(root.join(STATE_DIR)).unwrap();
        let folder = InstallFolder::at(&root).unwrap();
        (dir, folder)
    }

    /// A stand-in for the closed-game probe that finds the game running at `running` until
    /// `until`, and closed, or not there, elsewhere, counting its calls.
    struct StandIn {
        running: PathBuf,
        until: Instant,
        calls: Cell<u32>,
    }

    impl StandIn {
        fn new(running: &Path, for_how_long: Duration) -> StandIn {
            StandIn {
                running: running.to_path_buf(),
                until: Instant::now() + for_how_long,
                calls: Cell::new(0),
            }
        }

        fn closed(&self, executable: &Path) -> Result<(), GameUseError> {
            self.calls.set(self.calls.get() + 1);
            if executable != self.running {
                return Err(GameUseError::Missing);
            }
            if Instant::now() < self.until {
                Err(GameUseError::Running)
            } else {
                Ok(())
            }
        }
    }

    #[test]
    fn a_lock_another_job_holds_is_waited_for() {
        let (_dir, folder) = installation();
        let (taken, has_it) = mpsc::channel();
        let other = folder.clone();
        let holder = thread::spawn(move || {
            let lock = other.lock().unwrap();
            taken.send(()).unwrap();
            thread::sleep(LOCK_HELD_FOR);
            drop(lock);
        });
        has_it.recv().unwrap();
        let holds = Holds::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        let begun = Instant::now();
        let lock = lock_holding(&folder, &job, POLL).unwrap();
        // The lock was taken only once the other job let it go.
        assert!(holder.is_finished() || begun.elapsed() >= LOCK_HELD_FOR / 2);
        assert_eq!(holds.told(), [Told::Held(Hold::LockBusy), Told::Resumed]);
        holder.join().unwrap();
        drop(lock);
        // A free lock is taken with no hold told.
        let holds = Holds::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        drop(lock_holding(&folder, &job, POLL).unwrap());
        assert!(holds.told().is_empty());
    }

    #[test]
    fn a_cancel_ends_a_hold() {
        let (_dir, folder) = installation();
        let held = folder.lock().unwrap();
        let cancel = Cancel::default();
        let holds = Holds {
            cancel_on_hold: Some(&cancel),
            ..Holds::default()
        };
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        let begun = Instant::now();
        let refused = lock_holding(&folder, &job, Duration::from_secs(30));
        assert!(matches!(refused, Err(HeldLock::Cancelled)), "{refused:?}");
        assert!(begun.elapsed() < Duration::from_secs(5));
        assert_eq!(holds.told(), [Told::Held(Hold::LockBusy)]);
        drop(held);

        // The same for a running game, which is never asked again once the job stops.
        let executable = folder.root().join("gauntlet");
        let cancel = Cancel::default();
        let holds = Holds {
            cancel_on_hold: Some(&cancel),
            ..Holds::default()
        };
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        let stand_in = StandIn::new(&executable, Duration::from_secs(3600));
        let begun = Instant::now();
        let refused = game_closed_holding(
            &[&executable],
            &|path| stand_in.closed(path),
            &job,
            Duration::from_secs(30),
        );
        assert!(matches!(refused, Err(HeldGame::Cancelled)), "{refused:?}");
        assert!(begun.elapsed() < SCANNER_GRACE + Duration::from_secs(5));
        assert_eq!(holds.told(), [Told::Held(Hold::GameRunning)]);
    }

    #[test]
    fn a_game_that_closes_is_waited_for() {
        let (_dir, folder) = installation();
        let executable = folder.root().join("gauntlet");
        let stand_in = StandIn::new(&executable, running_for());
        let holds = Holds::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        game_closed_holding(&[&executable], &|path| stand_in.closed(path), &job, POLL).unwrap();
        assert_eq!(holds.told(), [Told::Held(Hold::GameRunning), Told::Resumed]);
        // It was asked again until it closed, and no more.
        assert!(stand_in.calls.get() >= 3, "{}", stand_in.calls.get());
        assert!(Instant::now() >= stand_in.until);
    }

    #[cfg(windows)]
    #[test]
    fn a_scanners_moment_is_no_hold() {
        let (_dir, folder) = installation();
        let executable = folder.root().join("gauntlet.exe");
        let answers = Cell::new(0);
        let closed = |_: &Path| {
            answers.set(answers.get() + 1);
            if answers.get() <= 2 {
                Err(GameUseError::Running)
            } else {
                Ok(())
            }
        };
        let holds = Holds::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        game_closed_holding(&[&executable], &closed, &job, POLL).unwrap();
        assert!(holds.told().is_empty());
        assert_eq!(answers.get(), 3);
    }

    /// Keeps the game at `executable` in use for `how_long`, from another thread, as the system
    /// holds a running program's file: on Windows open, sharing reading alone; on Linux run, as a
    /// copy of the system's own waiting program. Gives the thread, which ends once the game has.
    #[cfg(windows)]
    fn keep_in_use(executable: &Path, how_long: Duration) -> thread::JoinHandle<()> {
        use std::os::windows::fs::OpenOptionsExt;

        /// FILE_SHARE_READ: what a handle that lets others only read beside it shares.
        const SHARE_READ: u32 = 0x1;

        fs::write(executable, b"not a real executable").unwrap();
        let held = fs::OpenOptions::new()
            .read(true)
            .share_mode(SHARE_READ)
            .open(executable)
            .unwrap();
        thread::spawn(move || {
            thread::sleep(how_long);
            drop(held);
        })
    }

    /// Keeps the game at `executable` in use for `how_long`, from another thread, as the system
    /// holds a running program's file: on Windows open, sharing reading alone; on Linux run, as a
    /// copy of the system's own waiting program. Gives the thread, which ends once the game has.
    #[cfg(unix)]
    fn keep_in_use(executable: &Path, how_long: Duration) -> thread::JoinHandle<()> {
        use std::os::unix::fs::PermissionsExt;
        use std::process::{Command, Stdio};

        fs::copy("/bin/sleep", executable).unwrap();
        fs::set_permissions(executable, fs::Permissions::from_mode(0o755)).unwrap();
        let seconds = format!("{:.3}", how_long.as_secs_f64());
        let mut child = Command::new(executable)
            .arg(seconds)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        thread::spawn(move || {
            child.wait().unwrap();
        })
    }

    #[test]
    fn a_held_executable_holds_the_job() {
        let (_dir, folder) = installation();
        let executable = folder.root().join(if cfg!(windows) {
            "gauntlet.exe"
        } else {
            "gauntlet"
        });
        let in_use = keep_in_use(&executable, running_for() + Duration::from_millis(1200));
        let holds = Holds::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        game_closed_holding(&[&executable], &require_game_closed, &job, POLL).unwrap();
        assert_eq!(holds.told(), [Told::Held(Hold::GameRunning), Told::Resumed]);
        in_use.join().unwrap();
    }

    #[test]
    fn a_missing_executable_is_no_hold() {
        let (_dir, folder) = installation();
        let root = folder.root().join("gauntlet");
        let backup = folder.state_dir().join("update-backup").join("gauntlet");
        let stand_in = StandIn::new(Path::new("elsewhere"), Duration::from_secs(3600));
        let holds = Holds::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        let begun = Instant::now();
        game_closed_holding(
            &[&root, &backup],
            &|path| stand_in.closed(path),
            &job,
            Duration::from_secs(30),
        )
        .unwrap();
        assert!(begun.elapsed() < Duration::from_secs(1));
        assert!(holds.told().is_empty());
        assert_eq!(stand_in.calls.get(), 2);
    }

    #[test]
    fn every_place_is_waited_for() {
        let (_dir, folder) = installation();
        let root = folder.root().join("gauntlet");
        let backup = folder.state_dir().join("update-backup").join("gauntlet");
        let stand_in = StandIn::new(&backup, running_for());
        let holds = Holds::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        game_closed_holding(&[&root, &backup], &|path| stand_in.closed(path), &job, POLL).unwrap();
        assert_eq!(holds.told(), [Told::Held(Hold::GameRunning), Told::Resumed]);
        assert!(Instant::now() >= stand_in.until);
    }

    #[test]
    fn a_probe_that_fails_ends_the_wait() {
        let (_dir, folder) = installation();
        let executable = folder.root().join("gauntlet");
        let holds = Holds::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &holds,
        };
        let refused = game_closed_holding(
            &[&executable],
            &|path| {
                Err(GameUseError::NotWritable {
                    path: path.to_path_buf(),
                })
            },
            &job,
            POLL,
        );
        match refused {
            Err(HeldGame::Game(GameUseError::NotWritable { path })) => {
                assert_eq!(path, executable);
            }
            other => panic!("{other:?}"),
        }
        assert!(holds.told().is_empty());
    }
}
