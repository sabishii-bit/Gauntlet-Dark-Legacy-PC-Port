//! The Windows calls: moving a file or a folder with MoveFileExW, and the room a volume still has
//! for this process with GetDiskFreeSpaceExW.

use std::io;
use std::path::Path;
use std::ptr;

use windows_sys::Win32::Storage::FileSystem::{GetDiskFreeSpaceExW, MOVE_FILE_FLAGS, MoveFileExW};

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

/// The bytes that this process may still write to the volume that holds the folder `folder`, as
/// GetDiskFreeSpaceExW gives them to its caller: the volume's free bytes, held to what a disk
/// quota leaves this account. The folder goes to the call in its verbatim form, so that it is not
/// held to MAX_PATH.
pub(crate) fn available_to_caller(folder: &Path) -> io::Result<u64> {
    // With a separator at its end, which the call asks of a share's folder and any folder takes.
    let folder = VerbatimPath::new(&folder.join(""))?;
    let mut available = 0_u64;
    // SAFETY: the path is a NUL-terminated UTF-16 string that `folder` owns and keeps alive until
    // the call returns, which only reads it; `available` is a u64 that the call writes through
    // the pointer it is given; and the two figures not asked for are null, which the call allows.
    let measured = unsafe {
        GetDiskFreeSpaceExW(
            folder.as_ptr(),
            &mut available,
            ptr::null_mut(),
            ptr::null_mut(),
        )
    };
    if measured == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(available)
    }
}
