// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use std::collections::VecDeque;
use std::sync::Arc;
use std::sync::Mutex;

use super::*;
use crate::Bucket;
use crate::fake_http::Answer;
use crate::fake_http::Server;
use crate::gcs::Gcs;
use crate::gcs::Google;
use crate::s3::S3;
use crate::s3::SigV4;

#[test]
fn http_dates() {
    assert_eq!(
        http_date_ms("Mon, 12 Oct 2026 23:10:55 GMT"),
        Some(1_791_846_655_000)
    );
    assert_eq!(
        http_date_ms("Tue, 29 Feb 2000 00:00:00 GMT"),
        Some(951_782_400_000)
    );
    assert_eq!(http_date_ms("nonsense"), None);
}

#[test]
fn listing_xml() {
    let xml = "<ListBucketResult><IsTruncated>true</IsTruncated>\
        <Contents><Key>a&amp;b</Key><Size>3</Size></Contents>\
        <CommonPrefixes><Prefix>p/</Prefix></CommonPrefixes>\
        <NextContinuationToken>t</NextContinuationToken></ListBucketResult>";
    let keys: Vec<_> = elements(xml, "Contents")
        .map(|c| unescape(elements(c, "Key").next().unwrap()))
        .collect();
    assert_eq!(keys, ["a&b"]);
    for (xml, text) in [
        ("&amp;lt;&#13;&#x1F600;", "&lt;\r\u{1F600}"),
        ("a & b &bogus; &#xD800;", "a & b &bogus; &#xD800;"),
    ] {
        assert_eq!(unescape(xml), text, "{xml}");
    }
    let prefixes: Vec<_> = elements(xml, "CommonPrefixes")
        .map(|c| elements(c, "Prefix").next().unwrap())
        .collect();
    assert_eq!(prefixes, ["p/"]);
    assert_eq!(elements(xml, "NextContinuationToken").next(), Some("t"));
}

#[test]
fn sigv4_encoding() {
    assert_eq!(encode("ns/a b+c", false), "ns/a%20b%2Bc");
    assert_eq!(encode("ns/a", true), "ns%2Fa");
}

#[test]
fn listing_pages() {
    let page = |xml: &str| {
        let (mut entries, mut prefixes) = (Vec::new(), Vec::new());
        let next = listing_page("p/", xml.as_bytes(), &mut entries, &mut prefixes);
        let keys: Vec<_> = entries.into_iter().map(|e| e.key).collect();
        next.map(|next| (keys, prefixes, next))
    };
    let (keys, prefixes, next) = page(
        "<ListBucketResult><IsTruncated>true</IsTruncated>\
         <Contents><Key>p/a</Key></Contents>\
         <CommonPrefixes><Prefix>p/b/</Prefix></CommonPrefixes>\
         <NextContinuationToken>t&amp;</NextContinuationToken></ListBucketResult>",
    )
    .unwrap();
    assert_eq!(
        (keys, prefixes, next),
        (vec!["p/a".into()], vec!["p/b/".into()], Some("t&".into()))
    );
    let done = page(
        "<ListBucketResult><IsTruncated>false</IsTruncated>\
        <NextContinuationToken>t</NextContinuationToken></ListBucketResult>",
    );
    assert_eq!(done.unwrap().2, None);
    assert_eq!(
        page("<ListBucketResult></ListBucketResult>").unwrap().2,
        None
    );
    // Truncated, with nowhere to go on from: not a complete listing.
    for xml in [
        "<ListBucketResult><IsTruncated>true</IsTruncated></ListBucketResult>",
        "<ListBucketResult><IsTruncated>true</IsTruncated>\
         <NextContinuationToken></NextContinuationToken></ListBucketResult>",
    ] {
        let Err(Error::Http(message)) = page(xml) else {
            panic!("{xml} was accepted");
        };
        assert_eq!(message, "LIST p/: truncated without a continuation token");
    }
}

#[test]
fn error_codes_are_capped() {
    let long = format!("<Error><Code>{}</Code></Error>", "x".repeat(1000));
    let response = HttpResponse {
        status: 500,
        body: long.into_bytes(),
        ..HttpResponse::default()
    };
    let sent = Sent {
        response,
        maybe_applied: false,
    };
    let Err(error @ Error::Refused { .. }) = written("k", &sent, false) else {
        panic!("a 500 was accepted");
    };
    let message = format!("bucket: PUT k: 500 {}", "x".repeat(100));
    assert_eq!(error.to_string(), message);
}

// The real client against a fake bucket.

/// A fake bucket that answers each request with the next answer queued.
async fn fake() -> (Server, Arc<Mutex<VecDeque<Answer>>>) {
    let queue = Arc::new(Mutex::new(VecDeque::new()));
    let answers = queue.clone();
    let server = Server::serving(move |_, _| answers.lock().unwrap().pop_front().unwrap()).await;
    (server, queue)
}

/// The outcome of one bucket call, as a test compares it.
#[derive(Clone, Copy, Debug)]
enum Call {
    Get,
    /// Unconditional, discarding the version.
    Put,
    PutIfAbsent,
}

async fn call(bucket: &Bucket, call: Call) -> std::result::Result<String, String> {
    let outcome = match call {
        Call::Get => bucket
            .get("k")
            .await
            .map(|got| format!("{:?}", got.map(|(_, v)| v.0))),
        Call::Put => bucket
            .put("k", Vec::new(), &[])
            .await
            .map(|()| "written".into()),
        Call::PutIfAbsent => {
            let put = bucket.put_if("k", Vec::new(), Guard::Absent).await;
            put.map(|version| format!("{:?}", version.map(|v| v.0)))
        }
    };
    outcome.map_err(|e| match e {
        Error::Http(message) => message,
        Error::Refused { what, status, code } => format!("{what}: {status} {code}"),
        Error::OutcomeUnknown(message) => format!("unknown: {message}"),
        other => other.to_string(),
    })
}

/// In `retried`, the request's connection closes unanswered.
const CLOSE: u16 = 0;

/// Drives `bucket` as `end_to_end` does, through calls the bucket answers
/// only after refusing them, or closing their connections, once or more.
async fn retried(
    bucket: &Bucket,
    server: &Server,
    queue: &Mutex<VecDeque<Answer>>,
    version: (&'static str, &str),
) {
    let answer = |status| match status {
        CLOSE => Answer::Close,
        _ => Answer::Reply {
            status,
            headers: if status == 200 {
                vec![(version.0, version.1.to_owned())]
            } else {
                Vec::new()
            },
            body: String::new(),
        },
    };
    let unknown = |status| Err(format!("unknown: PUT k: {status} to a retry"));
    let written = Ok(format!("Some({:?})", version.1));
    let closed = "client error (SendRequest): connection closed before message completed";
    for (call_, statuses, expected) in [
        // A 429 or 503 may follow a write the bucket applied, and a guard
        // that fails after it may fail against that write.
        (Call::PutIfAbsent, &[503, 412][..], unknown(412)),
        (Call::PutIfAbsent, &[503, 409], unknown(409)),
        (Call::PutIfAbsent, &[429, 412], unknown(412)),
        (Call::PutIfAbsent, &[503, 429, 412], unknown(412)),
        (Call::PutIfAbsent, &[503, 200], written.clone()),
        (Call::PutIfAbsent, &[429, 200], written.clone()),
        (Call::PutIfAbsent, &[412], Ok("None".into())),
        // A write without a guard fails no guard.
        (Call::Put, &[503, 200], Ok("written".into())),
        (Call::Put, &[503, 412], Err("PUT k: 412 ".into())),
        // So may a connection that failed with the request on it, which
        // the bucket may have received whole.
        (Call::PutIfAbsent, &[CLOSE, 412], unknown(412)),
        (Call::PutIfAbsent, &[CLOSE, 503, 200], written.clone()),
        (Call::Put, &[CLOSE, 200], Ok("written".into())),
        (
            Call::Get,
            &[CLOSE, CLOSE, 200],
            Ok(format!("Some({:?})", version.1)),
        ),
        (Call::Put, &[CLOSE; 5], Err(format!("PUT k: {closed}"))),
    ] {
        queue
            .lock()
            .unwrap()
            .extend(statuses.iter().map(|status| answer(*status)));
        let outcome = call(bucket, call_).await;
        assert_eq!(outcome, expected, "{call_:?} answered {statuses:?}");
        assert_eq!(server.take().len(), statuses.len(), "{statuses:?}");
    }

    // A new connection that closed before the request went out on it: the
    // request is sent again, and a guard that fails then fails against no
    // copy of this write. Each answer closes its connection, so that the
    // next request opens one.
    let closing = |status| Answer::Reply {
        status,
        headers: vec![
            ("connection", "close".to_owned()),
            (version.0, version.1.to_owned()),
        ],
        body: String::new(),
    };
    for (call_, hang_ups, statuses, expected) in [
        (Call::PutIfAbsent, 1, &[412][..], Ok("None".into())),
        (Call::PutIfAbsent, 2, &[200], written.clone()),
        (Call::Get, 1, &[200], written.clone()),
    ] {
        server.hang_up(hang_ups);
        queue
            .lock()
            .unwrap()
            .extend(statuses.iter().map(|status| closing(*status)));
        let outcome = call(bucket, call_).await;
        assert_eq!(outcome, expected, "{call_:?} after {hang_ups} hang-ups");
        assert_eq!(server.take().len(), statuses.len(), "{call_:?}");
        // The request went out on new connections, not a pooled one.
        assert_eq!(server.hang_ups(), 0, "{call_:?}");
    }
    server.hang_up(5);
    let outcome = call(bucket, Call::Put).await;
    let message = outcome.expect_err("five hang-ups");
    assert!(message.starts_with("PUT k: client error ("), "{message}");
    assert_eq!(server.take().len(), 0);
    assert_eq!(server.hang_ups(), 0);

    // Any other failure is the answer: a body cut short after the head.
    let head = vec![(version.0, version.1.to_owned())];
    queue.lock().unwrap().push_back(Answer::Cut(head));
    let outcome = call(bucket, Call::Get).await;
    assert!(outcome.is_err(), "{outcome:?}");
    assert_eq!(server.take().len(), 1);
}

/// Drives `bucket`, the client of the fake bucket `b` at `server`, which
/// answers what `queue` holds. In its dialect `D`, a version is the header
/// `version`, user metadata headers start with `meta`, and requests carry an
/// `Authorization` header when `authorized`.
async fn end_to_end<D: Dialect>(
    bucket: Bucket,
    server: &Server,
    queue: &Mutex<VecDeque<Answer>>,
    (version, meta, authorized): ((&'static str, &str), &str, bool),
) {
    let answer = |status, versioned: bool, body: &str| Answer::Reply {
        status,
        headers: if versioned {
            vec![(version.0, version.1.to_owned())]
        } else {
            Vec::new()
        },
        body: body.to_owned(),
    };
    let unversioned = |call| format!("{call} k: 200 without a version ({})", D::VERSION);
    let mut requests = Vec::new();
    for (call_, status, versioned, expected) in [
        (Call::Get, 200, true, Ok(format!("Some({:?})", version.1))),
        (Call::Get, 200, false, Err(unversioned("GET"))),
        (Call::Put, 200, false, Err(unversioned("PUT"))),
        (Call::PutIfAbsent, 200, false, Err(unversioned("PUT"))),
        // A refused write without a guard is not a failed guard.
        (Call::Put, 412, false, Err("PUT k: 412 ".into())),
        (Call::Put, 409, false, Err("PUT k: 409 ".into())),
        (Call::PutIfAbsent, 412, false, Ok("None".into())),
        (Call::PutIfAbsent, 409, false, Ok("None".into())),
    ] {
        queue
            .lock()
            .unwrap()
            .push_back(answer(status, versioned, ""));
        let outcome = call(&bucket, call_).await;
        assert_eq!(outcome, expected, "{call_:?} answered {status}");
        requests.extend(server.take());
    }

    // User metadata goes in the dialect's headers.
    queue.lock().unwrap().push_back(answer(200, true, ""));
    bucket
        .put("k", Vec::new(), &[("owner", "n1".into())])
        .await
        .unwrap();
    let put = server.take();
    assert_eq!(put[0].header(&format!("{meta}owner")), Some("n1"));
    requests.extend(put);

    // A listing goes on from the continuation token of a truncated page.
    let page = |keys: &str, next: Option<&str>| {
        let truncated = next.map_or(String::new(), |t| {
            format!(
                "<IsTruncated>true</IsTruncated><NextContinuationToken>{t}</NextContinuationToken>"
            )
        });
        let body = format!("<ListBucketResult>{keys}{truncated}</ListBucketResult>");
        answer(200, false, &body)
    };
    queue.lock().unwrap().extend([
        page("<Contents><Key>a</Key></Contents>", Some("t&amp;1")),
        page("<Contents><Key>b</Key></Contents>", None),
    ]);
    let keys: Vec<_> = bucket
        .list("")
        .await
        .unwrap()
        .into_iter()
        .map(|e| e.key)
        .collect();
    assert_eq!(keys, ["a", "b"]);
    let pages = server.take();
    assert!(
        !pages[0].query.contains("continuation-token"),
        "{}",
        pages[0].query
    );
    assert!(
        pages[1].query.contains("continuation-token=t%261"),
        "{}",
        pages[1].query
    );
    requests.extend(pages);

    for request in requests {
        let header = request.header("authorization");
        assert_eq!(header.is_some(), authorized, "{} {header:?}", request.path);
    }
}

/// The first pause of the 429/503 backoff in the end-to-end tests, which
/// check what the retries answer and not when.
const BACKOFF: Duration = Duration::from_millis(5);

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

#[test]
fn an_s3_bucket_end_to_end() {
    runtime().block_on(async {
        let (server, queue) = fake().await;
        let options = [
            ("aws_endpoint", format!("http://{}", server.host)),
            ("aws_region", "auto".into()),
            ("aws_access_key_id", "k".into()),
            ("aws_secret_access_key", "s".into()),
            ("aws_token", "t".into()),
        ]
        .map(|(name, value)| (name.to_owned(), value));
        let (s3, prefix) = S3::new("s3://b", &options).unwrap();
        let bucket = Bucket::new(s3.backoff(BACKOFF), &prefix);
        let wire = (("etag", "\"e\""), "x-amz-meta-", true);
        retried(&bucket, &server, &queue, wire.0).await;
        end_to_end::<SigV4>(bucket, &server, &queue, wire).await;
    });
}

#[test]
fn an_anonymous_cloud_storage_emulator_end_to_end() {
    runtime().block_on(async {
        let (server, queue) = fake().await;
        let host = server.host.clone();
        let env = move |name: &str| (name == "STORAGE_EMULATOR_HOST").then(|| host.clone());
        let (gcs, prefix) = Gcs::with_env("gs://b", &[], &env, None).unwrap();
        let bucket = Bucket::new(gcs.backoff(BACKOFF), &prefix);
        let wire = (("x-goog-generation", "17"), "x-goog-meta-", false);
        retried(&bucket, &server, &queue, wire.0).await;
        end_to_end::<Google>(bucket, &server, &queue, wire).await;
    });
}

/// The bound (`http::prompt`) on a request the fake answers at once:
/// longer than a stall of a busy machine, so that it is not sent again.
/// Only a test of a stalled request waits it out.
const ATTEMPT: Duration = Duration::from_millis(600);

/// The bound in a call whose only requests under it are a stalled one and
/// its copy, which is not sent again however late it answers.
const STALLED_ATTEMPT: Duration = Duration::from_millis(50);

/// Where a client whose metadata server is the fake asks for tokens.
const TOKEN: &str = "/computeMetadata/v1/instance/service-accounts/default/token";

/// A Cloud Storage client of `server`, which is its metadata server too.
fn cloud_storage(server: &Server) -> Bucket {
    let host = server.host.clone();
    let env = move |name: &str| {
        matches!(name, "STORAGE_EMULATOR_HOST" | "GCE_METADATA_HOST").then(|| host.clone())
    };
    let (gcs, prefix) = Gcs::with_env("gs://b", &[], &env, None).unwrap();
    Bucket::new(gcs, &prefix)
}

/// A metadata server's answer with token `t<n>`.
fn token(n: usize) -> Answer {
    let body = serde_json::json!({ "access_token": format!("t{n}"), "expires_in": 3600 });
    crate::fake_http::reply(200, &body.to_string())
}

/// The authorization and precondition of each request but the metadata
/// server's.
fn object_requests(server: &Server) -> Vec<(String, Option<String>)> {
    let header =
        |request: &crate::fake_http::Request, name| request.header(name).map(str::to_owned);
    let requests = server.take().into_iter().filter(|r| r.path != TOKEN);
    let requests = requests.map(|r| {
        let authorization = header(&r, "authorization").unwrap_or_default();
        (authorization, header(&r, "x-goog-if-generation-match"))
    });
    requests.collect()
}

#[test]
fn a_stalled_cloud_storage_request_is_sent_again() {
    use crate::http::Repeat;
    use crate::http::prompt;
    runtime().block_on(async {
        // The first copy of each object request stalls; the second answers.
        let objects = Arc::new(Mutex::new(0));
        let server = Server::serving(move |request, n| {
            if request.path == TOKEN {
                return token(n);
            }
            let first = {
                let mut objects = objects.lock().unwrap();
                *objects += 1;
                *objects % 2 == 1
            };
            if first {
                return Answer::Stall;
            }
            let guarded = request.header("x-goog-if-generation-match").is_some();
            let generation = if guarded { "18" } else { "17" };
            Answer::Reply {
                status: 200,
                headers: vec![("x-goog-generation", generation.to_owned())],
                body: String::new(),
            }
        })
        .await;
        let bucket = cloud_storage(&server);
        let bound = STALLED_ATTEMPT;
        let repeat = &[Repeat::Read, Repeat::GuardedWrite];
        let bearer = || "Bearer t1".to_owned();

        let got = prompt(bound, repeat, bucket.get("k")).await.unwrap();
        assert_eq!(got.map(|(_, v)| v.0), Some("17".into()));
        // Both copies carry one authorization.
        assert_eq!(
            object_requests(&server),
            [(bearer(), None), (bearer(), None)]
        );

        // A write guarded by a generation, whose header is Cloud Storage's
        // own, is a guarded write too.
        let put = bucket.put_if("k", Vec::new(), Guard::Match(Version("17".into())));
        let put = prompt(bound, repeat, put).await.unwrap();
        assert_eq!(put.map(|v| v.0), Some("18".into()));
        let guarded = (bearer(), Some("17".to_owned()));
        assert_eq!(object_requests(&server), [guarded.clone(), guarded]);

        // An unguarded write goes once: no copy follows it within three
        // bounds of its arrival.
        let put = prompt(bound, repeat, bucket.put("k", Vec::new(), &[]));
        let (sent, answer) = server.requests_during(1, 3 * bound, put).await;
        assert!(answer.is_none(), "an unguarded write answered");
        assert_eq!(sent, 1);
        assert_eq!(object_requests(&server), [(bearer(), None)]);
    });
}

#[test]
fn a_copy_refused_its_token_is_sent_again_with_another() {
    use crate::http::Repeat;
    use crate::http::prompt;
    runtime().block_on(async {
        // Requests 1 and 4 fetch tokens. The first copy (2) stalls, the
        // second (3) is refused its token, and the request sent again with
        // the next answers.
        let server = Server::serving(move |request, n| {
            if request.path == TOKEN {
                return token(n);
            }
            match request.header("authorization") {
                Some("Bearer t1") if n == 2 => Answer::Stall,
                Some("Bearer t1") => crate::fake_http::reply(401, ""),
                _ => Answer::Reply {
                    status: 200,
                    headers: vec![("x-goog-generation", "17".to_owned())],
                    body: String::new(),
                },
            }
        })
        .await;
        let bucket = cloud_storage(&server);
        // The request sent again with the next token answers at once, so
        // the bound is one it needs.
        let get = prompt(ATTEMPT, &[Repeat::Read], bucket.get("k"));
        let got = tokio::time::timeout(Duration::from_secs(10), get).await;
        assert_eq!(got.unwrap().unwrap().map(|(_, v)| v.0), Some("17".into()));
        // The refusal ends the attempt, the stalled copy with it: the
        // request goes again once, with the next token, and answers.
        let bearer = |token: &str| (format!("Bearer {token}"), None);
        let sent = [bearer("t1"), bearer("t1"), bearer("t4")];
        assert_eq!(object_requests(&server), sent);
        let abandoned = tokio::time::timeout(Duration::from_secs(30), server.abandoned(2));
        abandoned.await.unwrap();
    });
}

#[test]
fn a_guarded_write_refused_beside_another_copy_is_of_unknown_outcome() {
    use crate::http::Repeat;
    use crate::http::prompt;
    runtime().block_on(async {
        // The first copy of a write to `stalls` or to `reauthorized` stalls.
        // The copy of `stalls` sent again is refused its guard; the one of
        // `reauthorized` is refused its token, and the write sent again with
        // the next is refused its guard. A write to `refused` is refused at
        // once.
        let seen = Arc::new(Mutex::new(std::collections::HashSet::new()));
        let server = Server::serving(move |request, n| {
            if request.path == TOKEN {
                return token(n);
            }
            if seen.lock().unwrap().insert(request.path.clone())
                && !request.path.ends_with("refused")
            {
                return Answer::Stall;
            }
            let refused = request.path.ends_with("reauthorized")
                && request.header("authorization") == Some("Bearer t1");
            crate::fake_http::reply(if refused { 401 } else { 412 }, "")
        })
        .await;
        let bucket = cloud_storage(&server);
        let guard = || Guard::Match(Version("17".into()));
        let put = |key, bound| {
            let put = bucket.put_if(key, Vec::new(), guard());
            prompt(bound, &[Repeat::GuardedWrite], put)
        };
        // The stalled copy may land yet, or have landed before the refusal,
        // which may then be of its own write.
        for key in ["stalls", "reauthorized"] {
            let outcome = tokio::time::timeout(Duration::from_secs(10), put(key, STALLED_ATTEMPT));
            let outcome = outcome.await.unwrap();
            assert!(
                matches!(outcome, Err(Error::OutcomeUnknown(_))),
                "{key}: {outcome:?}"
            );
        }
        // A write that went once failed its guard.
        assert_eq!(put("refused", ATTEMPT).await.unwrap(), None);
    });
}

/// The answer `status` with `headers` and `body`.
fn answer(status: u16, headers: &[(&'static str, &str)], body: &str) -> Answer {
    let headers = headers
        .iter()
        .map(|(name, value)| (*name, (*value).to_owned()));
    Answer::Reply {
        status,
        headers: headers.collect(),
        body: body.to_owned(),
    }
}

/// An error answer with S3's `Code`.
fn error(status: u16, code: &str) -> Answer {
    answer(status, &[], &format!("<Error><Code>{code}</Code></Error>"))
}

/// A refusal's status and code.
fn refusal<T>(result: Result<T>) -> (u16, String) {
    match result {
        Err(Error::Refused { status, code, .. }) => (status, code),
        other => panic!("not refused: {:?}", other.err()),
    }
}

/// The body of a read, read.
async fn body(body: crate::bucket::Body) -> Result<String> {
    let chunks: Vec<_> = body.collect().await;
    let chunks = chunks.into_iter().collect::<Result<Vec<_>>>()?;
    Ok(String::from_utf8(chunks.concat()).unwrap())
}

/// What the cluster test's fakes can't show of the calls R2 makes through
/// `bucket`, whose versions are the header `version`: error answers, a body
/// that stalls or is cut short, a completion sent again. Reads carry
/// `accept-encoding: gzip` if `gzip`; every request is signed if
/// `authorized`.
async fn r2_calls(
    bucket: &Bucket,
    server: &Server,
    queue: &Mutex<VecDeque<Answer>>,
    version: (&'static str, &str),
    (gzip, authorized): (bool, bool),
) {
    let push = |answers: Vec<Answer>| queue.lock().unwrap().extend(answers);
    let mut sent = Vec::new();
    // A HEAD's answer has no body, so no `Code`.
    push(vec![
        answer(404, &[], ""),
        error(403, "AccessDenied"),
        error(416, "InvalidRange"),
    ]);
    assert!(bucket.head("k").await.unwrap().is_none());
    assert_eq!(refusal(bucket.head("k").await), (403, String::new()));
    let past = Some(crate::bucket::Range::Suffix(0));
    let past = refusal(bucket.read("k", past).await);
    assert_eq!(past, (416, "InvalidRange".into()));
    let reads = server.take();
    assert_eq!(reads[2].header("range"), Some("bytes=-0"));
    for request in &reads {
        assert_eq!(request.header("accept-encoding").is_some(), gzip);
    }
    sent.extend(reads);

    // The body streams: its first bytes arrive long before its end.
    let versioned = || vec![(version.0, version.1.to_owned())];
    push(vec![Answer::Dribble(versioned())]);
    let (object, mut got) = bucket.read("k", None).await.unwrap().unwrap();
    assert_eq!(object.size, 100_000);
    let first = tokio::time::timeout(Duration::from_secs(5), got.next()).await;
    let first = first.unwrap().unwrap().unwrap();
    assert!(
        first.len() < 1000 && first.iter().all(|&b| b == b' '),
        "{first:?}"
    );
    drop(got);
    // A body cut short is an error at its end.
    push(vec![Answer::Cut(versioned())]);
    let (_, got) = bucket.read("k", None).await.unwrap().unwrap();
    let Err(Error::Http(message)) = body(got).await else {
        panic!("a short body was read");
    };
    assert!(message.starts_with("GET k: "), "{message}");
    sent.extend(server.take());

    // A completion that failed after its head went out; one sent again
    // after a refusal that may have followed its success, and found the
    // upload gone; an abort likewise, which is done.
    push(vec![
        answer(200, &[], "<Error><Code>InternalError</Code></Error>"),
        error(503, "SlowDown"),
        error(404, "NoSuchUpload"),
        error(400, "InvalidPart"),
        error(503, "SlowDown"),
        error(404, "NoSuchUpload"),
        error(404, "NoSuchUpload"),
    ]);
    let parts = [(1, "p1".to_owned())];
    let complete = || bucket.complete("k", "u", &parts);
    assert_eq!(refusal(complete().await), (200, "InternalError".into()));
    let unknown = complete().await;
    assert!(
        matches!(unknown, Err(Error::OutcomeUnknown(_))),
        "{unknown:?}"
    );
    assert_eq!(refusal(complete().await), (400, "InvalidPart".into()));
    bucket.abort("k", "u").await.unwrap();
    let gone = (404, "NoSuchUpload".to_owned());
    assert_eq!(refusal(bucket.abort("k", "u").await), gone);
    let completions = server.take();
    assert_eq!(completions.len(), 7);
    sent.extend(completions);
    for request in sent {
        let header = request.header("authorization");
        assert_eq!(header.is_some(), authorized, "{} {header:?}", request.path);
    }
}

#[test]
fn an_s3_bucket_answers_r2s_calls() {
    runtime().block_on(async {
        let (server, queue) = fake().await;
        let options = [
            ("aws_endpoint", format!("http://{}", server.host)),
            ("aws_access_key_id", "k".into()),
            ("aws_secret_access_key", "s".into()),
        ]
        .map(|(name, value)| (name.to_owned(), value));
        let (s3, prefix) = S3::new("s3://b", &options).unwrap();
        let bucket = Bucket::new(s3, &prefix);
        let version = ("etag", "\"e\"");
        r2_calls(&bucket, &server, &queue, version, (false, true)).await;
    });
}

#[test]
fn a_cloud_storage_emulator_answers_r2s_calls() {
    runtime().block_on(async {
        let (server, queue) = fake().await;
        let host = server.host.clone();
        let env = move |name: &str| (name == "STORAGE_EMULATOR_HOST").then(|| host.clone());
        let (gcs, prefix) = Gcs::with_env("gs://b", &[], &env, None).unwrap();
        let bucket = Bucket::new(gcs, &prefix);
        let version = ("x-goog-generation", "17");
        r2_calls(&bucket, &server, &queue, version, (true, false)).await;
    });
}

#[test]
fn a_stalled_read_is_sent_again_but_multipart_calls_are_not() {
    use crate::http::Repeat;
    use crate::http::prompt;
    runtime().block_on(async {
        // The first copy of a read stalls, the second answers; any other
        // request stalls.
        let server = Server::serving(move |request, n| {
            if n % 2 == 1 || !matches!(&request.method[..], "GET" | "HEAD") {
                return Answer::Stall;
            }
            // A HEAD's answer goes without the body.
            answer(200, &[("etag", "\"e\""), ("content-length", "5")], "hello")
        })
        .await;
        let options = [
            ("aws_endpoint", format!("http://{}", server.host)),
            ("aws_access_key_id", "k".into()),
            ("aws_secret_access_key", "s".into()),
        ]
        .map(|(name, value)| (name.to_owned(), value));
        let bucket = Bucket::new(S3::new("s3://b", &options).unwrap().0, "");
        let bound = Duration::from_millis(100);
        let any = &[Repeat::Read, Repeat::Delete, Repeat::GuardedWrite];

        let head = prompt(bound, any, bucket.head("k")).await.unwrap();
        assert_eq!(head.unwrap().size, 5);
        let (_, got) = prompt(bound, any, bucket.read("k", None))
            .await
            .unwrap()
            .unwrap();
        assert_eq!(body(got).await.unwrap(), "hello");
        assert_eq!(server.take().len(), 4);

        let patience = Duration::from_secs(1);
        let none = crate::bucket::Headers::default();
        let begin = prompt(bound, any, bucket.begin("k", &none, &[]));
        assert!(tokio::time::timeout(patience, begin).await.is_err());
        let part = prompt(bound, any, bucket.part("k", "u", 1, "x".into()));
        let part = tokio::time::timeout(patience, part).await;
        assert!(part.is_err(), "{part:?}");
        let complete = prompt(bound, any, bucket.complete("k", "u", &[]));
        assert!(tokio::time::timeout(patience, complete).await.is_err());
        assert_eq!(server.take().len(), 3);
    });
}
