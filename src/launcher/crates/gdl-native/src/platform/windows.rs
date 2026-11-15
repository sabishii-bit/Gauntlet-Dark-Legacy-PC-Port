//! The Windows calls: how the process's token is elevated, from OpenProcessToken and
//! GetTokenInformation.

use std::ffi::c_void;
use std::io;
use std::ptr;

use gdl_session::privilege::TokenKind;
use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
use windows_sys::Win32::Security::{
    self, GetTokenInformation, TOKEN_ELEVATION_TYPE, TOKEN_QUERY, TokenElevationType,
};
use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

/// The length of the one value a token's TokenElevationType class holds.
const ELEVATION_TYPE_LEN: u32 = size_of::<TOKEN_ELEVATION_TYPE>() as u32;

/// How the token this process runs with is elevated.
pub(crate) fn token_kind() -> io::Result<TokenKind> {
    let token = ProcessToken::open()?;
    // 0 is no elevation type, so a value the call did not write is refused, not read as a kind.
    let mut elevation: TOKEN_ELEVATION_TYPE = 0;
    let mut written: u32 = 0;
    // SAFETY: `token` is a handle with query access that stays open until it drops after the
    // call, `elevation` is writable for the ELEVATION_TYPE_LEN bytes passed, all that the
    // TokenElevationType class writes, and `written` is writable for the length the call reports.
    let read = unsafe {
        GetTokenInformation(
            token.0,
            TokenElevationType,
            (&raw mut elevation).cast::<c_void>(),
            ELEVATION_TYPE_LEN,
            &mut written,
        )
    };
    if read == 0 {
        return Err(io::Error::last_os_error());
    }
    kind_of(elevation)
}

/// The kind of token that the elevation type `elevation` names, or InvalidData for a value that
/// Windows does not define.
fn kind_of(elevation: TOKEN_ELEVATION_TYPE) -> io::Result<TokenKind> {
    // By path, because a bare name spelled like these reads as a binding in a pattern, and would
    // become one that matches every value were its import ever lost.
    match elevation {
        Security::TokenElevationTypeDefault => Ok(TokenKind::Default),
        Security::TokenElevationTypeFull => Ok(TokenKind::Full),
        Security::TokenElevationTypeLimited => Ok(TokenKind::Limited),
        other => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "the process's token has elevation type {other}, which Windows does not define"
            ),
        )),
    }
}

/// This process's access token, opened for queries and closed when dropped.
struct ProcessToken(HANDLE);

impl ProcessToken {
    /// Opens the token of this process, the one its threads act with unless they impersonate.
    fn open() -> io::Result<ProcessToken> {
        let mut token: HANDLE = ptr::null_mut();
        // SAFETY: GetCurrentProcess returns a pseudo-handle for this process that needs no
        // closing, and `token` is a writable HANDLE that OpenProcessToken fills when it succeeds.
        let opened = unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) };
        if opened == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(ProcessToken(token))
        }
    }
}

impl Drop for ProcessToken {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful OpenProcessToken, belongs to this value alone
        // and is closed only here.
        unsafe { CloseHandle(self.0) };
    }
}

#[cfg(test)]
mod tests {
    use std::io;

    use gdl_session::privilege::TokenKind;

    use super::kind_of;

    #[test]
    fn each_elevation_type_names_its_kind() {
        // The three values of TOKEN_ELEVATION_TYPE that Windows defines.
        for (elevation, kind) in [
            (1, TokenKind::Default),
            (2, TokenKind::Full),
            (3, TokenKind::Limited),
        ] {
            assert_eq!(kind_of(elevation).unwrap(), kind, "{elevation}");
        }
        for unknown in [0, 4, -1, i32::MIN, i32::MAX] {
            let error = kind_of(unknown).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidData, "{unknown}");
            assert_eq!(
                error.to_string(),
                format!(
                    "the process's token has elevation type {unknown}, which Windows does not define"
                )
            );
        }
    }
}
