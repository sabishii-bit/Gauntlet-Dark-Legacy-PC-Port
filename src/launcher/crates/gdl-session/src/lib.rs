//! The launcher window's logic without the window: its modes, the plans it shows, the jobs it runs
//! and their events, where an update comes from, the error codes the page turns into words, the
//! headless modes, the extraction of a Legends expansion's tree into a new folder, and whether the
//! window may start under the process's token or user.
#![forbid(unsafe_code)]

pub mod codes;
pub mod extraction;
pub mod headless;
pub mod privilege;
pub mod updating;
