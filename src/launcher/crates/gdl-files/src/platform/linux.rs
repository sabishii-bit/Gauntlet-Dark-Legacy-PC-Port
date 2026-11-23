//! The Linux calls: renaming without replacing with renameat2, and what a folder's file system
//! allows with statvfs.

use std::ffi::CString;
use std::io;
use std::mem::MaybeUninit;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

use crate::Volume;

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

/// What the file system that holds `folder` allows, as statvfs tells it: the blocks free to users
/// without privilege, in bytes, and whether it is mounted noexec. A relative path is taken from
/// the working folder.
pub(crate) fn file_system_of(folder: &Path) -> io::Result<Volume> {
    let folder = c_path(folder)?;
    let mut stat = MaybeUninit::<libc::statvfs>::uninit();
    // SAFETY: the path is a NUL-terminated string that `folder` owns and keeps alive until the
    // call returns, which only reads it, and `stat` is room for one statvfs, which the call fills
    // when it succeeds.
    let measured = unsafe { libc::statvfs(folder.as_ptr(), stat.as_mut_ptr()) };
    if measured != 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: statvfs succeeded, and so filled `stat` whole.
    let stat = unsafe { stat.assume_init() };
    Ok(Volume {
        available: stat.f_bavail.saturating_mul(stat.f_frsize),
        noexec: stat.f_flag & libc::ST_NOEXEC != 0,
    })
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
