// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;

use super::*;

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

fn api() -> (Api, Loopback) {
    let loopback = Loopback::default();
    loopback.enable();
    (Api::new("loopback:docker", &loopback), loopback)
}

/// Reads one request (head and `Content-Length` body) off `stream`.
async fn read_request(stream: &mut (impl AsyncRead + Unpin)) -> String {
    let mut buf = Vec::new();
    let mut byte = [0; 1];
    while !buf.ends_with(b"\r\n\r\n") {
        stream.read_exact(&mut byte).await.unwrap();
        buf.push(byte[0]);
    }
    let head = String::from_utf8(buf).unwrap();
    let length = head
        .lines()
        .find_map(|line| {
            let (name, value) = line.split_once(':')?;
            name.eq_ignore_ascii_case("content-length")
                .then(|| value.trim().parse::<usize>().unwrap())
        })
        .unwrap_or(0);
    let mut body = vec![0; length];
    stream.read_exact(&mut body).await.unwrap();
    head + &String::from_utf8(body).unwrap()
}

/// Serves one connection: reads a request, answers with `response`, returns the request.
async fn serve_one(loopback: &Loopback, response: &str) -> String {
    let listener = loopback.listen("docker").unwrap();
    let mut stream = listener.accept().await.unwrap();
    let request = read_request(&mut stream).await;
    stream.write_all(response.as_bytes()).await.unwrap();
    stream.shutdown().await.unwrap();
    request
}

#[test]
fn json_request_and_response() {
    let (api, loopback) = api();
    runtime().block_on(async {
        let server = serve_one(
            &loopback,
            "HTTP/1.1 201 Created\r\nContent-Length: 11\r\n\r\n{\"Id\":\"x\"}\n",
        );
        let client = api.json(
            Method::POST,
            "/containers/create?name=a",
            Some("{}".to_owned()),
        );
        let (request, response) = futures::join!(server, client);
        let response = response.unwrap();
        assert_eq!(response.status, 201);
        let created: super::super::api::ContainerCreateResponse = response.json().unwrap();
        assert_eq!(created.id, "x");
        let lower = request.to_ascii_lowercase();
        assert!(request.starts_with("POST /containers/create?name=a HTTP/1.1\r\n"));
        assert!(lower.contains("host: localhost\r\n"));
        assert!(lower.contains("content-type: application/json\r\n"));
        assert!(lower.contains("content-length: 2\r\n"));
        assert!(request.ends_with("\r\n\r\n{}"));
    });
}

#[test]
fn body_over_the_limit_fails() {
    let (api, loopback) = api();
    runtime().block_on(async {
        let server = serve_one(
            &loopback,
            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabcd",
        );
        let client = api.send(Method::GET, "/archive", None, 3);
        let (_, response) = futures::join!(server, client);
        let error = response.unwrap_err();
        assert!(
            error.description().contains("exceeded 3 bytes"),
            "{error:?}"
        );
    });
}

#[test]
fn upgrade_hijacks_the_connection() {
    let (api, loopback) = api();
    runtime().block_on(async {
        let listener = loopback.listen("docker").unwrap();
        let server = async {
            let mut stream = listener.accept().await.unwrap();
            let request = read_request(&mut stream).await;
            stream
                .write_all(
                    b"HTTP/1.1 101 UPGRADED\r\nConnection: Upgrade\r\nUpgrade: tcp\r\n\r\nearly",
                )
                .await
                .unwrap();
            let mut input = [0; 5];
            stream.read_exact(&mut input).await.unwrap();
            assert_eq!(&input, b"stdin");
            stream.write_all(b" late").await.unwrap();
            stream.shutdown().await.unwrap();
            request
        };
        let client = async {
            let Upgrade::Hijacked(mut io) =
                api.upgrade("/exec/1/start", "{}".to_owned()).await.unwrap()
            else {
                panic!("not hijacked");
            };
            io.write_all(b"stdin").await.unwrap();
            let mut output = String::new();
            io.read_to_string(&mut output).await.unwrap();
            output
        };
        let (request, output) = futures::join!(server, client);
        // Bytes that came with the response head are not lost.
        assert_eq!(output, "early late");
        let lower = request.to_ascii_lowercase();
        assert!(lower.contains("connection: upgrade\r\n"));
        assert!(lower.contains("upgrade: tcp\r\n"));
    });
}

#[test]
fn connect_tunnels_to_the_authority() {
    let (api, loopback) = api();
    runtime().block_on(async {
        let listener = loopback.listen("docker").unwrap();
        let server = async {
            let mut stream = listener.accept().await.unwrap();
            let request = read_request(&mut stream).await;
            stream
                .write_all(b"HTTP/1.1 200 OK\r\n\r\nearly")
                .await
                .unwrap();
            let mut input = [0; 4];
            stream.read_exact(&mut input).await.unwrap();
            assert_eq!(&input, b"ping");
            stream.write_all(b" pong").await.unwrap();
            stream.shutdown().await.unwrap();
            request
        };
        let client = async {
            let Upgrade::Hijacked(mut io) = api
                .connect("10.0.0.2:8080", &[("X-Dst-Addr", "10.0.0.2:8080")])
                .await
                .unwrap()
            else {
                panic!("not hijacked");
            };
            io.write_all(b"ping").await.unwrap();
            let mut output = String::new();
            io.read_to_string(&mut output).await.unwrap();
            output
        };
        let (request, output) = futures::join!(server, client);
        assert_eq!(output, "early pong");
        assert!(request.starts_with("CONNECT 10.0.0.2:8080 HTTP/1.1\r\n"));
        let lower = request.to_ascii_lowercase();
        assert!(lower.contains("host: 10.0.0.2:8080\r\n"));
        assert!(lower.contains("x-dst-addr: 10.0.0.2:8080\r\n"));
    });
}

#[test]
fn refused_connect_reports_the_status_and_body() {
    let (api, loopback) = api();
    runtime().block_on(async {
        let server = serve_one(
            &loopback,
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 13\r\n\r\nnot listening",
        );
        let client = api.connect("10.0.0.2:8080", &[]);
        let (_, upgrade) = futures::join!(server, client);
        let Upgrade::Refused {
            status,
            status_text,
            body,
        } = upgrade.unwrap()
        else {
            panic!("hijacked");
        };
        assert_eq!(status, 400);
        assert_eq!(status_text, "Bad Request");
        assert_eq!(body, "not listening");
    });
}

#[test]
fn refused_upgrade_reports_the_status() {
    let (api, loopback) = api();
    runtime().block_on(async {
        let server = serve_one(
            &loopback,
            "HTTP/1.1 404 No Such Exec\r\nContent-Length: 7\r\n\r\nmissing",
        );
        let client = api.upgrade("/exec/1/start", "{}".to_owned());
        let (_, upgrade) = futures::join!(server, client);
        let Upgrade::Refused {
            status,
            status_text,
            body,
        } = upgrade.unwrap()
        else {
            panic!("hijacked");
        };
        assert_eq!(status, 404);
        assert_eq!(status_text, "No Such Exec");
        assert_eq!(body, "missing");
    });
}

#[test]
fn unreachable_address_fails() {
    // A registry that is not enabled refuses every name.
    let api = Api::new("loopback:docker", &Loopback::default());
    runtime().block_on(async {
        assert!(api.get("/version").await.is_err());
    });
}
