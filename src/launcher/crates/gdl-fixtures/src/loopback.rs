//! A small HTTP/1.1 server on the loopback interface for the tests of the release feed and its
//! downloads: it answers each request target from a table the test fills, closes every connection
//! after its answer, and keeps every request it read, so a test can serve a feed, its redirects,
//! its rate limits and its broken transfers without reaching the network.

use std::collections::{BTreeMap, VecDeque};
use std::io::{self, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

/// The most bytes a request's head, its blank line included, may take: the server refuses a
/// longer one.
const MAX_REQUEST_HEAD: usize = 16 << 10;

/// What ends a request's head: the blank line after its last header.
const HEAD_END: &[u8; 4] = b"\r\n\r\n";

/// What ends each line of a request's head and of an answer's.
const LINE_END: &str = "\r\n";

/// The address the server listens on: the loopback interface, at a port the system chooses.
const LISTEN_ADDRESS: &str = "127.0.0.1:0";

/// The name of the server's thread, as a test's panic message names it.
const THREAD_NAME: &str = "loopback-server";

/// How many bytes a read of a request takes at most.
const READ_BLOCK: usize = 4 << 10;

/// How long the server waits on one read or write of a connection before giving it up, so a
/// client that stalls cannot hold the server's one thread for ever.
const STREAM_TIMEOUT: Duration = Duration::from_secs(10);

/// How long a dropped server waits to reach its own listener, which wakes its thread to stop.
const WAKE_TIMEOUT: Duration = Duration::from_secs(5);

/// The status of an answer to a target the table does not hold.
const NOT_FOUND: u16 = 404;

/// The status of an answer to a request whose head the server cannot read.
const BAD_REQUEST: u16 = 400;

/// The status of a whole answer.
const OK: u16 = 200;

/// The header that says how long an answer's body is.
const CONTENT_LENGTH: &str = "Content-Length";

/// The header that says where a redirect goes.
const LOCATION: &str = "Location";

/// The header every answer carries, since the server closes each connection after its answer.
const CONNECTION_CLOSE: &str = "Connection: close";

/// The version of HTTP the server answers in.
const HTTP_VERSION: &str = "HTTP/1.1";

/// What every request line's version begins with.
const HTTP_1: &str = "HTTP/1.";

/// What a loopback route answers.
#[derive(Clone, Debug)]
pub struct Reply {
    /// The answer's status code.
    pub status: u16,
    /// The answer's headers, in the order they are written; `Connection: close` follows them.
    pub headers: Vec<(String, String)>,
    /// The answer's body, written whole after the headers.
    pub body: Vec<u8>,
}

impl Reply {
    /// A 200 answer carrying `body`, with its `Content-Length`.
    pub fn ok(body: impl Into<Vec<u8>>) -> Reply {
        let body = body.into();
        Reply {
            status: OK,
            headers: vec![content_length(body.len() as u64)],
            body,
        }
    }

    /// A redirect with `status` to `location`, and no body.
    pub fn redirect(status: u16, location: &str) -> Reply {
        Reply {
            status,
            headers: vec![
                (LOCATION.to_owned(), location.to_owned()),
                content_length(0),
            ],
            body: Vec::new(),
        }
    }

    /// An answer of `status` alone, with no body.
    pub fn status(status: u16) -> Reply {
        Reply {
            status,
            headers: vec![content_length(0)],
            body: Vec::new(),
        }
    }

    /// The answer with the header `name: value` written after its others.
    pub fn with_header(mut self, name: &str, value: &str) -> Reply {
        self.headers.push((name.to_owned(), value.to_owned()));
        self
    }

    /// A body cut short: the headers promise `declared` bytes and the connection closes after the
    /// body. Panics unless `declared` is more than the body holds.
    pub fn cut(body: impl Into<Vec<u8>>, declared: u64) -> Reply {
        let body = body.into();
        assert!(
            declared > body.len() as u64,
            "a cut body promises more than its {} bytes, not {declared}",
            body.len()
        );
        Reply {
            status: OK,
            headers: vec![content_length(declared)],
            body,
        }
    }
}

/// A request the server was sent.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    /// The request's method, such as `GET`.
    pub method: String,
    /// The request's target, its query included, as the request line gives it.
    pub target: String,
    /// The request's headers in the order they were sent, each name as it was spelt and each value
    /// without the spaces about it.
    pub headers: Vec<(String, String)>,
}

/// An HTTP/1.1 server on 127.0.0.1 and an unused port, serving each request target from a table,
/// for the tests of the release feed and its downloads; it stops when dropped. One thread answers
/// the connections in turn, each with `Connection: close`, so a client reaches it again on a new
/// connection for every request. It reads a request's head alone, all that a GET sends.
pub struct LoopbackServer {
    /// Where the server listens.
    address: SocketAddr,
    /// The table of answers and the requests read, which the server's thread shares.
    shared: Arc<Shared>,
    /// The server's thread, until the server is dropped.
    thread: Option<JoinHandle<()>>,
}

/// What a server and its thread share.
#[derive(Default)]
struct Shared {
    /// The answers by request target, each target's in the order it is to give them.
    routes: Mutex<BTreeMap<String, VecDeque<Reply>>>,
    /// Every request read, in the order the server read them.
    requests: Mutex<Vec<Request>>,
    /// Set as the server is dropped, so its thread stops at its next connection.
    stopping: AtomicBool,
}

impl LoopbackServer {
    /// Starts a server on 127.0.0.1 at a port the system chooses, with an empty table.
    pub fn start() -> io::Result<LoopbackServer> {
        let listener = TcpListener::bind(LISTEN_ADDRESS)?;
        let address = listener.local_addr()?;
        let shared = Arc::new(Shared::default());
        let served = Arc::clone(&shared);
        let thread = thread::Builder::new()
            .name(THREAD_NAME.to_owned())
            .spawn(move || listen(&listener, &served))?;
        Ok(LoopbackServer {
            address,
            shared,
            thread: Some(thread),
        })
    }

    /// The server's base URL, `http://127.0.0.1:<port>/`.
    pub fn base(&self) -> String {
        format!("http://{}/", self.address)
    }

    /// Answers requests for `target`, its query included as a request line gives it, with
    /// `reply`. A target given several replies answers with each in turn, the last for every
    /// request after it, so a test can serve a broken transfer and then a whole one.
    pub fn route(&self, target: &str, reply: Reply) {
        locked(&self.shared.routes)
            .entry(target.to_owned())
            .or_default()
            .push_back(reply);
    }

    /// Every request the server has read so far, in the order it read them.
    pub fn requests(&self) -> Vec<Request> {
        locked(&self.shared.requests).clone()
    }
}

impl Drop for LoopbackServer {
    fn drop(&mut self) {
        self.shared.stopping.store(true, Ordering::SeqCst);
        // The thread waits in accept, so a connection of the server's own wakes it to stop. When
        // none can be made the thread is left waiting, rather than this drop.
        let woken = TcpStream::connect_timeout(&self.address, WAKE_TIMEOUT).is_ok();
        if let Some(thread) = self.thread.take()
            && woken
        {
            let _ = thread.join();
        }
    }
}

/// `mutex`'s value, also after a test panicked while it held it.
fn locked<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// The `Content-Length` header of a body of `len` bytes.
fn content_length(len: u64) -> (String, String) {
    (CONTENT_LENGTH.to_owned(), len.to_string())
}

/// Answers the listener's connections in turn until the server stops.
fn listen(listener: &TcpListener, shared: &Shared) {
    for stream in listener.incoming() {
        if shared.stopping.load(Ordering::SeqCst) {
            break;
        }
        if let Ok(stream) = stream {
            serve(shared, stream);
        }
    }
}

/// Reads one request from `stream`, keeps it, writes its answer and closes the connection.
fn serve(shared: &Shared, mut stream: TcpStream) {
    // A connection whose timeouts cannot be set is still served; its reads and writes may then
    // wait longer.
    let _ = stream.set_read_timeout(Some(STREAM_TIMEOUT));
    let _ = stream.set_write_timeout(Some(STREAM_TIMEOUT));
    let (reply, unread) = match read_head(&mut stream) {
        Ok(Head::Whole(head)) => match parse_head(&head) {
            Some(request) => {
                let reply = answer(shared, &request.target);
                locked(&shared.requests).push(request);
                (reply, false)
            }
            None => (Reply::status(BAD_REQUEST), false),
        },
        Ok(Head::TooLong) => (Reply::status(BAD_REQUEST), true),
        Ok(Head::Closed) | Err(_) => return,
    };
    // The client may have gone; what it no longer reads is no failure of the server's.
    let _ = write_reply(&mut stream, &reply);
    let _ = stream.shutdown(Shutdown::Write);
    if unread {
        drain(&mut stream);
    }
}

/// What a connection's first bytes held.
enum Head {
    /// A request's head, up to its blank line.
    Whole(Vec<u8>),
    /// No blank line within the first `MAX_REQUEST_HEAD` bytes.
    TooLong,
    /// The connection closed before its head ended.
    Closed,
}

/// Reads a request's head from `stream`, up to and without its blank line.
fn read_head(stream: &mut impl Read) -> io::Result<Head> {
    let mut head = Vec::new();
    let mut block = [0; READ_BLOCK];
    loop {
        let window = &head[..head.len().min(MAX_REQUEST_HEAD)];
        if let Some(end) = window
            .windows(HEAD_END.len())
            .position(|bytes| bytes == HEAD_END)
        {
            head.truncate(end);
            return Ok(Head::Whole(head));
        }
        if head.len() >= MAX_REQUEST_HEAD {
            return Ok(Head::TooLong);
        }
        let read = stream.read(&mut block)?;
        if read == 0 {
            return Ok(Head::Closed);
        }
        head.extend_from_slice(&block[..read]);
    }
}

/// The request a head spells: its request line, then a header a line; `None` for anything else.
fn parse_head(head: &[u8]) -> Option<Request> {
    let text = std::str::from_utf8(head).ok()?;
    let mut lines = text.split(LINE_END);
    let mut parts = lines.next()?.split(' ');
    let method = parts.next().filter(|method| !method.is_empty())?;
    let target = parts.next().filter(|target| !target.is_empty())?;
    let version = parts.next()?;
    if parts.next().is_some() || !version.starts_with(HTTP_1) {
        return None;
    }
    let headers = lines
        .map(|line| {
            let (name, value) = line.split_once(':')?;
            let named = !name.is_empty() && !name.contains(|c: char| c.is_ascii_whitespace());
            named.then(|| (name.to_owned(), value.trim_matches([' ', '\t']).to_owned()))
        })
        .collect::<Option<Vec<_>>>()?;
    Some(Request {
        method: method.to_owned(),
        target: target.to_owned(),
        headers,
    })
}

/// The table's next answer for `target`, which stays the answer once it is the last.
fn answer(shared: &Shared, target: &str) -> Reply {
    let mut routes = locked(&shared.routes);
    match routes.get_mut(target) {
        Some(replies) if replies.len() > 1 => replies.pop_front().expect("more than one reply"),
        Some(replies) => replies
            .front()
            .cloned()
            .unwrap_or_else(|| Reply::status(NOT_FOUND)),
        None => Reply::status(NOT_FOUND),
    }
}

/// Writes `reply` whole: its status line, its headers, `Connection: close`, a blank line, then
/// its body.
fn write_reply(stream: &mut impl Write, reply: &Reply) -> io::Result<()> {
    let mut head = format!(
        "{HTTP_VERSION} {} {}{LINE_END}",
        reply.status,
        reason(reply.status)
    );
    for (name, value) in &reply.headers {
        head.push_str(&format!("{name}: {value}{LINE_END}"));
    }
    head.push_str(&format!("{CONNECTION_CLOSE}{LINE_END}{LINE_END}"));
    stream.write_all(head.as_bytes())?;
    stream.write_all(&reply.body)?;
    stream.flush()
}

/// Reads and drops what the client still sends, until it closes its end or `STREAM_TIMEOUT`
/// passes, so that closing a connection whose request was refused part read does not reset it
/// before the client has read the answer. A request read whole leaves nothing to drain, and its
/// connection closes at once.
fn drain(stream: &mut TcpStream) {
    let until = Instant::now() + STREAM_TIMEOUT;
    let mut block = [0; READ_BLOCK];
    while Instant::now() < until {
        match stream.read(&mut block) {
            Ok(0) | Err(_) => return,
            Ok(_) => {}
        }
    }
}

/// The reason phrase of `status` among the answers the feed's tests give; HTTP allows an empty
/// one for the rest.
fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        301 => "Moved Permanently",
        302 => "Found",
        303 => "See Other",
        307 => "Temporary Redirect",
        308 => "Permanent Redirect",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        429 => "Too Many Requests",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "",
    }
}

#[cfg(test)]
mod tests {
    use std::io::{Read, Write};
    use std::net::TcpStream;
    use std::time::Duration;

    use super::{LoopbackServer, MAX_REQUEST_HEAD, Reply, Request};

    /// How long a test's client waits on the server.
    const CLIENT_TIMEOUT: Duration = Duration::from_secs(10);

    /// An answer as a client reads it to the connection's end.
    struct Answer {
        status_line: String,
        headers: Vec<(String, String)>,
        body: Vec<u8>,
    }

    impl Answer {
        fn header(&self, name: &str) -> Option<&str> {
            self.headers
                .iter()
                .find(|(given, _)| given.eq_ignore_ascii_case(name))
                .map(|(_, value)| value.as_str())
        }
    }

    /// Sends `request`'s bytes to `server` and reads the answer until the server closes.
    fn send(server: &LoopbackServer, request: &[u8]) -> Answer {
        let base = server.base();
        let authority = base
            .strip_prefix("http://")
            .and_then(|rest| rest.strip_suffix('/'))
            .expect("a loopback base");
        let mut stream = TcpStream::connect(authority).unwrap();
        stream.set_read_timeout(Some(CLIENT_TIMEOUT)).unwrap();
        stream.set_write_timeout(Some(CLIENT_TIMEOUT)).unwrap();
        stream.write_all(request).unwrap();
        let mut bytes = Vec::new();
        stream.read_to_end(&mut bytes).unwrap();
        let end = bytes
            .windows(4)
            .position(|window| window == b"\r\n\r\n")
            .expect("an answer's head");
        let head = std::str::from_utf8(&bytes[..end]).unwrap();
        let mut lines = head.split("\r\n");
        let status_line = lines.next().unwrap().to_owned();
        let headers = lines
            .map(|line| {
                let (name, value) = line.split_once(": ").unwrap();
                (name.to_owned(), value.to_owned())
            })
            .collect();
        Answer {
            status_line,
            headers,
            body: bytes[end + 4..].to_vec(),
        }
    }

    /// A GET of `target` as a client sends one.
    fn get(target: &str) -> Vec<u8> {
        format!("GET {target} HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent:  tester/1 \r\n\r\n")
            .into_bytes()
    }

    #[test]
    fn a_route_answers_and_the_request_is_kept() {
        let server = LoopbackServer::start().unwrap();
        let base = server.base();
        assert!(base.starts_with("http://127.0.0.1:"), "{base}");
        assert!(base.ends_with('/'), "{base}");
        let port: u16 = base["http://127.0.0.1:".len()..base.len() - 1]
            .parse()
            .unwrap();
        assert_ne!(port, 0);

        server.route(
            "/repos/x/releases?per_page=100&page=1",
            Reply::ok(&b"[]"[..]).with_header("X-RateLimit-Remaining", "59"),
        );
        let answer = send(&server, &get("/repos/x/releases?per_page=100&page=1"));
        assert_eq!(answer.status_line, "HTTP/1.1 200 OK");
        assert_eq!(answer.header("Content-Length"), Some("2"));
        assert_eq!(answer.header("X-RateLimit-Remaining"), Some("59"));
        assert_eq!(answer.header("Connection"), Some("close"));
        assert_eq!(answer.headers.len(), 3, "{:?}", answer.headers);
        assert_eq!(answer.body, b"[]");

        // A redirect names where it goes, and a bare status carries no body.
        server.route("/moved", Reply::redirect(302, "/elsewhere"));
        let answer = send(&server, &get("/moved"));
        assert_eq!(answer.status_line, "HTTP/1.1 302 Found");
        assert_eq!(answer.header("Location"), Some("/elsewhere"));
        assert_eq!(answer.header("Content-Length"), Some("0"));
        assert!(answer.body.is_empty());
        server.route(
            "/limited",
            Reply::status(429).with_header("Retry-After", "60"),
        );
        let answer = send(&server, &get("/limited"));
        assert_eq!(answer.status_line, "HTTP/1.1 429 Too Many Requests");
        assert_eq!(answer.header("Retry-After"), Some("60"));
        assert!(answer.body.is_empty());
        server.route("/teapot", Reply::status(418));
        assert_eq!(send(&server, &get("/teapot")).status_line, "HTTP/1.1 418 ");

        // Every request is kept as it was sent, the query in its target and each header's name
        // as it was spelt, its value trimmed.
        let sent = Request {
            method: "GET".to_owned(),
            target: String::new(),
            headers: vec![
                ("Host".to_owned(), "127.0.0.1".to_owned()),
                ("User-Agent".to_owned(), "tester/1".to_owned()),
            ],
        };
        let targets = [
            "/repos/x/releases?per_page=100&page=1",
            "/moved",
            "/limited",
            "/teapot",
        ];
        let expected: Vec<Request> = targets
            .iter()
            .map(|target| Request {
                target: (*target).to_owned(),
                ..sent.clone()
            })
            .collect();
        assert_eq!(server.requests(), expected);
    }

    #[test]
    fn replies_to_one_target_come_in_turn_and_the_last_stays() {
        let server = LoopbackServer::start().unwrap();
        server.route("/file", Reply::cut(&b"pay"[..], 7));
        server.route("/file", Reply::status(503));
        server.route("/file", Reply::ok(&b"payload"[..]));
        let first = send(&server, &get("/file"));
        assert_eq!(first.header("Content-Length"), Some("7"));
        assert_eq!(first.body, b"pay");
        assert_eq!(
            send(&server, &get("/file")).status_line,
            "HTTP/1.1 503 Service Unavailable"
        );
        for _ in 0..2 {
            let whole = send(&server, &get("/file"));
            assert_eq!(whole.status_line, "HTTP/1.1 200 OK");
            assert_eq!(whole.body, b"payload");
        }
        assert_eq!(server.requests().len(), 4);
    }

    #[test]
    fn an_unknown_target_is_not_found() {
        let server = LoopbackServer::start().unwrap();
        server.route("/known", Reply::ok(&b"known"[..]));
        for target in ["/unknown", "/known?query", "/Known", "/known/"] {
            let answer = send(&server, &get(target));
            assert_eq!(answer.status_line, "HTTP/1.1 404 Not Found", "{target}");
            assert_eq!(answer.header("Content-Length"), Some("0"), "{target}");
            assert!(answer.body.is_empty(), "{target}");
        }
        let targets: Vec<String> = server
            .requests()
            .into_iter()
            .map(|request| request.target)
            .collect();
        assert_eq!(targets, ["/unknown", "/known?query", "/Known", "/known/"]);
    }

    #[test]
    fn a_cut_body_closes_early() {
        let server = LoopbackServer::start().unwrap();
        let body = vec![b'x'; 1000];
        server.route("/download", Reply::cut(body.clone(), 4096));
        let answer = send(&server, &get("/download"));
        assert_eq!(answer.status_line, "HTTP/1.1 200 OK");
        assert_eq!(answer.header("Content-Length"), Some("4096"));
        assert_eq!(answer.body, body);
    }

    #[test]
    #[should_panic(expected = "promises more than its 3 bytes")]
    fn a_cut_body_must_promise_more_than_it_holds() {
        let _ = Reply::cut(&b"all"[..], 3);
    }

    #[test]
    fn a_head_the_server_cannot_read_is_refused_and_not_kept() {
        let server = LoopbackServer::start().unwrap();
        server.route("/", Reply::ok(&b"root"[..]));
        let long = format!(
            "GET / HTTP/1.1\r\nX-Padding: {}\r\n\r\n",
            "p".repeat(MAX_REQUEST_HEAD)
        );
        let fits = format!(
            "GET / HTTP/1.1\r\nX-Padding: {}\r\n\r\n",
            "p".repeat(MAX_REQUEST_HEAD - "GET / HTTP/1.1\r\nX-Padding: \r\n\r\n".len())
        );
        assert_eq!(fits.len(), MAX_REQUEST_HEAD);
        for request in [
            long.as_str(),
            "GET /\r\n\r\n",
            "GET / HTTP/1.1 extra\r\n\r\n",
            "GET / FTP/1.0\r\n\r\n",
            "GET  HTTP/1.1\r\n\r\n",
            "GET / HTTP/1.1\r\nNo colon here\r\n\r\n",
            "GET / HTTP/1.1\r\nBad Name: value\r\n\r\n",
        ] {
            let answer = send(&server, request.as_bytes());
            assert_eq!(
                answer.status_line,
                "HTTP/1.1 400 Bad Request",
                "{}",
                &request[..request.len().min(40)]
            );
        }
        assert!(server.requests().is_empty());
        assert_eq!(send(&server, fits.as_bytes()).body, b"root");
        assert_eq!(server.requests().len(), 1);
    }
}
