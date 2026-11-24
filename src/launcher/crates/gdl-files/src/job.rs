//! What a long file operation takes from the job it is part of: a way to be told to stop, which it
//! looks at between blocks of its work, and a place to tell how far it has come and what it waits
//! on; and how a block of that work is filled from what it reads.

use std::io::{self, Read};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::RelPath;

/// The most bytes a job reads or writes between two looks at its [`Cancel`], so that a request to
/// stop is heeded within one block.
pub const BLOCK_LEN: usize = 1 << 20;

/// Reads from `reader` until `block` is full or the reader ends, reading again after a read the
/// system interrupted, and gives how many bytes it filled: fewer than `block` holds only where the
/// reader ends.
pub fn fill_from<R: Read + ?Sized>(reader: &mut R, block: &mut [u8]) -> io::Result<usize> {
    let mut filled = 0;
    while filled < block.len() {
        match reader.read(&mut block[filled..]) {
            Ok(0) => break,
            Ok(read) => filled += read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(filled)
}

/// A request to stop a job, shared by every clone: a request made through one is seen through all
/// of them, on any thread.
#[derive(Clone, Debug, Default)]
pub struct Cancel(Arc<AtomicBool>);

impl Cancel {
    /// Asks the job to stop at its next look. A request cannot be taken back.
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Release);
    }

    /// Whether the job has been asked to stop.
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }

    /// Fails with [`Cancelled`] once the job has been asked to stop: what a job calls before each
    /// block of its work.
    pub fn check(&self) -> Result<(), Cancelled> {
        if self.is_cancelled() {
            Err(Cancelled)
        } else {
            Ok(())
        }
    }
}

/// What a job reports when it stops because it was asked to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
#[error("cancelled")]
pub struct Cancelled;

/// The name of a step of a job, such as staging or publishing, that its progress shows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PhaseId(pub &'static str);

/// A condition a job waits on that the player can end, rather than fail for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Hold {
    /// The game runs from the installation.
    GameRunning,
    /// Another job holds the installation's lock.
    LockBusy,
}

impl Hold {
    /// What ends the hold, as a job's events name it to the window: `gameClosed` or `lockFree`.
    pub fn resumes_when(self) -> &'static str {
        match self {
            Hold::GameRunning => "gameClosed",
            Hold::LockBusy => "lockFree",
        }
    }
}

/// Where a job tells how far it has come and what it waits on, from whichever thread it runs on.
pub trait Progress: Send + Sync {
    /// The job has begun `phase`.
    fn phase(&self, phase: PhaseId);
    /// `done` of the `total` bytes, or other units, of the job's phase are done; `item` names
    /// what the last of them belonged to, when they belonged to a name.
    fn advance(&self, done: u64, total: u64, item: Option<&RelPath>);
    /// The job waits for `hold` to end, keeping what it made.
    fn hold(&self, _hold: Hold) {}
    /// The hold the job waited on ended, and the job goes on.
    fn resumed(&self) {}
}

/// Progress that goes nowhere, for a job nobody follows.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoProgress;

impl Progress for NoProgress {
    fn phase(&self, _: PhaseId) {}

    fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
}

/// One part of a job's work told to the job's progress as part of the whole: the units of the
/// parts before it and its own, out of the job's total.
#[derive(Clone, Copy)]
pub struct RunningTotal<'a> {
    /// The job's own progress.
    pub job: &'a dyn Progress,
    /// The units of the parts done before this one.
    pub before: u64,
    /// The units of every part of the job.
    pub total: u64,
}

impl Progress for RunningTotal<'_> {
    fn phase(&self, phase: PhaseId) {
        self.job.phase(phase);
    }

    fn advance(&self, done: u64, _: u64, item: Option<&RelPath>) {
        self.job.advance(self.before + done, self.total, item);
    }

    fn hold(&self, hold: Hold) {
        self.job.hold(hold);
    }

    fn resumed(&self) {
        self.job.resumed();
    }
}

/// The job a long file operation is part of: how it is asked to stop and where its progress goes.
#[derive(Clone, Copy)]
pub struct Job<'a> {
    /// The request to stop, looked at before each block.
    pub cancel: &'a Cancel,
    /// Where the work done is told.
    pub progress: &'a dyn Progress,
}

#[cfg(test)]
mod tests {
    use std::io::{self, Read};
    use std::sync::Mutex;
    use std::thread;

    use super::{
        BLOCK_LEN, Cancel, Cancelled, Hold, Job, NoProgress, PhaseId, Progress, RunningTotal,
        fill_from,
    };
    use crate::{PAYLOAD_NAMES, RelPath};

    /// A reader of `bytes` that gives at most three of them a read, and is interrupted before
    /// every read that gives any, as a read a signal cuts short is; then fails with `last`, or
    /// ends when there is none.
    struct Halting {
        bytes: Vec<u8>,
        at: usize,
        interrupted: bool,
        last: Option<io::ErrorKind>,
    }

    impl Halting {
        fn new(bytes: &[u8], last: Option<io::ErrorKind>) -> Halting {
            Halting {
                bytes: bytes.to_vec(),
                at: 0,
                interrupted: false,
                last,
            }
        }
    }

    impl Read for Halting {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            if self.at == self.bytes.len() {
                return self.last.map_or(Ok(0), |kind| Err(kind.into()));
            }
            self.interrupted = !self.interrupted;
            if self.interrupted {
                return Err(io::ErrorKind::Interrupted.into());
            }
            let count = buffer.len().min(3).min(self.bytes.len() - self.at);
            buffer[..count].copy_from_slice(&self.bytes[self.at..self.at + count]);
            self.at += count;
            Ok(count)
        }
    }

    #[test]
    fn a_block_is_filled_as_far_as_its_reader_reaches() {
        let bytes: Vec<u8> = (1..=10).collect();
        // A reader that holds more than the block fills it, read by read, past each interruption.
        let mut block = [0; 8];
        let filled = fill_from(&mut Halting::new(&bytes, None), &mut block).unwrap();
        assert_eq!((filled, block), (8, [1, 2, 3, 4, 5, 6, 7, 8]));
        // One that ends first fills as much as it holds.
        let mut block = [0; 16];
        let filled = fill_from(&mut Halting::new(&bytes, None), &mut block).unwrap();
        assert_eq!((filled, &block[..10]), (10, bytes.as_slice()));
        assert_eq!(block[10..], [0; 6]);
        // Any other failure is the reader's, whatever it filled before.
        let mut failing = Halting::new(&bytes, Some(io::ErrorKind::BrokenPipe));
        let error = fill_from(&mut failing, &mut block).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::BrokenPipe);
        // An empty block is filled without a read.
        let mut none = Halting::new(&[], Some(io::ErrorKind::BrokenPipe));
        assert_eq!(fill_from(&mut none, &mut []).unwrap(), 0);
    }

    #[test]
    fn a_cancel_reaches_every_clone() {
        let cancel = Cancel::default();
        let clones = [cancel.clone(), cancel.clone()];
        let unrelated = Cancel::default();
        for one in clones.iter().chain([&cancel, &unrelated]) {
            assert!(!one.is_cancelled());
            assert_eq!(one.check(), Ok(()));
        }

        // A clone on another thread asks, and every clone, this thread's too, sees it.
        let asker = clones[1].clone();
        thread::spawn(move || asker.cancel()).join().unwrap();
        for one in clones.iter().chain([&cancel]) {
            assert!(one.is_cancelled());
            assert_eq!(one.check(), Err(Cancelled));
        }
        // So does a clone made afterwards, and asking again changes nothing.
        let later = cancel.clone();
        later.cancel();
        assert_eq!(later.check(), Err(Cancelled));
        assert_eq!(cancel.check(), Err(Cancelled));

        // A cancel made apart from it is not asked.
        assert!(!unrelated.is_cancelled());
        assert_eq!(unrelated.check(), Ok(()));
        assert_eq!(Cancelled.to_string(), "cancelled");
    }

    /// Progress that writes down everything it is told.
    #[derive(Default)]
    struct Recorded {
        /// The phases begun, the steps, the holds and their ends told, in order.
        told: Mutex<Vec<String>>,
    }

    impl Progress for Recorded {
        fn phase(&self, phase: PhaseId) {
            self.told.lock().unwrap().push(format!("phase {}", phase.0));
        }

        fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
            let item = item.map_or("-", RelPath::as_str);
            let step = format!("{done}/{total} {item}");
            self.told.lock().unwrap().push(step);
        }

        fn hold(&self, hold: Hold) {
            self.told.lock().unwrap().push(format!("hold {hold:?}"));
        }

        fn resumed(&self) {
            self.told.lock().unwrap().push("resumed".to_owned());
        }
    }

    /// Progress that writes down only its phases and steps, leaving holds to the defaults.
    #[derive(Default)]
    struct StepsOnly {
        /// The phases begun and the steps told, in order.
        told: Mutex<Vec<String>>,
    }

    impl Progress for StepsOnly {
        fn phase(&self, phase: PhaseId) {
            self.told.lock().unwrap().push(format!("phase {}", phase.0));
        }

        fn advance(&self, done: u64, total: u64, _: Option<&RelPath>) {
            self.told.lock().unwrap().push(format!("{done}/{total}"));
        }
    }

    /// Works through `total` bytes in blocks of at most [`BLOCK_LEN`], as a long file operation
    /// does: the job's cancel looked at before each, its progress told after.
    fn work(job: Job<'_>, total: u64, item: &RelPath) -> Result<(), Cancelled> {
        job.progress.phase(PhaseId("staging"));
        let block = u64::try_from(BLOCK_LEN).unwrap();
        let mut done = 0;
        while done < total {
            job.cancel.check()?;
            done = total.min(done + block);
            job.progress.advance(done, total, Some(item));
        }
        Ok(())
    }

    #[test]
    fn a_job_carries_its_cancel_and_its_progress() {
        assert_eq!(BLOCK_LEN, 1024 * 1024);
        let item = RelPath::parse("data/config.json", &PAYLOAD_NAMES).unwrap();
        let block = u64::try_from(BLOCK_LEN).unwrap();
        let cancel = Cancel::default();
        let progress = Recorded::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        work(job, 2 * block + 1, &item).unwrap();
        let told = progress.told.lock().unwrap().clone();
        let expected = [
            "phase staging".to_owned(),
            format!("{block}/{} data/config.json", 2 * block + 1),
            format!("{}/{} data/config.json", 2 * block, 2 * block + 1),
            format!("{}/{} data/config.json", 2 * block + 1, 2 * block + 1),
        ];
        assert_eq!(told, expected);

        // A copy of the job shares its cancel: asked to stop, it stops before its next block.
        let copy = job;
        cancel.cancel();
        assert_eq!(work(copy, block, &item), Err(Cancelled));
        assert_eq!(progress.told.lock().unwrap().len(), expected.len() + 1);

        // Nobody follows a job given no progress, which is told everything all the same.
        let quiet = Cancel::default();
        let unfollowed = Job {
            cancel: &quiet,
            progress: &NoProgress,
        };
        work(unfollowed, block, &item).unwrap();
        NoProgress.advance(1, 1, None);
    }

    #[test]
    fn a_running_total_adds_the_parts_before() {
        let block = u64::try_from(BLOCK_LEN).unwrap();
        let config = RelPath::parse("data/config.json", &PAYLOAD_NAMES).unwrap();
        let shader = RelPath::parse("shaders/test.spv", &PAYLOAD_NAMES).unwrap();
        // Three parts of one job, the middle one empty, each worked through as its own total.
        let parts = [(block + 1, &config), (0, &config), (block, &shader)];
        let total: u64 = parts.iter().map(|(len, _)| len).sum();
        let cancel = Cancel::default();
        let progress = Recorded::default();
        let mut before = 0;
        for (len, item) in parts {
            let part = RunningTotal {
                job: &progress,
                before,
                total,
            };
            let job = Job {
                cancel: &cancel,
                progress: &part,
            };
            work(job, len, item).unwrap();
            before += len;
        }
        assert_eq!(before, total);
        // The job is told each part's phase, and each step out of the whole, the parts before it
        // counted in.
        let told = progress.told.lock().unwrap().clone();
        let expected = [
            "phase staging".to_owned(),
            format!("{block}/{total} data/config.json"),
            format!("{}/{total} data/config.json", block + 1),
            "phase staging".to_owned(),
            "phase staging".to_owned(),
            format!("{total}/{total} shaders/test.spv"),
        ];
        assert_eq!(told, expected);

        // The part's own total is never told, only the job's; a copy tells the same, and a step
        // naming nothing names nothing.
        let alone = Recorded::default();
        let part = RunningTotal {
            job: &alone,
            before: 7,
            total: 20,
        };
        let copy = part;
        part.advance(3, 5, None);
        copy.advance(5, 5, Some(&shader));
        // A part of a part counts the parts before both, and its phases reach the job.
        let inner = RunningTotal {
            job: &part,
            before: 2,
            total: 5,
        };
        inner.advance(1, 3, None);
        inner.phase(PhaseId("publishing"));
        let told = alone.told.lock().unwrap().clone();
        assert_eq!(
            told,
            [
                "10/20 -",
                "12/20 shaders/test.spv",
                "10/20 -",
                "phase publishing"
            ]
        );
    }

    #[test]
    fn a_running_total_passes_holds_through() {
        // A part's holds and their ends reach the job as they were told, in turn with its steps,
        // and so do those of a part of a part.
        let progress = Recorded::default();
        let part = RunningTotal {
            job: &progress,
            before: 4,
            total: 10,
        };
        let inner = RunningTotal {
            job: &part,
            before: 1,
            total: 6,
        };
        part.hold(Hold::GameRunning);
        part.resumed();
        part.advance(2, 3, None);
        inner.hold(Hold::LockBusy);
        inner.resumed();
        inner.advance(1, 2, None);
        let told = progress.told.lock().unwrap().clone();
        assert_eq!(
            told,
            [
                "hold GameRunning",
                "resumed",
                "6/10 -",
                "hold LockBusy",
                "resumed",
                "6/10 -"
            ]
        );

        // Progress that does not follow holds is told them for nothing, its steps unchanged, and
        // so is a part over it.
        let steps = StepsOnly::default();
        let job: &dyn Progress = &steps;
        job.phase(PhaseId("staging"));
        job.hold(Hold::GameRunning);
        job.resumed();
        let over = RunningTotal {
            job,
            before: 1,
            total: 3,
        };
        over.hold(Hold::LockBusy);
        over.resumed();
        over.advance(2, 2, None);
        assert_eq!(*steps.told.lock().unwrap(), ["phase staging", "3/3"]);

        // Progress that goes nowhere is told a hold and its end like everything else.
        NoProgress.hold(Hold::GameRunning);
        NoProgress.resumed();
        let unfollowed = RunningTotal {
            job: &NoProgress,
            before: 0,
            total: 1,
        };
        unfollowed.hold(Hold::LockBusy);
        unfollowed.resumed();
    }

    #[test]
    fn a_hold_names_what_ends_it() {
        assert_eq!(Hold::GameRunning.resumes_when(), "gameClosed");
        assert_eq!(Hold::LockBusy.resumes_when(), "lockFree");
        // Each hold is its own: a copy is the same hold, and the two are told apart.
        let held = Hold::GameRunning;
        let copy = held;
        assert_eq!(copy, held);
        assert_ne!(Hold::GameRunning, Hold::LockBusy);
    }
}
