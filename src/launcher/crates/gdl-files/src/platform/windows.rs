//! The Windows calls: moving a file or a folder with MoveFileExW.

use std::io;
use std::path::Path;

use windows_sys::Win32::Storage::FileSystem::{MOVE_FILE_FLAGS, MoveFileExW};

use crate::long_path::VerbatimPath;

/// Moves `from` to `to` with MoveFileExW and `flags`. Both paths go to the call in their verbatim
/// form, so that neither is held to MAX_PATH.
pub(crate) fn move_file(from: &Path, to: &Path, flags: MOVE_FILE_FLAGS) -> io::Result<()> {
    let from = VerbatimPath::new(from)?;
    let to = VerbatimPath::new(to)?;
    // SAFETY: both pointers are to NUL-terminated UTF-16 strings that `from` and `to` own and keep
    // alive until the call returns, and MoveFileExW only reads them.
    let moved = unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), flags) };
    if moved == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
