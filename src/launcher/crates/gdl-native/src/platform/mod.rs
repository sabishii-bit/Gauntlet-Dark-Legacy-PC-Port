//! The crate's direct calls into the system, each behind a safe function. Nothing else in the
//! crate may use `unsafe`.

#[cfg(unix)]
mod unix;
#[cfg(windows)]
mod windows;

#[cfg(unix)]
pub(crate) use unix::effective_uid;
#[cfg(windows)]
pub(crate) use windows::token_kind;
