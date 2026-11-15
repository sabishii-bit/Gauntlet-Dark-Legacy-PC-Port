//! The Linux calls: renaming without replacing with renameat2.

use std::ffi::CString;
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

/// Renames `from` to `to` with renameat2 and RENAME_NOREPLACE, which the kernel refuses with
/// EEXIST when anything is at `to`. A relative path is taken from the working folder.
pub(crate) fn renameat2_noreplace(from: &Path, to: &Path) -> io::Result<()> {
    let from = c_path(from)?;
    let to = c_path(to)?;
    // SAFETY: both pointers are to NUL-terminated strings that `from` and `to` own and keep alive
    // until the call returns, renameat2 only reads them, and AT_FDCWD stands for no descriptor.
    let renamed = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            from.as_ptr(),
            libc::AT_FDCWD,
            to.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    if renamed == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

/// `path` as the string a system call takes, refused when it holds a NUL, which would end it early.
fn c_path(path: &Path) -> io::Result<CString> {
    CString::new(path.as_os_str().as_bytes()).map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "a path given to Linux holds a NUL",
        )
    })
}
