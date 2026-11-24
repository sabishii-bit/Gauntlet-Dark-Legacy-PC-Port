//! The crate's direct calls into the system, each behind a safe function. Nothing else in the
//! crate may use `unsafe`.

#[cfg(target_os = "linux")]
mod linux;
#[cfg(windows)]
mod windows;

#[cfg(target_os = "linux")]
pub(crate) use linux::exec_directly;
#[cfg(all(target_os = "linux", test))]
pub(crate) use linux::{exec_directly_marking_each, leak_descriptor};
#[cfg(windows)]
pub(crate) use windows::{
    Resource, fail_critical_errors, native_machine, notify_item_updated, set_execution_state,
    update_resources,
};
#[cfg(all(windows, test))]
pub(crate) use windows::{error_mode, execution_state, resource_bytes};
