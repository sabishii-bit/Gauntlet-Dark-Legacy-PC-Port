//! Keeping the system out of idle sleep while a job runs, so that an install or a recovery left
//! alone is not cut short by the machine going to sleep under it.

use std::marker::PhantomData;

#[cfg(windows)]
use windows_sys::Win32::System::Power::{ES_CONTINUOUS, ES_SYSTEM_REQUIRED};

#[cfg(windows)]
use crate::platform;

/// What a hold asks of the system until it is let go: that the system stay out of idle sleep,
/// whatever the display does.
#[cfg(windows)]
const HELD: u32 = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;

/// Keeps the system out of idle sleep while it lives, on the thread that made it.
#[must_use]
pub struct KeepAwake {
    /// The thread's requests that the hold replaced, which its end puts back; 0 when the system
    /// refused the hold, which then has nothing to put back.
    #[cfg(windows)]
    previous: u32,
    /// Keeps the hold on the thread that made it, since the system keeps such requests per thread.
    _thread: PhantomData<*const ()>,
}

/// Holds the system out of idle sleep until the value given back is dropped. A hold made while
/// another is held on the same thread gives the outer one back when it ends, never letting it go.
/// Only Windows is asked; elsewhere the hold asks nothing.
pub fn keep_awake() -> KeepAwake {
    KeepAwake {
        #[cfg(windows)]
        previous: platform::set_execution_state(HELD),
        _thread: PhantomData,
    }
}

#[cfg(windows)]
impl Drop for KeepAwake {
    fn drop(&mut self) {
        if self.previous != 0 {
            platform::set_execution_state(self.previous | ES_CONTINUOUS);
        }
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
        // The inner hold's end gives the outer one back rather than letting the system sleep.
        assert_eq!(execution_state(), HELD);
        drop(outer);
        assert_eq!(execution_state(), before);
        assert_ne!(before & ES_CONTINUOUS, 0, "{before:#x}");
    }

    #[test]
    fn a_hold_stays_on_its_own_thread() {
        // Another thread's hold is that thread's alone, and this one's requests are as they were.
        let before = execution_state();
        let held = Barrier::new(2);
        let release = Barrier::new(2);
        thread::scope(|scope| {
            scope.spawn(|| {
                let _hold = keep_awake();
                assert_eq!(execution_state(), HELD);
                held.wait();
                release.wait();
            });
            held.wait();
            assert_eq!(execution_state(), before);
            release.wait();
        });
        assert_eq!(execution_state(), before);
    }
}
