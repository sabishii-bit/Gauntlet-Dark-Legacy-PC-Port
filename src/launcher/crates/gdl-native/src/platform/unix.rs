//! The Unix calls: which user the process acts as, from geteuid.

/// The user ID this process acts as, which decides what it may change.
pub(crate) fn effective_uid() -> u32 {
    // SAFETY: geteuid takes no arguments, always succeeds and only reads the process's
    // credentials.
    unsafe { libc::geteuid() }
}
