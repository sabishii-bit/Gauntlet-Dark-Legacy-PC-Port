//! Whether the launcher may start its window under the process's token or user. Run as
//! administrator, the window would make folders that a normal start cannot later update, and run
//! as root, files that only root may change; so it refuses an elevated token, and root unless the
//! player allows it. The headless modes run under any, because release machines and CI runners
//! often run as administrator.

/// How Windows elevated the token a process runs with, as the token's elevation type records it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TokenKind {
    /// A token with no linked token: a standard user's, and every user's when User Account
    /// Control is off.
    Default,
    /// An administrator's elevated token: the program was run as administrator.
    Full,
    /// An administrator's filtered token: an administrator started the program normally.
    Limited,
}

/// The token or user a process runs as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Privilege {
    /// On Windows, how the process's token is elevated.
    Windows(TokenKind),
    /// On a Unix system such as Linux, the user the process acts as.
    Unix {
        /// The process's effective user ID, which is root's when it is 0.
        effective_uid: u32,
    },
}

/// What the launcher starts as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StartMode {
    /// A headless mode, run from a console or a script.
    Headless,
    /// The window, or the system dialog shown in its place when it cannot open.
    Window,
}

/// Why the window does not start.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// The process runs with an administrator's elevated token.
    Elevated,
    /// The process runs as root, and the player did not allow it.
    Root,
}

/// The command-line option with which a player lets the window start as root.
pub const ALLOW_ROOT: &str = "--allow-root";

/// The effective user ID of root.
const ROOT_UID: u32 = 0;

/// Why the launcher refuses to start in `mode` under `privilege`, or `None` when it may start.
/// Only the window refuses: an elevated token always, and root unless `allow_root` says that the
/// player gave [`ALLOW_ROOT`]. A token of the default kind is allowed, since every user has one on
/// a machine with User Account Control off.
pub fn refusal(privilege: Privilege, mode: StartMode, allow_root: bool) -> Option<Refusal> {
    match mode {
        StartMode::Headless => None,
        StartMode::Window => match privilege {
            Privilege::Windows(TokenKind::Full) => Some(Refusal::Elevated),
            Privilege::Windows(TokenKind::Default | TokenKind::Limited) => None,
            Privilege::Unix { effective_uid } if effective_uid == ROOT_UID && !allow_root => {
                Some(Refusal::Root)
            }
            Privilege::Unix { .. } => None,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::{ALLOW_ROOT, Privilege, Refusal, StartMode, TokenKind, refusal};

    /// Every kind of token Windows gives a process.
    const TOKENS: [TokenKind; 3] = [TokenKind::Default, TokenKind::Full, TokenKind::Limited];

    /// Root's effective user ID and others': the first user a Linux system adds, the user nobody,
    /// and the largest value an ID holds.
    const USERS: [u32; 4] = [0, 1000, 65534, u32::MAX];

    /// A Unix process acting as `effective_uid`.
    fn user(effective_uid: u32) -> Privilege {
        Privilege::Unix { effective_uid }
    }

    #[test]
    fn refusals_follow_the_token_and_the_mode() {
        use StartMode::{Headless, Window};
        let elevated = Privilege::Windows(TokenKind::Full);
        let limited = Privilege::Windows(TokenKind::Limited);
        let default = Privilege::Windows(TokenKind::Default);
        for (what, privilege, mode, allow_root, expected) in [
            (
                "run as administrator",
                elevated,
                Window,
                false,
                Some(Refusal::Elevated),
            ),
            (
                "run as administrator with the root option",
                elevated,
                Window,
                true,
                Some(Refusal::Elevated),
            ),
            (
                "a check run as administrator",
                elevated,
                Headless,
                false,
                None,
            ),
            (
                "an administrator's normal start",
                limited,
                Window,
                false,
                None,
            ),
            ("User Account Control off", default, Window, false, None),
            ("root", user(0), Window, false, Some(Refusal::Root)),
            ("root with the root option", user(0), Window, true, None),
            ("a check run as root", user(0), Headless, false, None),
            ("a user", user(1000), Window, false, None),
        ] {
            assert_eq!(refusal(privilege, mode, allow_root), expected, "{what}");
        }
    }

    #[test]
    fn headless_modes_run_under_any_token_or_user() {
        let privileges = TOKENS
            .map(Privilege::Windows)
            .into_iter()
            .chain(USERS.map(user));
        for privilege in privileges {
            for allow_root in [false, true] {
                assert_eq!(
                    refusal(privilege, StartMode::Headless, allow_root),
                    None,
                    "{privilege:?}, root option {allow_root}"
                );
            }
        }
    }

    #[test]
    fn the_root_option_allows_only_root() {
        // Players type the option, so its spelling must not change.
        assert_eq!(ALLOW_ROOT, "--allow-root");
        // It is a Linux option: it never lets an elevated token start the window.
        for kind in TOKENS {
            let privilege = Privilege::Windows(kind);
            assert_eq!(
                refusal(privilege, StartMode::Window, true),
                refusal(privilege, StartMode::Window, false),
                "{kind:?}"
            );
        }
        // Root alone is refused, and only without the option.
        for uid in USERS {
            let refused = (uid == 0).then_some(Refusal::Root);
            assert_eq!(
                refusal(user(uid), StartMode::Window, false),
                refused,
                "{uid}"
            );
            assert_eq!(refusal(user(uid), StartMode::Window, true), None, "{uid}");
        }
    }
}
