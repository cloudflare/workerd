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
        etag: String::new(),
        generation: String::new(),
        date: String::new(),
        body: long.into_bytes(),
    };
    let Err(Error::Http(message)) = written("k", &response, false) else {
        panic!("a 500 was accepted");
    };
    assert_eq!(message, format!("PUT k: 500 {}", "x".repeat(100)));
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
        other => other.to_string(),
    })
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
        let bucket = Bucket::new(s3, &prefix);
        let wire = (("etag", "\"e\""), "x-amz-meta-", true);
        end_to_end::<SigV4>(bucket, &server, &queue, wire).await;
    });
}

#[test]
fn an_anonymous_cloud_storage_emulator_end_to_end() {
    runtime().block_on(async {
        let (server, queue) = fake().await;
        let host = server.host.clone();
        let env = move |name: &str| (name == "STORAGE_EMULATOR_HOST").then(|| host.clone());
        let (gcs, prefix) = Gcs::with_env("gs://b", &[], &env).unwrap();
        let bucket = Bucket::new(gcs, &prefix);
        let wire = (("x-goog-generation", "17"), "x-goog-meta-", false);
        end_to_end::<Google>(bucket, &server, &queue, wire).await;
    });
}
