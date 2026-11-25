//! The proxy the system's own settings name beside the process's variables: on Windows the user's
//! Internet Settings, read from the registry as Python's urllib reads them, so that the release
//! feed's proxy rules can take them where no proxy variable is set.

use gdl_release::InternetSettings;

#[cfg(windows)]
use crate::platform;

/// The proxy values of the Internet Settings as the registry holds them, each `None` when the key
/// lacks it or holds it in a form that is not read.
#[cfg(any(windows, test))]
#[derive(Clone, Default, PartialEq, Eq)]
pub(crate) struct RawInternetSettings {
    /// `ProxyEnable`, which switches the proxy on.
    pub(crate) enable: Option<ProxyEnable>,
    /// `ProxyServer`, which names the proxy.
    pub(crate) server: Option<String>,
    /// `ProxyOverride`, which lists the hosts reached without it.
    pub(crate) bypass: Option<String>,
}

/// What the Internet Settings' switch holds: the number Windows writes, or text where another
/// program wrote a string.
#[cfg(any(windows, test))]
#[derive(Clone, PartialEq, Eq)]
pub(crate) enum ProxyEnable {
    /// A number value.
    Number(u32),
    /// A string value.
    Text(String),
}

/// The Windows per-user Internet Settings, read from the registry; `None` on other systems, or
/// when the key cannot be read.
pub fn internet_settings() -> Option<InternetSettings> {
    #[cfg(windows)]
    {
        platform::internet_settings_values().ok().map(settings_of)
    }
    #[cfg(not(windows))]
    {
        None
    }
}

/// The Internet Settings that `raw` holds. The proxy is switched on, as urllib takes the switch,
/// by a number other than nought or by text that is not empty, `"0"` among it, and left off when
/// the switch is missing; the server and the bypass list are kept as they are, the rules that read
/// them being the release feed's.
#[cfg(any(windows, test))]
fn settings_of(raw: RawInternetSettings) -> InternetSettings {
    let enabled = match raw.enable {
        Some(ProxyEnable::Number(number)) => number != 0,
        Some(ProxyEnable::Text(text)) => !text.is_empty(),
        None => false,
    };
    InternetSettings {
        enabled,
        server: raw.server,
        bypass: raw.bypass,
    }
}

#[cfg(test)]
mod tests {
    use gdl_release::InternetSettings;

    use super::{ProxyEnable, RawInternetSettings, internet_settings, settings_of};

    /// A proxy's address and a bypass list, as the Internet Settings dialog writes them.
    const SERVER: &str = "proxy.corp:8080";
    const BYPASS: &str = "*.corp;<local>";

    /// Raw settings of `enable` beside a server and a bypass list.
    fn raw(enable: Option<ProxyEnable>) -> RawInternetSettings {
        RawInternetSettings {
            enable,
            server: Some(SERVER.to_owned()),
            bypass: Some(BYPASS.to_owned()),
        }
    }

    /// Whether `enable` switches the proxy on.
    fn switched_on(enable: Option<ProxyEnable>) -> bool {
        settings_of(raw(enable)).enabled
    }

    #[test]
    fn raw_values_become_settings() {
        // A number switches the proxy on unless it is nought, as Windows writes the switch.
        assert!(!switched_on(Some(ProxyEnable::Number(0))));
        assert!(switched_on(Some(ProxyEnable::Number(1))));
        assert!(switched_on(Some(ProxyEnable::Number(u32::MAX))));
        // Any text switches it on, "0" too, and empty text does not, as urllib takes a string.
        assert!(switched_on(Some(ProxyEnable::Text("1".to_owned()))));
        assert!(switched_on(Some(ProxyEnable::Text("0".to_owned()))));
        assert!(!switched_on(Some(ProxyEnable::Text(String::new()))));
        // A missing switch leaves it off.
        assert!(!switched_on(None));

        // The server and the bypass list pass as they are, whether the proxy is on or off.
        let on = InternetSettings {
            enabled: true,
            server: Some(SERVER.to_owned()),
            bypass: Some(BYPASS.to_owned()),
        };
        assert_eq!(settings_of(raw(Some(ProxyEnable::Number(1)))), on);
        let off = InternetSettings {
            enabled: false,
            ..on.clone()
        };
        assert_eq!(settings_of(raw(Some(ProxyEnable::Number(0)))), off);
        // Missing values stay missing.
        assert_eq!(
            settings_of(RawInternetSettings::default()),
            InternetSettings::default()
        );
        let switch_alone = RawInternetSettings {
            enable: Some(ProxyEnable::Number(1)),
            ..RawInternetSettings::default()
        };
        assert_eq!(
            settings_of(switch_alone),
            InternetSettings {
                enabled: true,
                server: None,
                bypass: None,
            }
        );
    }

    #[cfg(windows)]
    #[test]
    fn the_settings_are_read_without_error() {
        use crate::platform::internet_settings_values;

        // The key is only read: it holds the same before and after, whatever that is, and what
        // is read is what it holds. Its values are never shown, since a proxy's address may hold
        // a password.
        let before = internet_settings_values().unwrap();
        let settings = internet_settings();
        let after = internet_settings_values().unwrap();
        assert!(
            before == after,
            "the Internet Settings changed while they were read"
        );
        assert_eq!(settings, Some(settings_of(after)));
    }

    #[cfg(not(windows))]
    #[test]
    fn other_systems_have_no_internet_settings() {
        assert_eq!(internet_settings(), None);
    }
}
