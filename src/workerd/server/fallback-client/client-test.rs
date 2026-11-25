// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::io::BufRead as _;
use std::io::BufReader;
use std::io::Read as _;
use std::io::Write as _;
use std::net::TcpListener;
use std::net::TcpStream;
use std::sync::Arc;
use std::sync::Mutex;
use std::thread::JoinHandle;

use workerd_capnp::worker::module;

use super::*;
use crate::protocol::ImportType;
use crate::protocol::Version;

/// A request the test service received, as it came off the wire.
#[derive(Debug, Clone)]
struct Seen {
    /// Which connection, counted from 0, and which request on it.
    connection: usize,
    request_on_connection: usize,
    request_line: String,
    /// Header lines, as sent.
    headers: Vec<String>,
    body: Vec<u8>,
}

impl Seen {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find_map(|line| {
            let (key, value) = line.split_once(':')?;
            key.eq_ignore_ascii_case(name).then(|| value.trim())
        })
    }

    fn target(&self) -> &str {
        self.request_line.split(' ').nth(1).unwrap()
    }

    fn json(&self) -> serde_json::Value {
        serde_json::from_slice(&self.body).unwrap()
    }
}

/// What the test service does with a request.
enum Action {
    /// Answers with this status, headers and body.
    Respond(u16, Vec<(&'static str, String)>, Vec<u8>),
    /// Closes the connection without answering.
    Drop,
    /// Answers with bytes that are not HTTP.
    Garbage,
    /// Answers with these bytes, which must be a whole response.
    Raw(Vec<u8>),
}

fn ok(body: &str) -> Action {
    Action::Respond(200, vec![], body.as_bytes().to_vec())
}

/// An HTTP/1.1 service on a thread of its own, written by hand so that it can misbehave.
struct TestService {
    address: String,
    seen: Arc<Mutex<Vec<Seen>>>,
    thread: Option<JoinHandle<()>>,
}

impl TestService {
    fn start(action: impl Fn(&Seen) -> Action + Send + Sync + 'static) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap().to_string();
        let seen = Arc::new(Mutex::new(Vec::new()));
        let thread = {
            let seen = Arc::clone(&seen);
            std::thread::spawn(move || {
                // Connections are served one at a time, as the client makes them.
                for (connection, stream) in listener.incoming().enumerate() {
                    let Ok(stream) = stream else { return };
                    if serve_connection(stream, connection, &seen, &action).is_err() {
                        // The test is over: `Drop` connected to say so.
                        return;
                    }
                }
            })
        };
        Self {
            address,
            seen,
            thread: Some(thread),
        }
    }

    fn seen(&self) -> Vec<Seen> {
        self.seen.lock().unwrap().clone()
    }

    fn connections(&self) -> usize {
        self.seen()
            .iter()
            .map(|s| s.connection + 1)
            .max()
            .unwrap_or(0)
    }
}

impl Drop for TestService {
    fn drop(&mut self) {
        // A connection that sends "STOP" ends the accept loop.
        if let Ok(mut stream) = TcpStream::connect(&self.address) {
            let _ = stream.write_all(b"STOP\r\n\r\n");
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// Serves requests on one connection until the client closes it or an action closes it. `Err`
/// when told to stop.
fn serve_connection(
    stream: TcpStream,
    connection: usize,
    seen: &Mutex<Vec<Seen>>,
    action: &dyn Fn(&Seen) -> Action,
) -> Result<(), ()> {
    let mut reader = BufReader::new(stream.try_clone().unwrap());
    let mut writer = stream;
    for request_on_connection in 0.. {
        let mut request_line = String::new();
        if reader.read_line(&mut request_line).unwrap_or(0) == 0 {
            return Ok(());
        }
        let request_line = request_line.trim_end().to_owned();
        if request_line == "STOP" {
            return Err(());
        }
        let mut headers = Vec::new();
        loop {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            let line = line.trim_end().to_owned();
            if line.is_empty() {
                break;
            }
            headers.push(line);
        }
        let mut request = Seen {
            connection,
            request_on_connection,
            request_line,
            headers,
            body: Vec::new(),
        };
        if let Some(length) = request.header("content-length") {
            let mut body = vec![0; length.parse().unwrap()];
            reader.read_exact(&mut body).unwrap();
            request.body = body;
        }
        let action = action(&request);
        seen.lock().unwrap().push(request);
        match action {
            Action::Respond(status, headers, body) => {
                let mut response = format!("HTTP/1.1 {status} X\r\n");
                for (name, value) in headers {
                    response.push_str(&format!("{name}: {value}\r\n"));
                }
                response.push_str(&format!("Content-Length: {}\r\n\r\n", body.len()));
                writer.write_all(response.as_bytes()).unwrap();
                writer.write_all(&body).unwrap();
            }
            Action::Drop => return Ok(()),
            Action::Garbage => {
                writer.write_all(b"this is not HTTP\r\n\r\n").unwrap();
                return Ok(());
            }
            Action::Raw(response) => writer.write_all(&response).unwrap(),
        }
    }
    Ok(())
}

/// A client whose worker runs on a thread of its own, joined when the client is dropped.
struct TestClient {
    client: Option<Client>,
    worker: Option<JoinHandle<()>>,
}

impl TestClient {
    fn new(address: &str) -> Self {
        let (client, worker) = new(address.to_owned());
        Self {
            client: Some(client),
            worker: Some(std::thread::spawn(move || worker.run())),
        }
    }

    fn resolve(&self, request: Request) -> Resolution {
        self.client.as_ref().unwrap().resolve(request)
    }
}

impl Drop for TestClient {
    fn drop(&mut self) {
        self.client = None;
        if let Some(worker) = self.worker.take() {
            worker.join().unwrap();
        }
    }
}

fn request(version: Version, import_type: ImportType, specifier: &str, raw: &str) -> Request {
    Request {
        version,
        import_type,
        specifier: specifier.as_bytes().to_vec(),
        raw_specifier: (!raw.is_empty()).then(|| raw.as_bytes().to_vec()),
        referrer: if version == Version::V1 {
            b"/worker.js".to_vec()
        } else {
            b"file:///bundle/worker.js".to_vec()
        },
        attributes: vec![(b"type".to_vec(), b"json".to_vec())],
    }
}

fn v2(specifier: &str) -> Request {
    request(Version::V2, ImportType::Import, specifier, "")
}

fn with_module<R>(resolution: &Resolution, f: impl FnOnce(module::Reader<'_>) -> R) -> R {
    let Resolution::Module(bytes) = resolution else {
        panic!("expected a module, got {resolution:?}");
    };
    let mut slice = bytes.as_slice();
    let message = capnp::serialize::read_message_from_flat_slice(
        &mut slice,
        capnp::message::ReaderOptions::new(),
    )
    .unwrap();
    f(message.get_root::<module::Reader<'_>>().unwrap())
}

/// The cases of the C++ client's test, against one service and one client.
#[test]
fn implements_the_v1_and_v2_protocols() {
    let service = TestService::start(|seen| {
        if seen.target().starts_with("/?") {
            return ok(r#"{"esModule":"export default 1;"}"#);
        }
        match seen.json()["specifier"].as_str().unwrap() {
            "file:///bundle/text.txt" => {
                ok(r#"{"name":"file:///bundle/text.txt","text":"fallback text"}"#)
            }
            "file:///bundle/redirect.js" => Action::Respond(
                301,
                vec![("Location", "file:///bundle/redirected.js".to_owned())],
                Vec::new(),
            ),
            "file:///bundle/malformed.js" => ok("{"),
            "file:///bundle/data.bin" => ok(r#"{"data":[1,2,3]}"#),
            "file:///bundle/empty.js" => ok(""),
            "file:///bundle/no-location.js" => Action::Respond(301, vec![], Vec::new()),
            _ => Action::Respond(404, vec![], b"not found".to_vec()),
        }
    });
    let client = TestClient::new(&service.address);

    let resolution = client.resolve(request(
        Version::V1,
        ImportType::Require,
        "/module.js",
        "./module.js",
    ));
    with_module(&resolution, |m| {
        assert_eq!(m.get_name().unwrap(), "module.js");
        assert!(matches!(m.which().unwrap(), module::EsModule(Ok(s)) if s == "export default 1;"));
    });

    let resolution = client.resolve(request(
        Version::V2,
        ImportType::Internal,
        "file:///bundle/text.txt",
        "./text.txt",
    ));
    with_module(&resolution, |m| {
        assert_eq!(m.get_name().unwrap(), "file:///bundle/text.txt");
        assert!(matches!(m.which().unwrap(), module::Text(Ok(s)) if s == "fallback text"));
    });

    assert_eq!(
        client.resolve(v2("file:///bundle/redirect.js")),
        Resolution::Redirect(b"file:///bundle/redirected.js".to_vec())
    );
    for missing in [
        "file:///bundle/missing.js",
        "file:///bundle/malformed.js",
        "file:///bundle/empty.js",
        "file:///bundle/no-location.js",
    ] {
        assert_eq!(
            client.resolve(v2(missing)),
            Resolution::NotFound,
            "{missing}"
        );
    }

    let resolution = client.resolve(v2("file:///bundle/data.bin"));
    with_module(&resolution, |m| {
        assert_eq!(m.get_name().unwrap(), "file:///bundle/data.bin");
        assert!(matches!(m.which().unwrap(), module::Data(Ok(d)) if d == [1, 2, 3]));
    });

    let seen = service.seen();
    assert_eq!(
        seen[0].request_line,
        "GET /?specifier=%2Fmodule.js&referrer=%2Fworker.js&rawSpecifier=.%2Fmodule.js HTTP/1.1"
    );
    assert_eq!(seen[0].header("host"), Some("localhost"));
    assert_eq!(seen[0].header("x-resolve-method"), Some("require"));
    // kj sends a GET with no body framing at all.
    assert_eq!(seen[0].header("content-length"), None);
    assert_eq!(seen[0].header("transfer-encoding"), None);

    assert_eq!(seen[1].request_line, "POST / HTTP/1.1");
    assert_eq!(seen[1].header("host"), Some("localhost"));
    assert_eq!(seen[1].header("x-resolve-method"), None);
    assert_eq!(
        seen[1].header("content-length"),
        Some(seen[1].body.len().to_string().as_str())
    );
    assert_eq!(
        String::from_utf8(seen[1].body.clone()).unwrap(),
        r#"{"type":"internal","specifier":"file:///bundle/text.txt","rawSpecifier":"./text.txt","referrer":"file:///bundle/worker.js","attributes":[{"name":"type","value":"json"}]}"#
    );

    // Every request, including those answered with an error, went over one connection.
    assert_eq!(service.connections(), 1);
}

#[test]
fn bytes_pass_through_unchanged() {
    let service = TestService::start(|_| {
        Action::Raw(b"HTTP/1.1 301 X\r\nLocation: to-\xfe\r\nContent-Length: 0\r\n\r\n".to_vec())
    });
    let client = TestClient::new(&service.address);
    let mut req = v2("file:///\u{e9}");
    req.specifier.push(0xff);
    req.referrer = b"a\"\\\n\x01\x7f".to_vec();
    let resolution = client.resolve(req);
    assert_eq!(resolution, Resolution::Redirect(b"to-\xfe".to_vec()));
    let body = service.seen()[0].body.clone();
    assert_eq!(
        body,
        b"{\"type\":\"import\",\"specifier\":\"file:///\xc3\xa9\xff\",\"referrer\":\"a\\\"\\\\\\n\\u0001\x7f\",\"attributes\":[{\"name\":\"type\",\"value\":\"json\"}]}"
    );
}

#[test]
fn reuses_one_connection_and_reconnects_when_the_service_closes_it() {
    let service = TestService::start(|seen| {
        if seen.request_on_connection == 0 {
            Action::Respond(
                200,
                vec![("Connection", "close".to_owned())],
                br#"{"text":"t"}"#.to_vec(),
            )
        } else {
            ok(r#"{"text":"t"}"#)
        }
    });
    let client = TestClient::new(&service.address);
    for _ in 0..3 {
        let resolution = client.resolve(v2("a"));
        assert!(
            matches!(resolution, Resolution::Module(_)),
            "{resolution:?}"
        );
    }
    assert_eq!(service.connections(), 3);
}

#[test]
fn retries_a_kept_connection_the_service_dropped() {
    // The second request arrives on the connection the first was answered on; the service drops
    // it without answering, and the client asks again on a new connection.
    let service = TestService::start(|seen| match (seen.connection, seen.request_on_connection) {
        (0, 1) => Action::Drop,
        _ => ok(r#"{"text":"t"}"#),
    });
    let client = TestClient::new(&service.address);
    for _ in 0..2 {
        let resolution = client.resolve(v2("a"));
        assert!(
            matches!(resolution, Resolution::Module(_)),
            "{resolution:?}"
        );
    }
    let seen = service.seen();
    assert_eq!(seen.len(), 3);
    assert_eq!((seen[2].connection, seen[2].request_on_connection), (1, 0));
}

#[test]
fn retries_a_new_connection_the_service_dropped() {
    let service = TestService::start(|seen| match seen.connection {
        0 => Action::Drop,
        _ => ok(r#"{"text":"t"}"#),
    });
    let client = TestClient::new(&service.address);
    let resolution = client.resolve(v2("a"));
    assert!(
        matches!(resolution, Resolution::Module(_)),
        "{resolution:?}"
    );
    assert_eq!(service.connections(), 2);
}

#[test]
fn retries_once_after_any_failure() {
    // A malformed response is not a disconnect, but it is retried all the same; the second
    // failure ends the request.
    let service = TestService::start(|_| Action::Garbage);
    let client = TestClient::new(&service.address);
    assert_eq!(client.resolve(v2("a")), Resolution::NotFound);
    assert_eq!(service.seen().len(), 2);
}

#[test]
fn a_refused_connection_is_a_disconnect() {
    // Bind and drop a listener, so that its port is very likely closed.
    let address = TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap();
    let e = TcpStream::connect(address).unwrap_err();
    assert!(FetchError::Io(e).is_disconnected());
}

#[test]
fn an_unreachable_service_resolves_nothing() {
    let address = TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .to_string();
    let client = TestClient::new(&address);
    assert_eq!(client.resolve(v2("a")), Resolution::NotFound);
}

#[test]
fn an_unparsable_address_shuts_the_client_down() {
    let client = TestClient::new("[::1");
    // The worker may not have exited yet when the first request is sent; once it has, every
    // request reports that it is gone.
    let first = client.resolve(v2("a"));
    assert!(
        matches!(first, Resolution::NotFound | Resolution::ShutDown),
        "{first:?}"
    );
    std::thread::sleep(std::time::Duration::from_millis(100));
    assert_eq!(client.resolve(v2("a")), Resolution::ShutDown);
}

#[cfg(unix)]
#[test]
fn connects_to_a_unix_socket() {
    let dir = std::env::temp_dir().join(format!("fallback-client-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    let path = dir.join("service.sock");
    let _ = std::fs::remove_file(&path);
    let listener = std::os::unix::net::UnixListener::bind(&path).unwrap();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        let mut length = 0;
        loop {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            if let Some(value) = line.to_ascii_lowercase().strip_prefix("content-length:") {
                length = value.trim().parse().unwrap();
            }
            if line == "\r\n" {
                break;
            }
        }
        let mut body = vec![0; length];
        reader.read_exact(&mut body).unwrap();
        let answer = br#"{"text":"t"}"#;
        write!(
            stream,
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n",
            answer.len()
        )
        .unwrap();
        stream.write_all(answer).unwrap();
    });
    let client = TestClient::new(&format!("unix:{}", path.display()));
    let resolution = client.resolve(v2("a"));
    assert!(
        matches!(resolution, Resolution::Module(_)),
        "{resolution:?}"
    );
    drop(client);
    server.join().unwrap();
    let _ = std::fs::remove_dir_all(&dir);
}
