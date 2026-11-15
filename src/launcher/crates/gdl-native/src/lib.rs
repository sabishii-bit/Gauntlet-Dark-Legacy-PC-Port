//! The system dialogs the launcher shows when its window cannot open, and the process token or
//! user that decides whether the window may start. The dialog is a task dialog on Windows, and on
//! Linux the first of kdialog, zenity, xmessage or the terminal that is there. Only its `platform`
//! module may call the OS directly.
#![deny(unsafe_code)]

#[allow(unsafe_code)]
mod platform;
pub mod privilege;
