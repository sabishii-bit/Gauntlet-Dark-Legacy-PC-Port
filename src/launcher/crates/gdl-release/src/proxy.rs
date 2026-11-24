//! Which proxy a request of the release feed or a download goes through, chosen as Python's
//! urllib, which the alpha installer reached GitHub through, chooses one: from the process's
//! `<scheme>_proxy` variables and their `no_proxy`, or, where no such variable is set, from the
//! Windows per-user Internet Settings and their bypass list. A network the installer reached is
//! then reached the same way. The choice is made here for every request, and the transport is
//! handed the one proxy chosen, or none.

use std::borrow::Cow;

use ureq::http::Uri;

use crate::ReleaseError;

/// What a proxy variable's name ends in, after the protocol it serves.
const PROXY_SUFFIX: &str = "_proxy";

/// The variable that tells a program it runs as a web server's CGI script, whose `HTTP_PROXY`
/// a client's `Proxy:` header can set, so that its upper-case form is not trusted.
const CGI_VARIABLE: &str = "REQUEST_METHOD";

/// The entry the `no_proxy` variable makes: the hosts that go direct.
const NO_PROXY: &str = "no";

/// The `no_proxy` value that sends every host direct.
const EVERY_HOST: &str = "*";

/// The bypass entry of the Internet Settings that sends every host without a dot direct.
const LOCAL_HOSTS: &str = "<local>";

/// The protocol and scheme of plain HTTP, and of the proxies the transport speaks to.
const HTTP: &str = "http";

/// The protocol and scheme of HTTPS, which every request the launcher makes to GitHub uses.
const HTTPS: &str = "https";

/// The protocols one Internet Settings server without `=` serves, and whose addresses without a
/// scheme are HTTP proxies.
const SERVER_PROTOCOLS: [&str; 3] = [HTTP, HTTPS, "ftp"];

/// The Internet Settings' protocol of a SOCKS proxy.
const SOCKS: &str = "socks";

/// The scheme an Internet Settings SOCKS address without one is given.
const SOCKS_SCHEME_PREFIX: &str = "socks://";

/// The scheme a SOCKS address is read as where it stands in for HTTP and HTTPS.
const SOCKS4_SCHEME_PREFIX: &str = "socks4://";

/// The schemes of a SOCKS proxy, which the transport cannot speak.
const SOCKS_SCHEMES: [&str; 5] = ["socks", "socks4", "socks4a", "socks5", "socks5h"];

/// What separates a scheme from the rest of a URL.
const SCHEME_SEPARATOR: &str = "://";

/// The characters that end a URL's authority.
const AUTHORITY_ENDS: [char; 3] = ['/', '?', '#'];

/// Why a SOCKS proxy is refused.
const SOCKS_REFUSED: &str = "SOCKS proxies are not supported";

/// Why a proxy address that no proxy URL can be made of is refused.
const UNREADABLE_ADDRESS: &str = "the proxy's address cannot be read";

/// The Windows per-user Internet Settings, as they stand in the registry.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct InternetSettings {
    /// Whether the proxy is switched on (`ProxyEnable`).
    pub enabled: bool,
    /// The proxy (`ProxyServer`): one `host:port` for every protocol, or `protocol=address`
    /// parts separated by `;`.
    pub server: Option<String>,
    /// The hosts that go direct (`ProxyOverride`): shell patterns separated by `;`, and
    /// `<local>` for every host whose name has no dot.
    pub bypass: Option<String>,
}

/// The system's proxy settings, taken once: the process's environment and, on Windows, the
/// Internet Settings.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProxySettings {
    /// The variables a proxy is chosen by, in the order the system lists them; the others are
    /// not kept.
    environment: Vec<(String, String)>,
    internet: Option<InternetSettings>,
    case_blind_names: bool,
}

impl ProxySettings {
    /// Settings of the given variables, in the order the system lists them, and Internet Settings;
    /// `case_blind_names` as on Windows, whose variable names ignore letter case.
    pub fn new(
        mut environment: Vec<(String, String)>,
        internet: Option<InternetSettings>,
        case_blind_names: bool,
    ) -> ProxySettings {
        environment.retain(|(name, _)| {
            let name = seen_name(name, case_blind_names);
            proxy_protocol(&name).is_some() || name == CGI_VARIABLE
        });
        ProxySettings {
            environment,
            internet,
            case_blind_names,
        }
    }

    /// This process's variables (names or values that are not Unicode are passed over) and the
    /// Internet Settings the caller read.
    pub fn of_process(internet: Option<InternetSettings>) -> ProxySettings {
        let environment = std::env::vars_os()
            .filter_map(|(name, value)| Some((name.into_string().ok()?, value.into_string().ok()?)))
            .collect();
        ProxySettings::new(environment, internet, cfg!(windows))
    }

    /// No proxy at all, as a loopback feed wants.
    pub fn none() -> ProxySettings {
        ProxySettings::default()
    }

    /// The proxy a request to `url` goes through, or `None` to go direct.
    pub fn proxy_for(&self, url: &Uri) -> Result<Option<ureq::Proxy>, ReleaseError> {
        self.choose(url)?
            .as_deref()
            .map(transport_proxy)
            .transpose()
    }

    /// What `proxy_for` decides, as text, for the vectors: the proxy's URL or `None`.
    #[cfg(test)]
    fn choice_for(&self, url: &str) -> Result<Option<String>, ReleaseError> {
        let url: Uri = url.parse().expect("a test URL parses");
        let choice = self.choose(&url)?;
        if let Some(proxy) = &choice {
            transport_proxy(proxy)?;
        }
        Ok(choice)
    }

    /// The URL of the proxy a request to `url` goes through, as the transport is to reach it.
    fn choose(&self, url: &Uri) -> Result<Option<String>, ReleaseError> {
        let scheme = url.scheme_str().unwrap_or_default().to_ascii_lowercase();
        // The host is the URL's authority as written, its port included, as urllib's request
        // gives it to the bypass rules.
        let host = url.authority().map_or("", |authority| authority.as_str());
        let environment = self.environment_entries();
        let address = match &self.internet {
            // Any entry at all, even one of another protocol or `no_proxy` alone, makes the
            // environment's the settings, and the Internet Settings are not read.
            Some(internet) if environment.is_empty() => {
                let entries = internet_entries(internet);
                let Some(address) = entries.serving(&scheme) else {
                    return Ok(None);
                };
                if !host.is_empty() && bypassed_by_internet(internet, host) {
                    return Ok(None);
                }
                proxy_url(address)?
            }
            _ => {
                let Some(address) = environment.serving(&scheme) else {
                    return Ok(None);
                };
                if !host.is_empty() && bypassed_by_environment(&environment, host) {
                    return Ok(None);
                }
                proxy_url(address)?
            }
        };
        Ok(Some(address))
    }

    /// The proxies the environment names, by protocol: every variable whose name ends in
    /// `_proxy` in any case and whose value is not empty; then, in a CGI script, no HTTP proxy;
    /// then those whose name ends in it in lower case again, so that a lower-case variable wins
    /// over its upper-case twin and an empty one takes the entry away.
    fn environment_entries(&self) -> Entries {
        let variables = self.variables();
        let mut entries = Entries::default();
        let mut proxies = Vec::new();
        for (name, value) in &variables {
            if let Some(protocol) = proxy_protocol(name) {
                if !value.is_empty() {
                    entries.set(&protocol, value);
                }
                proxies.push((name, *value, protocol));
            }
        }
        if variables.iter().any(|(name, _)| name == CGI_VARIABLE) {
            entries.remove(HTTP);
        }
        for (name, value, protocol) in proxies {
            if name.ends_with(PROXY_SUFFIX) {
                if value.is_empty() {
                    entries.remove(&protocol);
                } else {
                    entries.set(&protocol, value);
                }
            }
        }
        entries
    }

    /// The variables under the names the proxy rules see: as given, or, where names ignore case,
    /// in upper case as Python's environment on Windows holds them, a name given twice once.
    fn variables(&self) -> Vec<(Cow<'_, str>, &str)> {
        let mut variables: Vec<(Cow<'_, str>, &str)> = Vec::with_capacity(self.environment.len());
        for (name, value) in &self.environment {
            let name = seen_name(name, self.case_blind_names);
            match variables.iter_mut().find(|(seen, _)| *seen == name) {
                Some(variable) => variable.1 = value,
                None => variables.push((name, value)),
            }
        }
        variables
    }
}

/// `name` as the proxy rules see it: in upper case where names ignore case.
fn seen_name(name: &str, case_blind_names: bool) -> Cow<'_, str> {
    if case_blind_names {
        Cow::Owned(name.to_uppercase())
    } else {
        Cow::Borrowed(name)
    }
}

/// The protocol a proxy variable named `name` serves, in lower case: what comes before its
/// `_proxy`, which may be in any case. `None` for a variable of another name.
fn proxy_protocol(name: &str) -> Option<String> {
    let start = name.len().checked_sub(PROXY_SUFFIX.len())?;
    let suffix = name.as_bytes().get(start..)?;
    // The suffix is ASCII when it matches, so `start` is a character boundary.
    suffix
        .eq_ignore_ascii_case(PROXY_SUFFIX.as_bytes())
        .then(|| name[..start].to_lowercase())
}

/// The proxies of a setting by the protocol each serves, in the order they were first set, as a
/// Python dictionary keeps them.
#[derive(Default)]
struct Entries(Vec<(String, String)>);

impl Entries {
    fn set(&mut self, protocol: &str, address: &str) {
        match self.0.iter_mut().find(|(entry, _)| entry == protocol) {
            Some(entry) => address.clone_into(&mut entry.1),
            None => self.0.push((protocol.to_owned(), address.to_owned())),
        }
    }

    fn remove(&mut self, protocol: &str) {
        self.0.retain(|(entry, _)| entry != protocol);
    }

    fn get(&self, protocol: &str) -> Option<&str> {
        self.0
            .iter()
            .find(|(entry, _)| entry == protocol)
            .map(|(_, address)| address.as_str())
    }

    fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// The proxy that serves a request of `scheme`: the entry whose protocol is the scheme in
    /// any case, the last of them where several are, as urllib's handler takes each entry in
    /// turn under its protocol's lower-case name.
    fn serving(&self, scheme: &str) -> Option<&str> {
        self.0
            .iter()
            .rev()
            .find(|(protocol, _)| protocol.to_lowercase() == scheme)
            .map(|(_, address)| address.as_str())
    }
}

/// The proxies the Internet Settings name, by protocol. One server without `=` or `;` serves
/// HTTP, HTTPS and FTP; otherwise the `protocol=address` parts are read in turn until one has no
/// `=`, which ends the reading with the entries before it kept. Once every part is read, a SOCKS
/// proxy stands in for HTTP and HTTPS where they have none.
fn internet_entries(settings: &InternetSettings) -> Entries {
    let mut entries = Entries::default();
    let Some(server) = settings.server.as_deref().filter(|_| settings.enabled) else {
        return entries;
    };
    let server = if server.contains(['=', ';']) {
        Cow::Borrowed(server)
    } else {
        Cow::Owned(
            SERVER_PROTOCOLS
                .map(|protocol| format!("{protocol}={server}"))
                .join(";"),
        )
    };
    for part in server.split(';') {
        let Some((protocol, address)) = part.split_once('=') else {
            return entries;
        };
        let address = if has_scheme(address) {
            Cow::Borrowed(address)
        } else if SERVER_PROTOCOLS.contains(&protocol) {
            Cow::Owned(format!("{HTTP}{SCHEME_SEPARATOR}{address}"))
        } else if protocol == SOCKS {
            Cow::Owned(format!("{SOCKS_SCHEME_PREFIX}{address}"))
        } else {
            Cow::Borrowed(address)
        };
        entries.set(protocol, &address);
    }
    if let Some(socks) = entries.get(SOCKS).filter(|address| !address.is_empty()) {
        let socks = match socks.strip_prefix(SOCKS_SCHEME_PREFIX) {
            Some(rest) => format!("{SOCKS4_SCHEME_PREFIX}{rest}"),
            None => socks.to_owned(),
        };
        for protocol in [HTTP, HTTPS] {
            if entries.get(protocol).is_none_or(str::is_empty) {
                entries.set(protocol, &socks);
            }
        }
    }
    entries
}

/// Whether `address` starts with a scheme and `://`.
fn has_scheme(address: &str) -> bool {
    match address.find(['/', ':']) {
        Some(end) if end > 0 => address[end..].starts_with(SCHEME_SEPARATOR),
        _ => false,
    }
}

/// Whether `no_proxy` sends `host` direct: `*` sends every host; otherwise each entry between
/// commas, trimmed, its leading dots dropped, sends a host that is it or ends in `.` and it,
/// with or without the host's port, letter case ignored.
fn bypassed_by_environment(entries: &Entries, host: &str) -> bool {
    let Some(no_proxy) = entries.get(NO_PROXY) else {
        return false;
    };
    if no_proxy == EVERY_HOST {
        return true;
    }
    let host = host.to_lowercase();
    let (host_only, _) = split_port(&host);
    for name in no_proxy.split(',') {
        let name = name.trim();
        if name.is_empty() {
            continue;
        }
        let name = name.trim_start_matches('.').to_lowercase();
        if host_only == name || host == name {
            return true;
        }
        let subdomain = format!(".{name}");
        if host_only.ends_with(&subdomain) || host.ends_with(&subdomain) {
            return true;
        }
    }
    false
}

/// Whether the Internet Settings' bypass list sends `host` direct: with the proxy switched on,
/// each entry between semicolons, trimmed, is `<local>`, which sends a host whose name has no
/// dot, or a shell pattern the host's name, its port left off, matches as Windows matches one.
fn bypassed_by_internet(settings: &InternetSettings, host: &str) -> bool {
    let Some(bypass) = settings
        .bypass
        .as_deref()
        .filter(|bypass| settings.enabled && !bypass.is_empty())
    else {
        return false;
    };
    let (host, _) = split_port(host);
    bypass.split(';').map(str::trim).any(|entry| {
        if entry == LOCAL_HOSTS {
            !host.contains('.')
        } else {
            shell_pattern_matches(entry, host)
        }
    })
}

/// `host` and its port, split at its last colon when only digits follow it; a colon with
/// nothing after it is dropped.
fn split_port(host: &str) -> (&str, Option<&str>) {
    match host.rsplit_once(':') {
        Some((name, port)) if port.bytes().all(|byte| byte.is_ascii_digit()) => {
            (name, Some(port).filter(|port| !port.is_empty()))
        }
        _ => (host, None),
    }
}

/// The URL the transport reaches the proxy `address` at: a proxy URL, or an address without a
/// scheme, which is an HTTP proxy. An HTTPS proxy is reached as urllib reaches one for an HTTPS
/// request, by a plain `CONNECT`, so it is an HTTP proxy at the same address; a SOCKS proxy is
/// refused.
fn proxy_url(address: &str) -> Result<String, ReleaseError> {
    let (scheme, authority) = split_proxy(address)?;
    // A URL's authority would end at any of these, so the transport would read another host
    // out of the proxy's URL than urllib reads out of the address.
    if authority.contains(AUTHORITY_ENDS) {
        return Err(refused(UNREADABLE_ADDRESS));
    }
    match scheme.as_deref() {
        None | Some(HTTP | HTTPS) => Ok(format!("{HTTP}{SCHEME_SEPARATOR}{authority}")),
        Some(scheme) if SOCKS_SCHEMES.contains(&scheme) => Err(refused(SOCKS_REFUSED)),
        Some(_) => Err(refused("only HTTP and HTTPS proxies are supported")),
    }
}

/// A proxy address's scheme, in lower case, and its authority (its user information, host and
/// port), as urllib splits one: `scheme://authority/...`, or the whole address as the
/// authority when no `/` follows its first colon.
fn split_proxy(address: &str) -> Result<(Option<String>, &str), ReleaseError> {
    let (scheme, rest) = match address.find(['/', ':']) {
        Some(end) if end > 0 && address[end..].starts_with(':') => {
            (Some(address[..end].to_lowercase()), &address[end + 1..])
        }
        _ => (None, address),
    };
    if !rest.starts_with('/') {
        return Ok((None, address));
    }
    if !rest.starts_with("//") {
        return Err(refused("the proxy's URL names no host"));
    }
    // The authority ends at the first `/` after the user information, or after the `//`.
    let end = match rest.find('@') {
        Some(at) => rest[at..].find('/').map(|slash| at + slash),
        None => rest[2..].find('/').map(|slash| 2 + slash),
    };
    Ok((scheme, &rest[2..end.unwrap_or(rest.len())]))
}

/// The transport's proxy at `url`, credentials in it decoded.
fn transport_proxy(url: &str) -> Result<ureq::Proxy, ReleaseError> {
    // The URL is not repeated in the failure, since it may hold a password.
    ureq::Proxy::new(url).map_err(|_| refused(UNREADABLE_ADDRESS))
}

/// The refusal of a proxy setting the transport cannot use, for `detail`.
fn refused(detail: &str) -> ReleaseError {
    ReleaseError::Proxy {
        detail: detail.into(),
    }
}

/// One step of a shell pattern.
enum PatternStep {
    /// This character.
    Char(char),
    /// Any one character.
    AnyChar,
    /// Any run of characters, none included.
    AnyRun,
    /// One character of the ranges, each from its first character to its last, or, `negated`,
    /// one of none of them.
    Set {
        negated: bool,
        ranges: Vec<(char, char)>,
    },
}

impl PatternStep {
    /// Whether the step takes `character` as the one character it stands for.
    fn takes(&self, character: char) -> bool {
        match self {
            PatternStep::Char(own) => *own == character,
            PatternStep::AnyChar => true,
            PatternStep::AnyRun => false,
            PatternStep::Set { negated, ranges } => {
                *negated
                    != ranges
                        .iter()
                        .any(|&(first, last)| first <= character && character <= last)
            }
        }
    }
}

/// Whether `name` matches the shell pattern `pattern` (`*`, `?`, `[...]` and `[!...]`), as
/// Windows matches a name against one: letter case ignored, and slashes taken as backslashes.
fn shell_pattern_matches(pattern: &str, name: &str) -> bool {
    let fold = |text: &str| -> Vec<char> {
        text.to_ascii_lowercase()
            .chars()
            .map(|character| if character == '/' { '\\' } else { character })
            .collect()
    };
    let steps = pattern_steps(&fold(pattern));
    let name = fold(name);
    // Each step but a run takes one character; on a mismatch, the last run takes one more.
    let (mut step, mut at) = (0, 0);
    let mut last_run = None;
    while at < name.len() {
        match steps.get(step) {
            Some(PatternStep::AnyRun) => {
                step += 1;
                last_run = Some((step, at));
            }
            Some(next) if next.takes(name[at]) => {
                step += 1;
                at += 1;
            }
            _ => match last_run {
                Some((after, from)) => {
                    step = after;
                    at = from + 1;
                    last_run = Some((after, at));
                }
                None => return false,
            },
        }
    }
    steps[step..]
        .iter()
        .all(|rest| matches!(rest, PatternStep::AnyRun))
}

/// The steps of a shell pattern. A `[` that no `]` closes stands for itself; a `]` straight
/// after the `[` or `[!` belongs to the set; in a set, `a-z` is a range, a range whose first
/// character comes after its last takes none, and a `-` first or last stands for itself.
fn pattern_steps(pattern: &[char]) -> Vec<PatternStep> {
    let mut steps = Vec::new();
    let mut at = 0;
    while let Some(&character) = pattern.get(at) {
        at += 1;
        match character {
            '*' => {
                if !matches!(steps.last(), Some(PatternStep::AnyRun)) {
                    steps.push(PatternStep::AnyRun);
                }
            }
            '?' => steps.push(PatternStep::AnyChar),
            '[' => match set_at(pattern, at) {
                Some((set, next)) => {
                    steps.push(set);
                    at = next;
                }
                None => steps.push(PatternStep::Char('[')),
            },
            _ => steps.push(PatternStep::Char(character)),
        }
    }
    steps
}

/// The set whose body starts at `start`, just after its `[`, and where the pattern goes on after
/// its `]`; `None` when no `]` closes it.
fn set_at(pattern: &[char], start: usize) -> Option<(PatternStep, usize)> {
    let mut end = start;
    if pattern.get(end) == Some(&'!') {
        end += 1;
    }
    if pattern.get(end) == Some(&']') {
        end += 1;
    }
    end += pattern
        .get(end..)?
        .iter()
        .position(|&character| character == ']')?;
    let (negated, body) = match &pattern[start..end] {
        ['!', body @ ..] => (true, body),
        body => (false, body),
    };
    let mut ranges = Vec::new();
    let mut at = 0;
    while let Some(&first) = body.get(at) {
        match body.get(at + 1..at + 3) {
            Some(&['-', last]) => {
                ranges.push((first, last));
                at += 3;
            }
            _ => {
                ranges.push((first, first));
                at += 1;
            }
        }
    }
    Some((PatternStep::Set { negated, ranges }, end + 1))
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;

    use serde::Deserialize;

    use super::{
        InternetSettings, ProxySettings, SOCKS_REFUSED, UNREADABLE_ADDRESS, shell_pattern_matches,
    };
    use crate::ReleaseError;

    /// The proxy cases the release scripts' tests hold to Python's own rules.
    const VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/launcher/proxy-vectors.json"
    ));

    /// The shared file. An unknown field is refused, so none goes untested here.
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Vectors {
        schema: u32,
        environment: Vec<EnvironmentCase>,
        registry: Vec<RegistryCase>,
    }

    /// A case of the environment's variables alone.
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct EnvironmentCase {
        name: String,
        system: Option<String>,
        env: Vec<(String, String)>,
        url: String,
        proxy: Option<String>,
        refused: Option<String>,
        urllib: Option<String>,
    }

    /// A case of the Internet Settings, and of variables beside them.
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct RegistryCase {
        name: String,
        system: Option<String>,
        #[serde(default)]
        env: Vec<(String, String)>,
        enabled: bool,
        server: Option<String>,
        bypass: Option<String>,
        url: String,
        proxy: Option<String>,
        refused: Option<String>,
        urllib: Option<String>,
    }

    /// What a case expects the launcher to choose: the proxy's URL, none, or the refusal of a
    /// SOCKS proxy.
    type Answer = Result<Option<String>, &'static str>;

    /// The proxy a vector's settings choose for `url`, as a case gives it.
    fn answer(settings: &ProxySettings, url: &str) -> Answer {
        match settings.choice_for(url) {
            Ok(choice) => Ok(choice),
            Err(ReleaseError::Proxy { detail }) if &*detail == SOCKS_REFUSED => Err("socks"),
            Err(error) => panic!("{url}: {error}"),
        }
    }

    /// The answer a case expects, held to the fields' rules: a refusal replaces the proxy, and
    /// urllib's own answer, where given, is not the launcher's.
    fn expected(
        name: &str,
        proxy: &Option<String>,
        refused: &Option<String>,
        urllib: &Option<String>,
    ) -> Answer {
        let answer = match refused.as_deref() {
            Some("socks") => {
                assert_eq!(*proxy, None, "{name}: a refused case names no proxy");
                Err("socks")
            }
            Some(other) => panic!("{name}: no refusal is named {other:?}"),
            None => Ok(proxy.clone()),
        };
        if let Some(urllib) = urllib {
            assert_ne!(answer, Ok(Some(urllib.clone())), "{name}: urllib agrees");
        }
        answer
    }

    /// Whether a case's names are taken as given, as upper-case names that ignore case, or both
    /// ways alike: as on the one system the case names, or as on either.
    fn name_rules(name: &str, system: &Option<String>) -> &'static [bool] {
        match system.as_deref() {
            None => &[false, true],
            Some("linux") => &[false],
            Some("windows") => &[true],
            Some(other) => panic!("{name}: no system is named {other:?}"),
        }
    }

    fn variables(pairs: &[(&str, &str)]) -> Vec<(String, String)> {
        pairs
            .iter()
            .map(|&(name, value)| (name.to_owned(), value.to_owned()))
            .collect()
    }

    /// Settings of the variables alone, names taken as given.
    fn environment(pairs: &[(&str, &str)]) -> ProxySettings {
        ProxySettings::new(variables(pairs), None, false)
    }

    /// Settings of a switched-on Internet Settings server and bypass list alone.
    fn internet(server: &str, bypass: Option<&str>) -> ProxySettings {
        let settings = InternetSettings {
            enabled: true,
            server: Some(server.to_owned()),
            bypass: bypass.map(str::to_owned),
        };
        ProxySettings::new(Vec::new(), Some(settings), true)
    }

    fn proxy_of(settings: &ProxySettings, url: &str) -> ureq::Proxy {
        let url = url.parse().expect("the URL parses");
        match settings.proxy_for(&url) {
            Ok(Some(proxy)) => proxy,
            Ok(None) => panic!("{url} goes direct"),
            Err(error) => panic!("{url}: {error}"),
        }
    }

    #[test]
    fn cases_follow_the_shared_vectors() {
        let vectors: Vectors = serde_json::from_str(VECTORS).expect("the shared vectors parse");
        assert_eq!(vectors.schema, 1);
        assert!(!vectors.environment.is_empty() && !vectors.registry.is_empty());
        let mut names = BTreeSet::new();

        for case in &vectors.environment {
            assert!(names.insert(case.name.as_str()), "{} twice", case.name);
            let expected = expected(&case.name, &case.proxy, &case.refused, &case.urllib);
            for &case_blind_names in name_rules(&case.name, &case.system) {
                let settings = ProxySettings::new(case.env.clone(), None, case_blind_names);
                assert_eq!(
                    answer(&settings, &case.url),
                    expected,
                    "{} (names ignore case: {case_blind_names})",
                    case.name
                );
            }
        }

        for case in &vectors.registry {
            assert!(names.insert(case.name.as_str()), "{} twice", case.name);
            let expected = expected(&case.name, &case.proxy, &case.refused, &case.urllib);
            let internet = InternetSettings {
                enabled: case.enabled,
                server: case.server.clone(),
                bypass: case.bypass.clone(),
            };
            // The registry is read on Windows alone, but its rules are values on every system.
            for &case_blind_names in name_rules(&case.name, &case.system) {
                let settings =
                    ProxySettings::new(case.env.clone(), Some(internet.clone()), case_blind_names);
                assert_eq!(
                    answer(&settings, &case.url),
                    expected,
                    "{} (names ignore case: {case_blind_names})",
                    case.name
                );
            }
        }
    }

    #[test]
    fn a_socks_proxy_is_refused() {
        let url = "https://api.github.com/repos"
            .parse()
            .expect("the URL parses");
        for scheme in ["socks", "socks4", "socks4a", "socks5", "socks5h", "SOCKS5"] {
            let settings = environment(&[("https_proxy", &format!("{scheme}://s:1080"))]);
            match settings.proxy_for(&url) {
                Err(error @ ReleaseError::Proxy { .. }) => {
                    assert_eq!(error.rule(), "proxy");
                    assert_eq!(
                        error.to_string(),
                        format!("The system's proxy setting cannot be used: {SOCKS_REFUSED}")
                    );
                }
                other => panic!("{scheme}: {other:?}"),
            }
        }
        // The Internet Settings' SOCKS proxy stands in for HTTPS, and is refused there too.
        assert_eq!(
            answer(&internet("socks=s:1080", None), "https://api.github.com/"),
            Err("socks")
        );
        // A host that goes direct never meets the proxy it bypasses.
        let bypassed = environment(&[("https_proxy", "socks5://s:1080"), ("no_proxy", "*")]);
        assert_eq!(answer(&bypassed, "https://api.github.com/repos"), Ok(None));
    }

    #[test]
    fn a_loopback_feed_goes_direct() {
        let none = ProxySettings::none();
        assert_eq!(none, ProxySettings::default());
        for url in [
            "http://127.0.0.1:8080/repos/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases",
            "http://[::1]:8080/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/v1.0.0-beta/x",
            "https://api.github.com/repos",
        ] {
            assert_eq!(none.choice_for(url).expect("no proxy is no failure"), None);
            let parsed = url.parse().expect("the URL parses");
            assert!(none.proxy_for(&parsed).expect("no failure").is_none());
        }
    }

    #[test]
    fn a_proxy_with_credentials_keeps_them() {
        let settings = environment(&[("https_proxy", "http://user:pa%40ss@proxy:8080")]);
        let proxy = proxy_of(&settings, "https://api.github.com/repos");
        assert_eq!(proxy.protocol(), ureq::ProxyProtocol::Http);
        assert_eq!(proxy.username(), Some("user"));
        assert_eq!(proxy.password(), Some("pa@ss"));
        assert_eq!(proxy.host(), "proxy");
        assert_eq!(proxy.port(), 8080);
    }

    #[test]
    fn an_https_proxy_is_reached_by_plain_connect() {
        let settings = environment(&[("https_proxy", "https://user:pw@proxy.test:8443")]);
        let url = "https://api.github.com/repos";
        assert_eq!(
            settings
                .choice_for(url)
                .expect("an HTTPS proxy is no failure"),
            Some("http://user:pw@proxy.test:8443".to_owned())
        );
        let proxy = proxy_of(&settings, url);
        assert_eq!(proxy.protocol(), ureq::ProxyProtocol::Http);
        assert_eq!((proxy.host(), proxy.port()), ("proxy.test", 8443));
        assert_eq!(
            (proxy.username(), proxy.password()),
            (Some("user"), Some("pw"))
        );
    }

    #[test]
    fn a_proxy_address_is_read_as_urllib_reads_it() {
        let url = "https://api.github.com/repos";
        let read = [
            // A path after the address is no part of it, and the scheme is read in any case.
            ("http://proxy.test:8080/", "http://proxy.test:8080"),
            ("HTTP://proxy.test:8080", "http://proxy.test:8080"),
            ("//proxy.test:8080", "http://proxy.test:8080"),
            ("proxy.test", "http://proxy.test"),
            ("user:pw@proxy.test:3128", "http://user:pw@proxy.test:3128"),
            ("http://[::1]:3128", "http://[::1]:3128"),
            // The authority ends at the first slash after the user information.
            (
                "http://user:pw@proxy.test:3128/a@b/c",
                "http://user:pw@proxy.test:3128",
            ),
        ];
        for (address, proxy) in read {
            let settings = environment(&[("https_proxy", address)]);
            assert_eq!(
                settings
                    .choice_for(url)
                    .unwrap_or_else(|error| panic!("{address}: {error}")),
                Some(proxy.to_owned()),
                "{address}"
            );
        }
        let refused = [
            ("http:/proxy.test:8080", "the proxy's URL names no host"),
            (
                "ftp://proxy.test:21",
                "only HTTP and HTTPS proxies are supported",
            ),
            ("http://", UNREADABLE_ADDRESS),
            ("http://proxy test:8080", UNREADABLE_ADDRESS),
            // urllib reads the host after the user information; a URL would end the authority
            // inside it and name another host.
            ("http://us/er:pw@proxy.test:3128", UNREADABLE_ADDRESS),
            ("http://us?er:pw@proxy.test:3128", UNREADABLE_ADDRESS),
            ("us#er:pw@proxy.test:3128", UNREADABLE_ADDRESS),
        ];
        for (address, why) in refused {
            let settings = environment(&[("https_proxy", address)]);
            match settings.choice_for(url) {
                Err(ReleaseError::Proxy { detail }) => assert_eq!(&*detail, why, "{address}"),
                other => panic!("{address}: {other:?}"),
            }
        }
    }

    #[test]
    fn names_that_ignore_case_are_read_as_windows_holds_them() {
        let cgi = [
            ("request_method", "GET"),
            ("http_proxy", "http://proxy.test:8080"),
        ];
        let url = "http://127.0.0.1/";
        // Where names keep their case, neither variable is the one the CGI rule names upper-case.
        let kept = ProxySettings::new(variables(&cgi), None, false);
        assert_eq!(
            kept.choice_for(url).expect("no failure"),
            Some("http://proxy.test:8080".to_owned())
        );
        // Where they ignore it, both are upper-case: the CGI rule holds, and no lower-case
        // variable gives the HTTP proxy back.
        let dropped = ProxySettings::new(variables(&cgi), None, true);
        assert_eq!(dropped.choice_for(url).expect("no failure"), None);
        // A name given twice is one variable, its last value kept.
        let twice = [
            ("https_proxy", "http://first.test:1"),
            ("HTTPS_PROXY", "http://last.test:2"),
        ];
        let once = ProxySettings::new(variables(&twice), None, true);
        assert_eq!(
            once.choice_for("https://api.github.com/")
                .expect("no failure"),
            Some("http://last.test:2".to_owned())
        );
    }

    #[test]
    fn only_the_variables_a_proxy_is_chosen_by_are_kept() {
        let unrelated = [
            ("PATH", "/usr/bin"),
            ("HOME", "/home/player"),
            ("proxy", "x"),
        ];
        assert_eq!(
            ProxySettings::new(variables(&unrelated), None, false),
            ProxySettings::none()
        );
        let related = [
            ("PATH", "/usr/bin"),
            ("REQUEST_METHOD", "GET"),
            ("Https_Proxy", "x"),
        ];
        assert_eq!(
            ProxySettings::new(variables(&related), None, false),
            ProxySettings::new(variables(&related[1..]), None, false)
        );
        // A name of the suffix alone serves the protocol of no name, which still hides the
        // Internet Settings.
        let bare = ProxySettings::new(
            variables(&[("_proxy", "http://bare.test:1")]),
            Some(InternetSettings {
                enabled: true,
                server: Some("proxy.corp:8080".to_owned()),
                bypass: None,
            }),
            false,
        );
        assert_eq!(
            bare.choice_for("https://api.github.com/")
                .expect("no failure"),
            None
        );
    }

    #[test]
    fn the_internet_settings_name_protocols_as_urllibs_handlers_take_them() {
        let url = "https://api.github.com/";
        // A protocol in upper case still serves its scheme, its address without a scheme an
        // HTTP proxy's.
        assert_eq!(
            internet("HTTPS=s:2", None)
                .choice_for(url)
                .expect("no failure"),
            Some("http://s:2".to_owned())
        );
        // Of two entries that serve one scheme, the later one serves it.
        assert_eq!(
            internet("https=a:1;HTTPS=b:2", None)
                .choice_for(url)
                .expect("no failure"),
            Some("http://b:2".to_owned())
        );
        // A switched-off proxy bypasses nothing either, and a missing server names none.
        let off = ProxySettings::new(
            Vec::new(),
            Some(InternetSettings {
                enabled: false,
                server: Some("proxy.corp:8080".to_owned()),
                bypass: Some("<local>".to_owned()),
            }),
            true,
        );
        assert_eq!(
            off.choice_for("https://intranet/").expect("no failure"),
            None
        );
        let missing = ProxySettings::new(
            Vec::new(),
            Some(InternetSettings {
                enabled: true,
                server: None,
                bypass: None,
            }),
            true,
        );
        assert_eq!(missing.choice_for(url).expect("no failure"), None);
    }

    #[test]
    fn this_process_is_read_as_its_system_names_variables() {
        let unicode = || {
            std::env::vars_os()
                .filter_map(|(name, value)| {
                    Some((name.into_string().ok()?, value.into_string().ok()?))
                })
                .collect::<Vec<_>>()
        };
        let internet = InternetSettings {
            enabled: true,
            server: Some("proxy.corp:8080".to_owned()),
            bypass: Some("<local>".to_owned()),
        };
        assert_eq!(
            ProxySettings::of_process(Some(internet.clone())),
            ProxySettings::new(unicode(), Some(internet), cfg!(windows))
        );
        assert_eq!(
            ProxySettings::of_process(None),
            ProxySettings::new(unicode(), None, cfg!(windows))
        );
    }

    #[test]
    fn shell_patterns_match_as_windows_matches_them() {
        // Each answer is what Python's fnmatch gives on Windows for the pattern and the name.
        let cases = [
            ("*", "api.github.com", true),
            ("", "api.github.com", false),
            ("api.github.com", "API.GitHub.com", true),
            ("*.github.com", "github.com", false),
            ("*.github.com", "api.github.com", true),
            ("api.*", "api.github.com", true),
            ("a*b*c", "axxbyyc", true),
            ("a*b*c", "axxbyyca", false),
            ("a**c", "abc", true),
            ("a*c*", "abcabc", true),
            ("???", "abc", true),
            ("???", "ab", false),
            ("[a-c]pi.github.com", "api.github.com", true),
            ("[!a]pi.github.com", "api.github.com", false),
            ("[!x]pi.gith?b.co[a-n]", "api.github.com", true),
            ("[A-C]x", "bx", true),
            ("[z-a]", "a", false),
            ("[!z-a]", "q", true),
            ("[az-ab]", "a", true),
            ("[az-ab]", "b", true),
            ("[az-ab]", "m", false),
            ("[]]", "]", true),
            ("[!]]", "]", false),
            ("[!]]", "a", true),
            ("[", "[", true),
            ("[!]", "[!]", true),
            ("[a-]", "-", true),
            ("[-a]", "-", true),
            ("[a-b-d]", "c", false),
            ("[a-b-d]", "-", true),
            ("[a-b-d]", "d", true),
            ("[!-x]", "-", false),
            ("*[", "a[", true),
            ("[/]", "\\", true),
            ("a/b", "a\\b", true),
            ("192.168.*", "192.168.0.1", true),
            ("192.168.*", "10.0.0.1", false),
        ];
        for (pattern, name, matches) in cases {
            assert_eq!(
                shell_pattern_matches(pattern, name),
                matches,
                "{pattern:?} {name:?}"
            );
        }
    }
}
