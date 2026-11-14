//! The system dialogs the launcher shows when its window cannot open: a task dialog on Windows, and
//! on Linux the first of kdialog, zenity, xmessage or the terminal that is there. Only its
//! `platform` module may call the OS directly.
#![deny(unsafe_code)]
