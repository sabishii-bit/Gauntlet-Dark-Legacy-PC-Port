//! The crate's direct calls into the system, each behind a safe function. Nothing else in the
//! crate may use `unsafe`.

#[cfg(target_os = "linux")]
mod linux;
#[cfg(windows)]
mod windows;

#[cfg(target_os = "linux")]
pub(crate) use linux::renameat2_noreplace;
#[cfg(windows)]
pub(crate) use windows::move_file;
