//! The crate's direct calls into the system, each behind a safe function. Nothing else in the
//! crate may use `unsafe`.

#[cfg(windows)]
mod windows;

#[cfg(all(windows, test))]
pub(crate) use windows::resource_bytes;
#[cfg(windows)]
pub(crate) use windows::{Resource, notify_item_updated, update_resources};
