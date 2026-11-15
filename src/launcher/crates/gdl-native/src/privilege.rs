//! Which token or user this process runs as, asked of the system, so that gdl-session's
//! `privilege::refusal` can judge it before the window starts.

use std::io;

use gdl_session::privilege::Privilege;

use crate::platform;

/// The token or user this process runs as: on Windows how its token is elevated, and on a Unix
/// system such as Linux the user it acts as. Only Windows can fail to answer: when the token
/// cannot be opened or read, or holds an elevation type that Windows does not define.
pub fn current_privilege() -> io::Result<Privilege> {
    #[cfg(windows)]
    {
        platform::token_kind().map(Privilege::Windows)
    }
    #[cfg(unix)]
    {
        Ok(Privilege::Unix {
            effective_uid: platform::effective_uid(),
        })
    }
}

#[cfg(test)]
mod tests {
    use gdl_session::privilege::Privilege;

    use super::current_privilege;

    #[test]
    fn the_query_answers_for_this_process() {
        let privilege = current_privilege().expect("the system tells how this process runs");
        // Which token or user runs the tests is not known, so only the kind of answer is checked
        // here; gdl-session tests the refusal against every answer.
        if cfg!(windows) {
            assert!(matches!(privilege, Privilege::Windows(_)), "{privilege:?}");
        } else {
            assert!(matches!(privilege, Privilege::Unix { .. }), "{privilege:?}");
        }
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn the_effective_user_owns_the_process_folder() {
        use std::os::unix::fs::MetadataExt;
        // Linux gives the folder it keeps for a process under /proc to the process's effective
        // user, which is an answer that does not come through geteuid.
        let owner = std::fs::metadata("/proc/self").expect("/proc/self").uid();
        assert_eq!(
            current_privilege().expect("the system tells how this process runs"),
            Privilege::Unix {
                effective_uid: owner
            }
        );
    }
}
