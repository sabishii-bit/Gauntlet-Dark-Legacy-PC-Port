//! How the launcher writes files: names, staging, publishing by rename, atomic JSON, link checks
//! and retries. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]
