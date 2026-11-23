//! The crate's direct calls into the system, each behind a safe function. Nothing else in the
//! crate may use `unsafe`.

#[cfg(windows)]
mod windows;

#[cfg(windows)]
pub(crate) use windows::{
    Resource, native_machine, notify_item_updated, set_execution_state, update_resources,
};
#[cfg(all(windows, test))]
pub(crate) use windows::{execution_state, resource_bytes};
