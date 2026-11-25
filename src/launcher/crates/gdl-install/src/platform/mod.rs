//! The crate's direct calls into the system, each behind a safe function. Nothing else in the
//! crate may use `unsafe`.

#[cfg(target_os = "linux")]
mod linux;
#[cfg(windows)]
mod windows;

#[cfg(target_os = "linux")]
pub(crate) use linux::{OpenedPid, exec_directly, pidfd_kill, pidfd_open, pidfd_wait};
#[cfg(all(target_os = "linux", test))]
pub(crate) use linux::{exec_directly_marking_each, leak_descriptor};
#[cfg(windows)]
pub(crate) use windows::{
    OpenedProcess, Resource, end_process, fail_critical_errors, internet_settings_values,
    is_same_file, known_folder, long_path, native_machine, notify_item_updated,
    onedrive_account_folders, open_process, own_process_created, process_created, process_image,
    reparse_tag, set_execution_state, update_resources, wait_process, windows_build,
};
#[cfg(all(windows, test))]
pub(crate) use windows::{
    error_mode, execution_state, map_image, refuse_fault_dialogs, resource_bytes, set_short_name,
    short_path,
};
