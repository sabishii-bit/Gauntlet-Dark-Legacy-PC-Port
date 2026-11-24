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
    Resource, fail_critical_errors, known_folder, native_machine, notify_item_updated,
    onedrive_account_folders, reparse_tag, set_execution_state, update_resources, windows_build,
};
#[cfg(all(windows, test))]
pub(crate) use windows::{error_mode, execution_state, refuse_fault_dialogs, resource_bytes};
