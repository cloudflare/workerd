// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpListener;

use super::*;
use crate::fake_http::Answer;
use crate::fake_http::Server;
use crate::fake_http::reply;

#[test]
fn a_body_held_in_memory_is_capped() {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    rt.block_on(async {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let _ = stream.read(&mut [0; 4096]).await;
            let size = MAX_BODY + 1;
            let head = format!("HTTP/1.1 200 OK\r\ncontent-length: {size}\r\n\r\n");
            stream.write_all(head.as_bytes()).await.unwrap();
            let _ = stream.write_all(&vec![0; size]).await;
        });
        let http = Http::new().unwrap();
        let wait = Duration::from_secs(10);
        let url = format!("http://127.0.0.1:{port}/");
        let payload = Payload::Bytes(bytes::Bytes::new());
        let sent = http.send("GET".into(), url, vec![], payload, None, (wait, wait));
        assert_eq!(sent.await.err().as_deref(), Some("response body too large"));
    });
}

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

/// A server that answers each request as `answer` says, given its number,
/// from 1.
async fn serving(answer: impl Fn(usize) -> Answer + Send + Sync + 'static) -> Server {
    Server::serving(move |_, n| answer(n)).await
}

/// `answer`, `ms` late.
fn after(ms: u64, answer: Answer) -> Answer {
    Answer::After(Duration::from_millis(ms), Box::new(answer))
}

fn get(http: &Http, server: &Server) -> impl Future<Output = Result<HttpResponse, String>> {
    let wait = Duration::from_secs(30);
    let url = format!("http://{}/k", server.host);
    let payload = Payload::Bytes(bytes::Bytes::new());
    http.send("GET".into(), url, vec![], payload, None, (wait, wait))
}

/// Waits for request `n` to be dropped unanswered, its connection closed.
async fn abandoned(server: &Server, n: usize) {
    let abandoned = tokio::time::timeout(Duration::from_secs(30), server.abandoned(n));
    abandoned.await.unwrap();
}

const EMPTY: Payload = Payload::Bytes(bytes::Bytes::new());

/// What a scope that sends anything twice names.
const ANY: &[Repeat] = &[Repeat::Read, Repeat::Delete, Repeat::GuardedWrite];

fn get_call(empty: &Payload) -> Call<'_> {
    Call {
        method: "GET",
        guarded: false,
        body: empty,
        to: None,
    }
}

fn what() -> String {
    "GET k".to_owned()
}

/// Every answer settles.
fn settles(_: &HttpResponse) -> bool {
    true
}

/// A failed guard waits for the other copy's answer.
fn not_refused(response: &HttpResponse) -> bool {
    response.status != 412
}

#[test]
fn a_request_with_no_answer_is_sent_again_on_another_connection() {
    runtime().block_on(async {
        let server = serving(|n| {
            if n == 1 {
                Answer::Stall
            } else {
                reply(200, "ok")
            }
        })
        .await;
        let http = Http::new().unwrap();
        let empty = EMPTY;
        let call = get_call(&empty);
        let answer = prompt(
            Duration::from_millis(100),
            ANY,
            attempt(&call, what, settles, || get(&http, &server)),
        );
        let answer = answer.await;
        assert!(answer.sent_twice);
        assert_eq!(answer.answer.unwrap().body, b"ok");
        assert_eq!(server.connections(), 2);
        // The stalled request's connection is closed, not pooled.
        abandoned(&server, 1).await;
    });
}

#[test]
fn a_request_that_answers_in_time_goes_once() {
    runtime().block_on(async {
        let server = serving(|_| reply(412, "")).await;
        let http = Http::new().unwrap();
        let empty = EMPTY;
        let call = get_call(&empty);
        let answer = prompt(
            Duration::from_millis(500),
            ANY,
            attempt(&call, what, not_refused, || get(&http, &server)),
        );
        let answer = answer.await;
        assert_eq!(answer.answer.unwrap().status, 412);
        assert!(!answer.sent_twice);
        assert_eq!(server.requests(), 1);
    });
}

#[test]
fn a_request_that_answers_late_is_not_abandoned() {
    runtime().block_on(async {
        let server = serving(|n| {
            if n == 1 {
                after(300, reply(201, ""))
            } else {
                Answer::Stall
            }
        })
        .await;
        let http = Http::new().unwrap();
        let empty = EMPTY;
        let call = get_call(&empty);
        let answer = prompt(
            Duration::from_millis(100),
            ANY,
            attempt(&call, what, settles, || get(&http, &server)),
        );
        let answer = answer.await;
        assert!(answer.sent_twice);
        assert_eq!(answer.answer.unwrap().status, 201);
        assert_eq!(server.connections(), 2);
        // The copy sent again lost, and its connection is closed.
        abandoned(&server, 2).await;
    });
}

#[test]
fn an_answer_that_does_not_settle_waits_for_the_other_copy() {
    runtime().block_on(async {
        // Sent again at 0.5 s and refused at once; the first answers at
        // 0.75 s.
        let server = serving(|n| match n {
            1 => after(750, reply(201, "")),
            _ => reply(412, ""),
        })
        .await;
        let http = Http::new().unwrap();
        let empty = EMPTY;
        let call = get_call(&empty);
        let answer = prompt(
            Duration::from_millis(500),
            ANY,
            attempt(&call, what, not_refused, || get(&http, &server)),
        );
        assert_eq!(answer.await.answer.unwrap().status, 201);
    });
}

#[test]
fn an_answer_that_does_not_settle_waits_for_the_other_copy_one_bound() {
    runtime().block_on(async {
        let server = serving(|n| match n {
            1 => Answer::Stall,
            _ => reply(412, ""),
        })
        .await;
        let http = Http::new().unwrap();
        let empty = EMPTY;
        let call = get_call(&empty);
        let answer = prompt(
            Duration::from_millis(100),
            ANY,
            attempt(&call, what, not_refused, || get(&http, &server)),
        );
        // Well before the stalled request's own 30 s are up.
        let answer = tokio::time::timeout(Duration::from_secs(10), answer);
        let answer = answer.await.unwrap();
        assert_eq!(answer.answer.unwrap().status, 412);
        // The refusal answers the copy sent again, and the first, whose
        // answer never came, may yet take effect.
        assert!(answer.sent_twice);
        abandoned(&server, 1).await;
    });
}

#[test]
fn a_request_is_sent_again_only_once() {
    runtime().block_on(async {
        let server = serving(|_| Answer::Stall).await;
        let http = Http::new().unwrap();
        let empty = EMPTY;
        let call = get_call(&empty);
        let answer = prompt(
            Duration::from_millis(50),
            ANY,
            attempt(&call, what, settles, || get(&http, &server)),
        );
        // The caller's deadline ends it, as it ends a request sent once.
        let deadline = tokio::time::timeout(Duration::from_secs(2), answer);
        assert!(deadline.await.is_err());
        assert_eq!(server.requests(), 2);
    });
}

#[test]
fn a_request_outside_prompt_waits_for_its_answer() {
    runtime().block_on(async {
        let server = serving(|n| {
            if n == 1 {
                Answer::Stall
            } else {
                reply(200, "ok")
            }
        })
        .await;
        let http = Http::new().unwrap();
        let empty = EMPTY;
        let call = get_call(&empty);
        let answer = tokio::time::timeout(
            Duration::from_millis(500),
            attempt(&call, what, settles, || get(&http, &server)),
        );
        assert!(answer.await.is_err());
        assert_eq!(server.requests(), 1);
    });
}

#[test]
fn only_a_request_safe_to_send_twice_is_sent_again() {
    let empty = EMPTY;
    let small = Payload::Bytes(vec![0; 100].into());
    let large = Payload::Bytes(vec![0; 1 << 20].into());
    let file = Payload::File("record".into());
    let to = Some(Path::new("record"));
    let call = |method, guarded, body, to| Call {
        method,
        guarded,
        body,
        to,
    };
    let cases = [
        (call("GET", false, &empty, None), Some(Repeat::Read)),
        (call("DELETE", false, &empty, None), Some(Repeat::Delete)),
        (call("PUT", true, &small, None), Some(Repeat::GuardedWrite)),
        // A download into a file.
        (call("GET", false, &empty, to), None),
        (call("PUT", false, &small, None), None),
        // Uploads, whose own bound grows with their size.
        (call("PUT", true, &large, None), None),
        (call("PUT", true, &file, None), None),
        (call("POST", true, &small, None), None),
    ];
    for (call, repeat) in cases {
        assert_eq!(call.repeat(), repeat, "{} {}", call.method, call.guarded);
    }
    // One that is not, or that the scope does not name, waits for its
    // answer, whatever `prompt` bounds.
    let delete = call("DELETE", false, &empty, None);
    let unguarded = call("PUT", false, &small, None);
    for (call, repeat) in [(&unguarded, ANY), (&delete, &[Repeat::Read][..])] {
        runtime().block_on(async {
            let server = serving(|n| {
                if n == 1 {
                    Answer::Stall
                } else {
                    reply(200, "ok")
                }
            })
            .await;
            let http = Http::new().unwrap();
            let answer = prompt(
                Duration::from_millis(50),
                repeat,
                attempt(call, what, settles, || get(&http, &server)),
            );
            let deadline = tokio::time::timeout(Duration::from_millis(500), answer);
            assert!(deadline.await.is_err());
            assert_eq!(server.requests(), 1, "{}", call.method);
        });
    }
}
