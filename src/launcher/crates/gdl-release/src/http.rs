//! How the launcher reaches GitHub's releases: which URLs a request and each of its redirects may
//! go to, redirects followed by hand so that every hop is held to that rule, the headers and
//! timeouts every request carries, GitHub's rate limits read in the order GitHub asks clients to
//! read them, and answers read only up to a bound. Nothing else in the crate names the
//! transport's own types.

use std::io::{self, Read};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use gdl_files::Cancel;
use serde_json::Value;
use ureq::http::uri::{Authority, Scheme};
use ureq::http::{Response, Uri};
use ureq::tls::{RootCerts, TlsConfig};
use ureq::{Agent, Body};

use crate::{GITHUB_API_HOST, ProxySettings, RELEASE_HOSTS, ReleaseError};

/// The version of GitHub's API the launcher asks for, which GitHub answers in
/// `X-GitHub-Api-Version-Selected`.
pub const API_VERSION: &str = "2026-03-10";

/// What a request to GitHub's API accepts: its JSON.
pub const API_ACCEPT: &str = "application/vnd.github+json";

/// What a download accepts: the file's bytes.
pub const DOWNLOAD_ACCEPT: &str = "application/octet-stream";

/// The product every request's `User-Agent` names, before the launcher's version.
pub const USER_AGENT_PRODUCT: &str = "GauntletDarkLegacy-Launcher";

/// How long each step of a request may take: resolving the host, connecting, sending, the
/// answer's headers, and every read of its body. A slow transfer that keeps moving is never cut
/// off.
pub const READ_TIMEOUT: Duration = Duration::from_secs(15);

/// The most redirects a request follows, as urllib follows at most this many.
pub const MAX_REDIRECTS: u32 = 10;

/// The statuses GitHub refuses a request with when a rate limit is spent.
const RATE_LIMIT_STATUSES: [u16; 2] = [403, 429];

/// The status of a rate limit that is always one, whatever else the answer says.
const TOO_MANY_REQUESTS: u16 = 429;

/// The redirect statuses a GET follows.
const REDIRECT_STATUSES: [u16; 5] = [301, 302, 303, 307, 308];

/// The status of the answer a request is made for.
const OK: u16 = 200;

/// How long a secondary rate limit that names no wait is waited out, in seconds: GitHub asks
/// for at least a minute.
pub const SECONDARY_LIMIT_WAIT: u64 = 60;

/// The most of a refused answer's body read to tell a secondary rate limit.
const MAX_ERROR_BODY: u64 = 64 << 10;

/// What the message of a secondary rate limit's answer holds, letter case aside.
const SECONDARY_LIMIT_TEXT: &str = "secondary rate limit";

/// The key of a refused answer's message in GitHub's JSON.
const MESSAGE: &str = "message";

/// The request header that names what the answer may be.
const ACCEPT: &str = "accept";

/// The request header that names the API version asked for.
const API_VERSION_HEADER: &str = "x-github-api-version";

/// The answer header that names where a redirect goes.
const LOCATION: &str = "location";

/// The answer header that names how many seconds to wait before asking again.
const RETRY_AFTER: &str = "retry-after";

/// The answer header that names how many requests the hourly limit still allows.
const RATE_LIMIT_REMAINING: &str = "x-ratelimit-remaining";

/// The answer header that names when the hourly limit starts again, in seconds since the epoch.
const RATE_LIMIT_RESET: &str = "x-ratelimit-reset";

/// What `x-ratelimit-remaining` holds once the hourly limit is spent.
const NONE_REMAINING: &str = "0";

/// What an authority ends in that names HTTPS's own port.
const HTTPS_PORT_SUFFIX: &str = ":443";

/// What parts a URL's scheme from its authority.
const SCHEME_SEPARATOR: &str = "://";

/// What a scheme-relative location begins with.
const SCHEME_RELATIVE: &str = "//";

/// The failures of the system's network that mean the feed could not be reached at all.
const OFFLINE_KINDS: [io::ErrorKind; 7] = [
    io::ErrorKind::ConnectionRefused,
    io::ErrorKind::ConnectionReset,
    io::ErrorKind::ConnectionAborted,
    io::ErrorKind::TimedOut,
    io::ErrorKind::NetworkUnreachable,
    io::ErrorKind::HostUnreachable,
    io::ErrorKind::NotConnected,
];

/// Where a request may go: GitHub's release hosts over HTTPS, or a loopback feed's own origin.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Origins {
    /// GitHub's release hosts, over HTTPS.
    GitHub,
    /// A feed on this machine, over plain HTTP, at exactly this authority (`127.0.0.1:<port>`
    /// or `[::1]:<port>`).
    #[cfg(test)]
    Loopback {
        /// The feed's host and port.
        authority: String,
    },
}

/// Whether `url` may be fetched, and the URL it names: for GitHub, scheme `https`, no user
/// information, port none or 443, and a host among [`RELEASE_HOSTS`], letter case aside; for a
/// loopback feed, scheme `http` and exactly its authority. Anything else is
/// [`ReleaseError::UntrustedHost`].
pub(crate) fn trusted(url: &str, origins: &Origins) -> Result<Uri, ReleaseError> {
    let refused = || ReleaseError::UntrustedHost { url: url.into() };
    let uri: Uri = url.parse().map_err(|_| refused())?;
    let authority = uri.authority().map(Authority::as_str).ok_or_else(refused)?;
    let reachable = match origins {
        Origins::GitHub => uri.scheme() == Some(&Scheme::HTTPS) && is_release_host(authority),
        #[cfg(test)]
        Origins::Loopback { authority: own } => {
            uri.scheme() == Some(&Scheme::HTTP) && authority == own
        }
    };
    if reachable { Ok(uri) } else { Err(refused()) }
}

/// Whether `authority` is one of GitHub's release hosts, letter case aside, with HTTPS's own
/// port or none. An authority with user information names no such host.
fn is_release_host(authority: &str) -> bool {
    let host = authority
        .strip_suffix(HTTPS_PORT_SUFFIX)
        .unwrap_or(authority);
    RELEASE_HOSTS
        .iter()
        .any(|known| host.eq_ignore_ascii_case(known))
}

/// The next URL of a redirect from `from` to `location`: an absolute URL, a scheme-relative one
/// (`//host/path`) or an absolute path (`/path`), resolved against `from`; any other form is
/// [`ReleaseError::Failed`]. Where the next URL may go is [`trusted`]'s to judge.
pub(crate) fn redirected(from: &Uri, location: &str) -> Result<String, ReleaseError> {
    if has_scheme(location) {
        return Ok(location.to_owned());
    }
    let scheme = from.scheme_str().unwrap_or_default();
    if location.starts_with(SCHEME_RELATIVE) {
        return Ok(format!("{scheme}:{location}"));
    }
    if location.starts_with('/') {
        let authority = from.authority().map_or("", Authority::as_str);
        return Ok(format!("{scheme}{SCHEME_SEPARATOR}{authority}{location}"));
    }
    Err(ReleaseError::Failed {
        detail: format!(
            "a redirect from {from} goes to {location:?}, which is neither a URL nor an \
             absolute path"
        )
        .into(),
    })
}

/// Whether `location` begins with a scheme and `://`: a letter, then letters, digits, `+`, `-`
/// or `.`.
fn has_scheme(location: &str) -> bool {
    let Some((scheme, _)) = location.split_once(SCHEME_SEPARATOR) else {
        return false;
    };
    let mut characters = scheme.chars();
    characters
        .next()
        .is_some_and(|first| first.is_ascii_alphabetic())
        && characters.all(|next| next.is_ascii_alphanumeric() || matches!(next, '+' | '-' | '.'))
}

/// `path` with every `%XX` decoded to the byte it names; `None` for a `%` not followed by two
/// hexadecimal digits, or decoded bytes that are not UTF-8.
pub(crate) fn percent_decoded(path: &str) -> Option<String> {
    let bytes = path.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len());
    let mut at = 0;
    while let Some(&byte) = bytes.get(at) {
        if byte == b'%' {
            let high = hex_value(*bytes.get(at + 1)?)?;
            let low = hex_value(*bytes.get(at + 2)?)?;
            decoded.push((high << 4) | low);
            at += 3;
        } else {
            decoded.push(byte);
            at += 1;
        }
    }
    String::from_utf8(decoded).ok()
}

/// The value of the hexadecimal digit `digit`, in either case.
fn hex_value(digit: u8) -> Option<u8> {
    char::from(digit)
        .to_digit(16)
        .and_then(|value| u8::try_from(value).ok())
}

/// What a request is for: the API's JSON, or a file's bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Wanted {
    /// A page of the releases list.
    Api,
    /// A release asset's bytes.
    File,
}

/// The transport every request of the feed and its downloads goes through: one agent that
/// follows no redirect of its own, trusts the system's certificate store, sends the launcher's
/// own `User-Agent` and gives up a step after [`READ_TIMEOUT`], with each hop's proxy chosen
/// from the system's settings.
pub(crate) struct Transport {
    agent: Agent,
    origins: Origins,
    proxies: ProxySettings,
}

impl Transport {
    /// A transport to `origins`, through `proxies`, naming launcher `launcher_version`. Only a
    /// loopback feed is reached over plain HTTP.
    pub(crate) fn new(
        origins: Origins,
        proxies: ProxySettings,
        launcher_version: &str,
    ) -> Transport {
        let tls = TlsConfig::builder()
            .root_certs(RootCerts::PlatformVerifier)
            .build();
        let agent = Agent::config_builder()
            .https_only(matches!(origins, Origins::GitHub))
            .max_redirects(0)
            .http_status_as_error(false)
            .user_agent(format!("{USER_AGENT_PRODUCT}/{launcher_version}"))
            .timeout_resolve(Some(READ_TIMEOUT))
            .timeout_connect(Some(READ_TIMEOUT))
            .timeout_send_request(Some(READ_TIMEOUT))
            .timeout_recv_response(Some(READ_TIMEOUT))
            .timeout_per_read(Some(READ_TIMEOUT))
            // Each request names the proxy chosen for it, so the agent reads none of its own.
            .proxy(None)
            .tls_config(tls)
            .build()
            .new_agent();
        Transport {
            agent,
            origins,
            proxies,
        }
    }

    /// GETs `url`, following redirects by hand, and gives the final 200 answer. Every hop is
    /// held to [`trusted`]; a URL reached twice, or a redirect past [`MAX_REDIRECTS`], is
    /// [`ReleaseError::Redirects`]; a spent rate limit is [`ReleaseError::RateLimited`]; any
    /// other answer is [`ReleaseError::Status`]. The cancel is looked at before each hop.
    pub(crate) fn get(
        &self,
        url: &str,
        wanted: Wanted,
        cancel: &Cancel,
    ) -> Result<Response<Body>, ReleaseError> {
        let mut uri = trusted(url, &self.origins)?;
        let mut reached = vec![uri.to_string()];
        loop {
            cancel.check()?;
            let mut response = self.call(&uri, wanted)?;
            let status = response.status().as_u16();
            if status == OK {
                return Ok(response);
            }
            if REDIRECT_STATUSES.contains(&status) {
                let next = trusted(
                    &redirected(&uri, location_of(&response, &uri)?)?,
                    &self.origins,
                )?;
                let text = next.to_string();
                // `reached` holds the first URL and one for each redirect followed.
                if reached.len() > MAX_REDIRECTS as usize || reached.contains(&text) {
                    return Err(ReleaseError::Redirects { url: url.into() });
                }
                reached.push(text);
                uri = next;
                continue;
            }
            if RATE_LIMIT_STATUSES.contains(&status)
                && let Some(limit) = rate_limit(status, &mut response)
            {
                return Err(limit);
            }
            return Err(ReleaseError::Status {
                status,
                url: uri.to_string().into(),
            });
        }
    }

    /// One request to `uri`, with the headers `wanted` asks for and the proxy chosen for it.
    fn call(&self, uri: &Uri, wanted: Wanted) -> Result<Response<Body>, ReleaseError> {
        let (accept, api_version) = self.headers_for(uri, wanted);
        let mut request = self.agent.get(uri.clone()).header(ACCEPT, accept);
        if let Some(version) = api_version {
            request = request.header(API_VERSION_HEADER, version);
        }
        request
            .config()
            .proxy(self.proxies.proxy_for(uri)?)
            .build()
            .call()
            .map_err(failure_of)
    }

    /// What a request to `uri` for `wanted` accepts, and the API version it asks for: the
    /// version goes to GitHub's API alone, never to the hosts a download is sent on to.
    fn headers_for(&self, uri: &Uri, wanted: Wanted) -> (&'static str, Option<&'static str>) {
        let accept = match wanted {
            Wanted::Api => API_ACCEPT,
            Wanted::File => DOWNLOAD_ACCEPT,
        };
        let to_the_api = match &self.origins {
            Origins::GitHub => uri
                .host()
                .is_some_and(|host| host.eq_ignore_ascii_case(GITHUB_API_HOST)),
            // A loopback feed serves its API and its downloads from one host.
            #[cfg(test)]
            Origins::Loopback { .. } => wanted == Wanted::Api,
        };
        (accept, to_the_api.then_some(API_VERSION))
    }
}

/// Where the redirect `response` to a request of `from` goes, as its `Location` names it.
fn location_of<'a>(response: &'a Response<Body>, from: &Uri) -> Result<&'a str, ReleaseError> {
    response
        .headers()
        .get(LOCATION)
        .and_then(|location| location.to_str().ok())
        .ok_or_else(|| ReleaseError::Failed {
            detail: format!("a redirect from {from} names no place to go").into(),
        })
}

/// The rate limit a refusal of `status` names, in the order GitHub asks clients to read one: a
/// `retry-after` of whole seconds, waited from now; else an hourly limit spent, until its
/// `x-ratelimit-reset`; else a secondary limit, a 429 or a 403 whose message names one, waited
/// [`SECONDARY_LIMIT_WAIT`]. `None` for a refusal that is no rate limit.
fn rate_limit(status: u16, response: &mut Response<Body>) -> Option<ReleaseError> {
    let header = |name: &str| {
        response
            .headers()
            .get(name)
            .and_then(|value| value.to_str().ok())
            .map(str::trim)
    };
    let limited = |reset_at| Some(ReleaseError::RateLimited { reset_at });
    if let Some(wait) = header(RETRY_AFTER).and_then(|wait| wait.parse::<u64>().ok()) {
        return limited(Some(now().saturating_add(wait)));
    }
    if header(RATE_LIMIT_REMAINING) == Some(NONE_REMAINING) {
        return limited(header(RATE_LIMIT_RESET).and_then(|reset| reset.parse().ok()));
    }
    if status == TOO_MANY_REQUESTS || names_a_secondary_limit(response.body_mut()) {
        return limited(Some(now().saturating_add(SECONDARY_LIMIT_WAIT)));
    }
    None
}

/// Whether a refusal's `body`, read to [`MAX_ERROR_BODY`], is GitHub's JSON whose message names a
/// secondary rate limit.
fn names_a_secondary_limit(body: &mut Body) -> bool {
    let Ok(bytes) = read_bounded(body, MAX_ERROR_BODY, ReleaseError::ResponseTooLarge) else {
        return false;
    };
    let Ok(Value::Object(answer)) = serde_json::from_slice::<Value>(&bytes) else {
        return false;
    };
    answer
        .get(MESSAGE)
        .and_then(Value::as_str)
        .is_some_and(|message| message.to_ascii_lowercase().contains(SECONDARY_LIMIT_TEXT))
}

/// The seconds since the Unix epoch, by the system's clock.
fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |since| since.as_secs())
}

/// Reads at most `limit` bytes of `body`, refusing one longer with `too_long`: one byte more than
/// the limit is asked for, so a body of exactly the limit is read whole.
pub(crate) fn read_bounded(
    body: &mut Body,
    limit: u64,
    too_long: ReleaseError,
) -> Result<Vec<u8>, ReleaseError> {
    let mut bytes = Vec::new();
    body.as_reader()
        .take(limit.saturating_add(1))
        .read_to_end(&mut bytes)
        .map_err(read_failure)?;
    if bytes.len() as u64 > limit {
        return Err(too_long);
    }
    Ok(bytes)
}

/// The feed's failure for `error`, a read of an answer's body that failed, named as
/// [`failure_of`] names the transport's own failures: a transfer broken part way is
/// [`ReleaseError::Failed`] or [`ReleaseError::Offline`].
pub(crate) fn read_failure(error: io::Error) -> ReleaseError {
    failure_of(ureq::Error::from(error))
}

/// The feed's failure for the transport's `error`: an unreachable host or network, or a step that
/// timed out, is [`ReleaseError::Offline`]; plain HTTP where HTTPS is required is
/// [`ReleaseError::UntrustedHost`]; a proxy that cannot be used is [`ReleaseError::Proxy`]; any
/// other, a failed secure connection among them, is [`ReleaseError::Failed`] in its own words.
fn failure_of(error: ureq::Error) -> ReleaseError {
    let detail = || -> Box<str> { error.to_string().into() };
    match &error {
        ureq::Error::HostNotFound | ureq::Error::ConnectionFailed | ureq::Error::Timeout(_) => {
            ReleaseError::Offline { detail: detail() }
        }
        ureq::Error::Io(failure) if OFFLINE_KINDS.contains(&failure.kind()) => {
            ReleaseError::Offline { detail: detail() }
        }
        ureq::Error::RequireHttpsOnly(url) => ReleaseError::UntrustedHost {
            url: url.as_str().into(),
        },
        ureq::Error::InvalidProxyUrl => ReleaseError::Proxy { detail: detail() },
        // The transport's failures are a list that may grow, so every other one is named here.
        _ => ReleaseError::Failed { detail: detail() },
    }
}

#[cfg(test)]
mod tests {
    use std::io;
    use std::net::TcpListener;
    use std::time::{SystemTime, UNIX_EPOCH};

    use gdl_files::Cancel;
    use gdl_fixtures::{LoopbackServer, Reply, Request};
    use ureq::http::Uri;

    use super::{
        API_ACCEPT, API_VERSION, DOWNLOAD_ACCEPT, MAX_REDIRECTS, Origins, SECONDARY_LIMIT_WAIT,
        Transport, Wanted, failure_of, percent_decoded, read_bounded, redirected, trusted,
    };
    use crate::{MAX_PAGE_BYTES, ProxySettings, ReleaseError};

    /// The launcher version the tests' requests name.
    const RUNNING: &str = "1.0.0-beta";

    /// A time an hourly limit starts again, in seconds since the epoch.
    const RESET_AT: u64 = 1_764_003_600;

    /// A page of the releases list, served for the transport's tests.
    const PAGE: &[u8] = b"[]";

    fn uri(text: &str) -> Uri {
        text.parse()
            .unwrap_or_else(|error| panic!("{text}: {error}"))
    }

    fn now() -> u64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("the clock is past the epoch")
            .as_secs()
    }

    /// The host and port of a loopback server's base.
    fn authority_of(server: &LoopbackServer) -> String {
        let base = server.base();
        base.strip_prefix("http://")
            .and_then(|rest| rest.strip_suffix('/'))
            .expect("a loopback base")
            .to_owned()
    }

    /// A transport to `server` alone, going direct.
    fn transport(server: &LoopbackServer) -> Transport {
        let origins = Origins::Loopback {
            authority: authority_of(server),
        };
        Transport::new(origins, ProxySettings::none(), RUNNING)
    }

    /// The body `transport` gives for `url`, read to a page's bound.
    fn fetched(transport: &Transport, url: &str, wanted: Wanted) -> Result<Vec<u8>, ReleaseError> {
        let mut response = transport.get(url, wanted, &Cancel::default())?;
        read_bounded(
            response.body_mut(),
            MAX_PAGE_BYTES,
            ReleaseError::ResponseTooLarge,
        )
    }

    /// The value of `request`'s header `name`, letter case aside.
    fn header<'a>(request: &'a Request, name: &str) -> Option<&'a str> {
        request
            .headers
            .iter()
            .find(|(given, _)| given.eq_ignore_ascii_case(name))
            .map(|(_, value)| value.as_str())
    }

    /// An answer of `status` carrying `body`.
    fn answer(status: u16, body: &[u8]) -> Reply {
        let mut reply = Reply::ok(body);
        reply.status = status;
        reply
    }

    /// The reset a rate-limited answer names, or a panic naming what came instead.
    fn reset_of(result: Result<Vec<u8>, ReleaseError>) -> Option<u64> {
        match result {
            Err(ReleaseError::RateLimited { reset_at }) => reset_at,
            other => panic!("not a rate limit: {other:?}"),
        }
    }

    /// Asserts that `reset` is `wait` seconds after a moment between `before` and now.
    fn assert_waited(reset: Option<u64>, before: u64, wait: u64) {
        let reset = reset.expect("a wait from now");
        assert!(
            (before + wait..=now() + wait).contains(&reset),
            "{reset} is not {wait} s from now"
        );
    }

    #[test]
    fn only_githubs_release_hosts_are_trusted() {
        for url in [
            "http://github.com/file",
            "https://github.com.evil.example/file",
            "file:///tmp/payload",
            "https://user@github.com/file",
            "https://:password@github.com/file",
            "https://github.com:443@evil.example/file",
            "https://github.com:444/file",
            "https://github.com:443:443/file",
            "https://API.GITHUB.COM.evil/x",
            "https://[::1]/x",
            "https://127.0.0.1/x",
            "ftp://github.com/file",
            "https://uploads.github.com/x",
            "https://githubusercontent.com/x",
            "github.com/file",
            "/repos/x",
            "",
            "not a url",
        ] {
            match trusted(url, &Origins::GitHub) {
                Err(ReleaseError::UntrustedHost { url: refused }) => {
                    assert_eq!(&*refused, url, "the refusal names the URL as given");
                }
                other => panic!("{url:?} was not refused: {other:?}"),
            }
        }
        for url in [
            "https://release-assets.githubusercontent.com/github-production-release-asset/abc?token=x",
            "https://objects.githubusercontent.com/github-production-release-asset/abc",
            "https://GitHub.com:443/x",
            "https://api.github.com/repos/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases?per_page=100&page=1",
            "https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/v1.0.0-beta/x",
            "HTTPS://github.com/x",
        ] {
            let accepted = trusted(url, &Origins::GitHub).unwrap_or_else(|error| panic!("{error}"));
            assert_eq!(accepted, uri(url), "{url}");
        }
    }

    #[test]
    fn a_redirect_to_plain_http_is_refused() {
        let from = uri("https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases");
        let next = redirected(&from, "http://github.com/file").expect("an absolute URL");
        assert_eq!(next, "http://github.com/file");
        assert!(matches!(
            trusted(&next, &Origins::GitHub),
            Err(ReleaseError::UntrustedHost { .. })
        ));
        // A scheme-relative redirect keeps HTTPS, but not to a host of another's.
        let next = redirected(&from, "//evil.example/file").expect("a scheme-relative URL");
        assert_eq!(next, "https://evil.example/file");
        assert!(matches!(
            trusted(&next, &Origins::GitHub),
            Err(ReleaseError::UntrustedHost { .. })
        ));
    }

    #[test]
    fn locations_resolve_as_a_browser_resolves_them() {
        let from = uri("https://github.com/sabishii-bit/x/releases/download/v1/a?b=c");
        for (location, next) in [
            (
                "https://objects.githubusercontent.com/z?token=t",
                "https://objects.githubusercontent.com/z?token=t",
            ),
            (
                "//release-assets.githubusercontent.com/p?q=1",
                "https://release-assets.githubusercontent.com/p?q=1",
            ),
            ("/other/path?y=1", "https://github.com/other/path?y=1"),
            ("/", "https://github.com/"),
            // A location whose query names a URL is still an absolute path.
            ("/go?to=https://x", "https://github.com/go?to=https://x"),
            ("ftp://elsewhere/x", "ftp://elsewhere/x"),
        ] {
            assert_eq!(
                redirected(&from, location).unwrap_or_else(|error| panic!("{error}")),
                next,
                "{location}"
            );
        }
        // An absolute path keeps the port of the URL it came from.
        let loopback = uri("http://127.0.0.1:8080/a");
        assert_eq!(
            redirected(&loopback, "/b").unwrap(),
            "http://127.0.0.1:8080/b"
        );
        for location in [
            "relative/path",
            "path",
            "?query",
            "#fragment",
            "",
            "mailto:someone",
            "1https://x/y",
            ":///x",
        ] {
            match redirected(&from, location) {
                Err(error @ ReleaseError::Failed { .. }) => {
                    assert!(
                        error.to_string().contains(&format!("{location:?}")),
                        "{error}"
                    );
                }
                other => panic!("{location:?} resolved: {other:?}"),
            }
        }
    }

    #[test]
    fn percent_escapes_decode_strictly() {
        assert_eq!(
            percent_decoded("/r/download/v1.0.0%2Bbuild.1/x").as_deref(),
            Some("/r/download/v1.0.0+build.1/x")
        );
        assert_eq!(percent_decoded("%2b").as_deref(), Some("+"));
        assert_eq!(percent_decoded("a+b").as_deref(), Some("a+b"));
        assert_eq!(percent_decoded("%C3%A9").as_deref(), Some("\u{e9}"));
        assert_eq!(percent_decoded("%25").as_deref(), Some("%"));
        assert_eq!(percent_decoded("").as_deref(), Some(""));
        for path in ["%2", "%", "%zz", "%2g", "a%", "%%41", "%FF", "%C3"] {
            assert_eq!(percent_decoded(path), None, "{path:?}");
        }
    }

    #[test]
    fn a_loopback_feed_trusts_its_own_origin_alone() {
        let origins = Origins::Loopback {
            authority: "127.0.0.1:8080".to_owned(),
        };
        let own = "http://127.0.0.1:8080/repos/x?per_page=100&page=1";
        assert_eq!(trusted(own, &origins).unwrap(), uri(own));
        assert!(trusted("http://127.0.0.1:8080", &origins).is_ok());
        for url in [
            "https://127.0.0.1:8080/x",
            "http://127.0.0.1:8081/x",
            "http://127.0.0.1/x",
            "http://localhost:8080/x",
            "http://[::1]:8080/x",
            "http://user@127.0.0.1:8080/x",
            "https://github.com/x",
            "https://api.github.com/repos",
        ] {
            assert!(
                matches!(
                    trusted(url, &origins),
                    Err(ReleaseError::UntrustedHost { .. })
                ),
                "{url}"
            );
        }
        // GitHub's hosts never take a loopback feed's URL.
        assert!(trusted(own, &Origins::GitHub).is_err());
        let ipv6 = Origins::Loopback {
            authority: "[::1]:8080".to_owned(),
        };
        assert!(trusted("http://[::1]:8080/x", &ipv6).is_ok());
        assert!(trusted("http://127.0.0.1:8080/x", &ipv6).is_err());
    }

    #[test]
    fn a_redirect_is_followed_within_the_origin() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let authority = authority_of(&server);
        server.route("/final", Reply::ok(PAGE));
        let transport = transport(&server);
        let locations = [
            "/final".to_owned(),
            format!("{base}final"),
            format!("//{authority}/final"),
        ];
        for status in [301, 302, 303, 307, 308] {
            for location in &locations {
                let start = format!("/moved-{status}");
                server.route(&start, Reply::redirect(status, location));
                let body = fetched(&transport, &format!("{base}moved-{status}"), Wanted::Api)
                    .unwrap_or_else(|error| panic!("{status} {location}: {error}"));
                assert_eq!(body, PAGE, "{status} {location}");
            }
        }
        // Every hop was asked for in turn, and nothing past the origin.
        let targets: Vec<String> = server
            .requests()
            .into_iter()
            .map(|request| request.target)
            .collect();
        assert_eq!(targets.len(), 5 * locations.len() * 2);
        for pair in targets.chunks(2) {
            assert!(pair[0].starts_with("/moved-"), "{pair:?}");
            assert_eq!(pair[1], "/final");
        }
    }

    #[test]
    fn a_redirect_out_of_the_origin_is_refused() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let authority = authority_of(&server);
        let port = authority.rsplit_once(':').unwrap().1;
        let elsewhere = format!("http://127.0.0.1:{}/b", port.parse::<u16>().unwrap() ^ 1);
        let transport = transport(&server);
        for location in [
            elsewhere.as_str(),
            &format!("https://{authority}/b"),
            &format!("http://localhost:{port}/b"),
            &format!("http://[::1]:{port}/b"),
            &format!("http://user@{authority}/b"),
            "//example.com/b",
            "https://github.com/x",
        ] {
            server.route("/a", Reply::redirect(302, location));
            match fetched(&transport, &format!("{base}a"), Wanted::Api) {
                Err(ReleaseError::UntrustedHost { url }) => assert!(
                    url.ends_with("/b") || url.ends_with("/x"),
                    "{location}: {url}"
                ),
                other => panic!("{location} was followed: {other:?}"),
            }
        }
        // A relative location is no place a redirect may go, and one naming none is no redirect.
        server.route("/a", Reply::redirect(302, "b"));
        assert!(matches!(
            fetched(&transport, &format!("{base}a"), Wanted::Api),
            Err(ReleaseError::Failed { .. })
        ));
        server.route("/a", Reply::status(302));
        match fetched(&transport, &format!("{base}a"), Wanted::Api) {
            Err(error @ ReleaseError::Failed { .. }) => {
                assert!(
                    error.to_string().contains("names no place to go"),
                    "{error}"
                );
            }
            other => panic!("a redirect without a location: {other:?}"),
        }
        // Nothing was asked of any place but the first.
        assert!(
            server
                .requests()
                .iter()
                .all(|request| request.target == "/a")
        );
    }

    #[test]
    fn a_redirect_loop_and_the_eleventh_hop_are_refused() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let transport = transport(&server);

        // A chain of `hops` redirects, then the page.
        let chain = |name: &str, hops: u32| {
            for hop in 0..hops {
                let next = format!("/{name}/{}", hop + 1);
                server.route(&format!("/{name}/{hop}"), Reply::redirect(302, &next));
            }
            server.route(&format!("/{name}/{hops}"), Reply::ok(PAGE));
            format!("{base}{name}/0")
        };
        let followed = chain("ten", MAX_REDIRECTS);
        assert_eq!(fetched(&transport, &followed, Wanted::Api).unwrap(), PAGE);
        let refused = chain("eleven", MAX_REDIRECTS + 1);
        match fetched(&transport, &refused, Wanted::Api) {
            Err(error @ ReleaseError::Redirects { .. }) => {
                assert_eq!(error.rule(), "redirects");
                assert!(error.to_string().ends_with(&refused), "{error}");
            }
            other => panic!("eleven redirects were followed: {other:?}"),
        }
        let eleventh: Vec<String> = server
            .requests()
            .into_iter()
            .map(|request| request.target)
            .filter(|target| target.starts_with("/eleven/"))
            .collect();
        let expected: Vec<String> = (0..=MAX_REDIRECTS)
            .map(|hop| format!("/eleven/{hop}"))
            .collect();
        assert_eq!(
            eleventh, expected,
            "the redirect past the tenth is not followed"
        );

        // A URL reached a second time ends the request, however few the redirects.
        server.route("/one", Reply::redirect(302, "/two"));
        server.route("/two", Reply::redirect(307, &format!("{base}one")));
        server.route("/self", Reply::redirect(301, "/self"));
        for start in ["one", "self"] {
            assert!(
                matches!(
                    fetched(&transport, &format!("{base}{start}"), Wanted::Api),
                    Err(ReleaseError::Redirects { .. })
                ),
                "{start}"
            );
        }
    }

    #[test]
    fn a_spent_rate_limit_names_its_reset() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let transport = transport(&server);
        server.route(
            "/spent",
            Reply::status(403)
                .with_header("X-RateLimit-Remaining", "0")
                .with_header("X-RateLimit-Reset", &RESET_AT.to_string()),
        );
        let spent = fetched(&transport, &format!("{base}spent"), Wanted::Api);
        assert_eq!(reset_of(spent), Some(RESET_AT));

        // A reset that is no number of seconds names no time.
        server.route(
            "/unread",
            Reply::status(403)
                .with_header("X-RateLimit-Remaining", "0")
                .with_header("X-RateLimit-Reset", "soon"),
        );
        let unread = fetched(&transport, &format!("{base}unread"), Wanted::Api);
        assert_eq!(reset_of(unread), None);

        // A 429 spent the same way names the same reset.
        server.route(
            "/many",
            Reply::status(429)
                .with_header("X-RateLimit-Remaining", "0")
                .with_header("X-RateLimit-Reset", &RESET_AT.to_string()),
        );
        let many = fetched(&transport, &format!("{base}many"), Wanted::Api);
        assert_eq!(reset_of(many), Some(RESET_AT));

        server.route("/wait", Reply::status(429).with_header("Retry-After", "60"));
        let before = now();
        let waited = fetched(&transport, &format!("{base}wait"), Wanted::Api);
        assert_waited(reset_of(waited), before, 60);
    }

    #[test]
    fn retry_after_comes_before_the_primary_reset() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let transport = transport(&server);
        let hour_ahead = now() + 3600;
        server.route(
            "/limited",
            Reply::status(403)
                .with_header("Retry-After", "60")
                .with_header("X-RateLimit-Remaining", "0")
                .with_header("X-RateLimit-Reset", &hour_ahead.to_string()),
        );
        let before = now();
        let limited = fetched(&transport, &format!("{base}limited"), Wanted::Api);
        assert_waited(reset_of(limited), before, 60);

        // A wait given as a date is no number of seconds, so the hourly reset is read instead.
        server.route(
            "/dated",
            Reply::status(403)
                .with_header("Retry-After", "Wed, 21 Oct 2026 07:28:00 GMT")
                .with_header("X-RateLimit-Remaining", "0")
                .with_header("X-RateLimit-Reset", &hour_ahead.to_string()),
        );
        let dated = fetched(&transport, &format!("{base}dated"), Wanted::Api);
        assert_eq!(reset_of(dated), Some(hour_ahead));
    }

    #[test]
    fn a_secondary_limit_without_a_wait_is_waited_a_minute() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let transport = transport(&server);
        let message = br#"{"message": "You have exceeded a secondary rate limit.", "documentation_url": "https://docs.github.com/"}"#;
        server.route(
            "/secondary",
            answer(403, message).with_header("X-RateLimit-Remaining", "59"),
        );
        server.route(
            "/shouted",
            answer(403, br#"{"message": "SECONDARY RATE LIMIT"}"#),
        );
        server.route("/bare", Reply::status(429));
        for target in ["secondary", "shouted", "bare"] {
            let before = now();
            let limited = fetched(&transport, &format!("{base}{target}"), Wanted::Api);
            assert_waited(reset_of(limited), before, SECONDARY_LIMIT_WAIT);
        }
    }

    #[test]
    fn a_plain_403_is_a_status() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let transport = transport(&server);
        server.route(
            "/forbidden",
            answer(403, br#"{"message": "Forbidden"}"#).with_header("X-RateLimit-Remaining", "59"),
        );
        server.route("/bare", Reply::status(403));
        server.route(
            "/text",
            answer(403, b"secondary rate limit, but not GitHub's JSON"),
        );
        server.route("/missing", Reply::status(404));
        server.route("/broken", Reply::status(500));
        server.route("/unchanged", Reply::status(304));
        server.route("/empty", Reply::status(204));
        for (target, status) in [
            ("forbidden", 403),
            ("bare", 403),
            ("text", 403),
            ("missing", 404),
            ("broken", 500),
            ("unchanged", 304),
            ("empty", 204),
        ] {
            let url = format!("{base}{target}");
            match fetched(&transport, &url, Wanted::Api) {
                Err(error @ ReleaseError::Status { .. }) => {
                    assert_eq!(
                        error.to_string(),
                        format!("GitHub answered HTTP {status} for {url}")
                    );
                }
                other => panic!("{target}: {other:?}"),
            }
        }
    }

    #[test]
    fn the_api_headers_reach_the_api_alone() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let transport = transport(&server);
        server.route("/api", Reply::ok(PAGE));
        server.route("/file", Reply::ok(&b"bytes"[..]));
        server.route("/moved", Reply::redirect(302, "/api"));
        fetched(&transport, &format!("{base}api"), Wanted::Api).unwrap();
        fetched(&transport, &format!("{base}file"), Wanted::File).unwrap();
        fetched(&transport, &format!("{base}moved"), Wanted::Api).unwrap();
        let requests = server.requests();
        let seen: Vec<(&str, Option<&str>, Option<&str>)> = requests
            .iter()
            .map(|request| {
                (
                    request.target.as_str(),
                    header(request, "Accept"),
                    header(request, "X-GitHub-Api-Version"),
                )
            })
            .collect();
        assert_eq!(
            seen,
            [
                ("/api", Some(API_ACCEPT), Some(API_VERSION)),
                ("/file", Some(DOWNLOAD_ACCEPT), None),
                ("/moved", Some(API_ACCEPT), Some(API_VERSION)),
                ("/api", Some(API_ACCEPT), Some(API_VERSION)),
            ]
        );
        // No request carries credentials or cookies.
        for request in &requests {
            for name in ["Authorization", "Cookie", "Proxy-Authorization"] {
                assert_eq!(header(request, name), None, "{name} in {request:?}");
            }
        }

        // From GitHub, the API version goes to the API's host alone: a download sent on to the
        // storage hosts carries none.
        let github = Transport::new(Origins::GitHub, ProxySettings::none(), RUNNING);
        for url in [
            "https://api.github.com/repos/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases",
            "https://API.GitHub.com/repos",
        ] {
            assert_eq!(
                github.headers_for(&uri(url), Wanted::Api),
                (API_ACCEPT, Some(API_VERSION)),
                "{url}"
            );
        }
        for url in [
            "https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port/releases/download/v1/x",
            "https://release-assets.githubusercontent.com/x",
            "https://objects.githubusercontent.com/x",
        ] {
            assert_eq!(
                github.headers_for(&uri(url), Wanted::File),
                (DOWNLOAD_ACCEPT, None),
                "{url}"
            );
            assert_eq!(
                github.headers_for(&uri(url), Wanted::Api),
                (API_ACCEPT, None),
                "{url}"
            );
        }
    }

    #[test]
    fn the_user_agent_names_the_launcher_and_its_version() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        server.route("/moved", Reply::redirect(302, "/page"));
        server.route("/page", Reply::ok(PAGE));
        fetched(&transport(&server), &format!("{base}moved"), Wanted::Api).unwrap();
        let newer = Transport::new(
            Origins::Loopback {
                authority: authority_of(&server),
            },
            ProxySettings::none(),
            "1.2.3-rc.1+build.7",
        );
        fetched(&newer, &format!("{base}page"), Wanted::File).unwrap();
        let agents: Vec<Option<String>> = server
            .requests()
            .iter()
            .map(|request| header(request, "User-Agent").map(str::to_owned))
            .collect();
        assert_eq!(
            agents,
            [
                Some("GauntletDarkLegacy-Launcher/1.0.0-beta".to_owned()),
                Some("GauntletDarkLegacy-Launcher/1.0.0-beta".to_owned()),
                Some("GauntletDarkLegacy-Launcher/1.2.3-rc.1+build.7".to_owned()),
            ]
        );
    }

    #[test]
    fn a_body_past_its_limit_is_refused() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        let transport = transport(&server);
        let limit = usize::try_from(MAX_PAGE_BYTES).unwrap();
        server.route("/fits", Reply::ok(vec![b'x'; limit]));
        server.route("/over", Reply::ok(vec![b'x'; limit + 1]));
        let fits = fetched(&transport, &format!("{base}fits"), Wanted::Api).unwrap();
        assert_eq!(fits.len(), limit);
        assert!(matches!(
            fetched(&transport, &format!("{base}over"), Wanted::Api),
            Err(ReleaseError::ResponseTooLarge)
        ));

        // The refusal is the one the reader names, at whatever bound it reads to.
        server.route("/small", Reply::ok(&b"abcd"[..]));
        let read = |limit| {
            let mut response = transport
                .get(&format!("{base}small"), Wanted::File, &Cancel::default())
                .unwrap();
            let too_long = ReleaseError::Oversize {
                asset: "small".into(),
            };
            read_bounded(response.body_mut(), limit, too_long)
        };
        assert_eq!(read(4).unwrap(), b"abcd");
        assert_eq!(read(5).unwrap(), b"abcd");
        assert!(matches!(read(3), Err(ReleaseError::Oversize { asset }) if &*asset == "small"));
        assert!(matches!(read(0), Err(ReleaseError::Oversize { .. })));
    }

    #[test]
    fn a_refused_connection_is_offline() {
        // A port the system gave and took back, which nothing listens on.
        let port = {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            listener.local_addr().unwrap().port()
        };
        let authority = format!("127.0.0.1:{port}");
        let transport = Transport::new(
            Origins::Loopback {
                authority: authority.clone(),
            },
            ProxySettings::none(),
            RUNNING,
        );
        match transport.get(
            &format!("http://{authority}/x"),
            Wanted::Api,
            &Cancel::default(),
        ) {
            Err(error @ ReleaseError::Offline { .. }) => assert_eq!(error.rule(), "offline"),
            other => panic!("a closed port answered: {other:?}"),
        }
    }

    #[test]
    fn a_github_agent_refuses_plain_http() {
        let server = LoopbackServer::start().unwrap();
        server.route("/x", Reply::ok(PAGE));
        let github = Transport::new(Origins::GitHub, ProxySettings::none(), RUNNING);
        match github.get("http://github.com/file", Wanted::Api, &Cancel::default()) {
            Err(ReleaseError::UntrustedHost { url }) => assert_eq!(&*url, "http://github.com/file"),
            other => panic!("plain HTTP to GitHub: {other:?}"),
        }
        // Past the host rule, the agent itself refuses plain HTTP before it connects.
        let url = format!("{}x", server.base());
        match github.call(&uri(&url), Wanted::Api) {
            Err(error @ ReleaseError::UntrustedHost { .. }) => {
                assert!(error.to_string().contains(&url), "{error}");
            }
            other => panic!("a GitHub agent fetched {url}: {other:?}"),
        }
        assert!(server.requests().is_empty());
    }

    #[test]
    fn a_cancelled_request_sends_nothing() {
        let server = LoopbackServer::start().unwrap();
        server.route("/x", Reply::ok(PAGE));
        let cancel = Cancel::default();
        cancel.cancel();
        let url = format!("{}x", server.base());
        assert!(matches!(
            transport(&server).get(&url, Wanted::Api, &cancel),
            Err(ReleaseError::Cancelled)
        ));
        assert!(server.requests().is_empty());
    }

    #[test]
    fn transport_failures_are_named() {
        let refused = io::Error::from(io::ErrorKind::ConnectionRefused);
        let cases = [
            (ureq::Error::HostNotFound, "offline"),
            (ureq::Error::ConnectionFailed, "offline"),
            (ureq::Error::Timeout(ureq::Timeout::Connect), "offline"),
            (ureq::Error::Timeout(ureq::Timeout::PerRead), "offline"),
            (ureq::Error::Io(refused), "offline"),
            (
                ureq::Error::Io(io::ErrorKind::ConnectionReset.into()),
                "offline",
            ),
            (
                ureq::Error::Io(io::ErrorKind::NetworkUnreachable.into()),
                "offline",
            ),
            (
                ureq::Error::Io(io::ErrorKind::UnexpectedEof.into()),
                "failed",
            ),
            (ureq::Error::Tls("bad certificate"), "failed"),
            (
                ureq::Error::ConnectProxyFailed("refused".to_owned()),
                "failed",
            ),
            (ureq::Error::InvalidProxyUrl, "proxy"),
            (
                ureq::Error::RequireHttpsOnly("http://github.com/x".to_owned()),
                "untrustedHost",
            ),
        ];
        for (error, rule) in cases {
            let shown = error.to_string();
            let failure = failure_of(error);
            assert_eq!(failure.rule(), rule, "{shown}");
            match failure {
                ReleaseError::UntrustedHost { url } => assert_eq!(&*url, "http://github.com/x"),
                ReleaseError::Offline { detail }
                | ReleaseError::Failed { detail }
                | ReleaseError::Proxy { detail } => assert_eq!(&*detail, shown),
                other => panic!("{other:?}"),
            }
        }
    }
}
