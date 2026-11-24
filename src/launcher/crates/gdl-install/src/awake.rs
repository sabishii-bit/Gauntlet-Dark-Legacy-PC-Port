//! Keeping the system out of idle sleep while a job runs, so that an install or a recovery left
//! alone is not cut short by the machine going to sleep under it.

use std::marker::PhantomData;

#[cfg(windows)]
use std::cell::Cell;

#[cfg(windows)]
use windows_sys::Win32::System::Power::{ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

#[cfg(windows)]
use crate::platform;

/// What a hold asks of the system until it is let go: that the system stay out of idle sleep,
/// whatever the display does.
#[cfg(windows)]
const HELD: u32 = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;

/// A thread's holds.
#[cfg(windows)]
#[derive(Clone, Copy)]
struct Holds {
    /// How many holds the thread has.
    count: usize,
    /// The thread's requests that its first hold replaced, which the end of its last puts back;
    /// 0 when the system refused the first, which then left nothing to put back.
    previous: u32,
}

#[cfg(windows)]
thread_local! {
    /// The holds of this thread, which alone they keep awake: the system keeps such requests per
    /// thread.
    static HOLDS: Cell<Holds> = const {
        Cell::new(Holds {
            count: 0,
            previous: 0,
        })
    };
}

/// Keeps the system out of idle sleep while it lives, on the thread that made it.
#[must_use]
pub struct KeepAwake {
    /// Keeps the hold on the thread that made it, since the system keeps such requests per thread.
    _thread: PhantomData<*const ()>,
}

/// Holds the system out of idle sleep until the value given back is dropped. The system is asked
/// as a thread's first hold begins and given back the thread's earlier requests as its last ends,
/// in whatever order its holds end, so that no hold lets the system sleep while another of the
/// thread's lives. While the thread holds the system, its lasting requests are the hold's alone.
/// Only Windows is asked; elsewhere the hold asks nothing.
pub fn keep_awake() -> KeepAwake {
    #[cfg(windows)]
    HOLDS.with(|holds| {
        let mut held = holds.get();
        if held.count == 0 {
            held.previous = platform::set_execution_state(HELD);
        }
        held.count += 1;
        holds.set(held);
    });
    KeepAwake {
        _thread: PhantomData,
    }
}

#[cfg(windows)]
impl Drop for KeepAwake {
    fn drop(&mut self) {
        HOLDS.with(|holds| {
            let mut held = holds.get();
            let last = held.count == 1;
            held.count = held.count.saturating_sub(1);
            if last && held.previous != 0 {
                platform::set_execution_state(held.previous | ES_CONTINUOUS);
            }
            holds.set(held);
        });
    }
}

#[cfg(all(test, windows))]
mod tests {
    use std::sync::Barrier;
    use std::thread;

    use windows_sys::Win32::System::Power::{ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

    use super::{HELD, keep_awake};
    use crate::platform::execution_state;

    #[test]
    fn a_hold_is_taken_and_given_back_and_nests() {
        let before = execution_state();
        assert_eq!(
            before & ES_SYSTEM_REQUIRED,
            0,
            "the test's thread holds the system already: {before:#x}"
        );
        assert_eq!(HELD, 0x8000_0001);
        let outer = keep_awake();
        assert_eq!(execution_state(), HELD);
        {
            let _inner = keep_awake();
            assert_eq!(execution_state(), HELD);
        }
        // The inner hold's end leaves the outer one standing rather than let the system sleep.
        assert_eq!(execution_state(), HELD);
        drop(outer);
        assert_eq!(execution_state(), before);
        // Two holds ended in the order they began: the first's end leaves the second standing, and
        // the second's gives back what the thread had before either.
        let first = keep_awake();
        let second = keep_awake();
        drop(first);
        assert_eq!(execution_state(), HELD);
        drop(second);
        assert_eq!(execution_state(), before);
        assert_ne!(before & ES_CONTINUOUS, 0, "{before:#x}");
    }

    #[test]
    fn a_hold_stays_on_its_own_thread() {
        // Another thread's hold is that thread's alone, and this one's requests are as they were.
        // Each thread reads its state between the waits and checks it only once both are done
        // waiting, so that a failed check fails the test rather than leave the other waiting.
        let before = execution_state();
        let held = Barrier::new(2);
        let release = Barrier::new(2);
        let (theirs, ours) = thread::scope(|scope| {
            let holder = scope.spawn(|| {
                let hold = keep_awake();
                let theirs = execution_state();
                held.wait();
                release.wait();
                drop(hold);
                theirs
            });
            held.wait();
            let ours = execution_state();
            release.wait();
            (holder.join().unwrap(), ours)
        });
        assert_eq!(theirs, HELD);
        assert_eq!(ours, before);
        assert_eq!(execution_state(), before);
    }
}
