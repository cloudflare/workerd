// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use super::*;
use crate::http::Repeat;

fn block_on<F: std::future::Future>(f: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap()
        .block_on(f)
}

#[test]
fn a_renewal_whose_answer_was_lost_is_not_a_fence() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        a.renew(0).await.unwrap();
        // The renewal at 1_000 lands, but its answer never arrives.
        let landed = NodeRecord {
            node: "a".into(),
            address: "a:1".into(),
            expires_ms: 11_000,
            instance: a.instance,
            draining: false,
            write: 0,
        };
        let body = serde_json::to_vec(&landed).unwrap();
        bucket.put(&node_key("a"), body, &[]).await.unwrap();
        assert_eq!(a.renew(2_000).await.unwrap(), 12_000);
        // Someone else's write is still a fence.
        let other = NodeRecord {
            instance: a.instance + 1,
            ..landed
        };
        let body = serde_json::to_vec(&other).unwrap();
        bucket.put(&node_key("a"), body, &[]).await.unwrap();
        assert!(matches!(a.renew(3_000).await, Err(Error::Fenced(_))));
    });
}

/// A renewal the store refused where the refusal may be of its own first
/// attempt: the reread decides, as it does after any refusal.
#[test]
fn a_renewal_of_unknown_outcome_rereads_the_record() {
    block_on(async {
        let store = crate::bucket::memory::Memory::default();
        let unknown = store.unknown.clone();
        let bucket = Bucket::new(store, "");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        a.renew(0).await.unwrap();
        // The renewal's write landed.
        unknown.borrow_mut().push(node_key("a"));
        assert_eq!(a.renew(1_000).await.unwrap(), 11_000);
        assert!(unknown.borrow().is_empty());
        let (body, version) = bucket.get(&node_key("a")).await.unwrap().unwrap();
        let record: NodeRecord = serde_json::from_slice(&body).unwrap();
        assert_eq!(record.expires_ms, 11_000);
        assert_eq!(a.own.borrow().as_ref(), Some(&version));
        // Another writer's record is still a fence.
        let other = NodeRecord {
            instance: a.instance + 1,
            ..record
        };
        let body = serde_json::to_vec(&other).unwrap();
        bucket.put(&node_key("a"), body, &[]).await.unwrap();
        unknown.borrow_mut().push(node_key("a"));
        assert!(matches!(a.renew(2_000).await, Err(Error::Fenced(_))));
    });
}

#[test]
fn a_deregistration_of_unknown_outcome_rereads_the_record() {
    block_on(async {
        let store = crate::bucket::memory::Memory::default();
        let unknown = store.unknown.clone();
        let bucket = Bucket::new(store, "");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        a.renew(0).await.unwrap();
        unknown.borrow_mut().push(node_key("a"));
        a.deregister().await.unwrap();
        assert!(unknown.borrow().is_empty());
        let (body, _) = bucket.get(&node_key("a")).await.unwrap().unwrap();
        let record: NodeRecord = serde_json::from_slice(&body).unwrap();
        assert_eq!((record.expires_ms, record.draining), (0, true));
    });
}

/// A write's key and body.
type Write = (String, Vec<u8>);

/// Lands `late`, a write of ours a timed-out round left in flight, just
/// after serving the next GET.
#[derive(Default)]
struct Late {
    inner: crate::bucket::memory::Memory,
    late: std::rc::Rc<RefCell<Option<Write>>>,
}

#[async_trait::async_trait(?Send)]
impl crate::bucket::Store for Late {
    async fn get(
        &self,
        key: &str,
        to: Option<&std::path::Path>,
    ) -> Result<Option<(Vec<u8>, Version)>> {
        let got = self.inner.get(key, to).await;
        if let Some((key, body)) = self.late.take() {
            let body = crate::bucket::Payload::Bytes(body.into());
            self.inner.put(&key, body, None, &[]).await?;
        }
        got
    }
    async fn put(
        &self,
        key: &str,
        body: crate::bucket::Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        self.inner.put(key, body, guard, metadata).await
    }
    async fn list(&self, prefix: &str) -> Result<Vec<crate::bucket::Entry>> {
        self.inner.list(prefix).await
    }
    async fn delete(&self, key: &str) -> Result<()> {
        self.inner.delete(key).await
    }
}

#[test]
fn two_late_renewals_of_ours_are_not_a_fence() {
    block_on(async {
        let store = Late::default();
        let late = store.late.clone();
        let bucket = Bucket::new(store, "");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        a.renew(0).await.unwrap();
        let ours = |expires_ms| {
            let record = NodeRecord {
                node: "a".into(),
                address: "a:1".into(),
                expires_ms,
                instance: a.instance,
                draining: false,
                write: 0,
            };
            serde_json::to_vec(&record).unwrap()
        };
        // Two timed-out rounds landed: one before this renewal's first
        // read, one just after it.
        bucket.put(&node_key("a"), ours(11_000), &[]).await.unwrap();
        late.replace(Some((node_key("a"), ours(12_000))));
        assert_eq!(a.renew(3_000).await.unwrap(), 13_000);
    });
}

#[test]
fn a_listing_deletes_records_long_expired() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let b = Registry::new(bucket.clone(), "b", "b:1", 10_000);
        let c = Registry::new(bucket.clone(), "c", "c:1", 10_000);
        a.renew(0).await.unwrap();
        b.renew(90_000).await.unwrap();
        c.renew(115_000).await.unwrap();
        // a expired at 10s, b at 100s: at 115s only a is ten lives dead.
        let live = c.peers(115_000).await.unwrap();
        assert_eq!(live.iter().map(|p| &*p.node).collect::<Vec<_>>(), ["c"]);
        let keys: Vec<_> = bucket.list("nodes/").await.unwrap();
        let keys: Vec<_> = keys.iter().map(|e| &*e.key).collect();
        assert_eq!(keys, ["nodes/b.json", "nodes/c.json"]);
    });
}

#[test]
fn a_deregistration_outlasts_a_renewal_of_ours_that_landed_first() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let b = Registry::new(bucket.clone(), "b", "b:1", 10_000);
        a.renew(0).await.unwrap();
        // A timed-out renewal of a's lands before the deregistration.
        let landed = NodeRecord {
            node: "a".into(),
            address: "a:1".into(),
            expires_ms: 11_000,
            instance: a.instance,
            draining: false,
            write: 0,
        };
        let body = serde_json::to_vec(&landed).unwrap();
        bucket.put(&node_key("a"), body, &[]).await.unwrap();
        a.deregister().await.unwrap();
        b.renew(1).await.unwrap();
        let live = b.peers(1).await.unwrap();
        assert_eq!(live.iter().map(|p| &*p.node).collect::<Vec<_>>(), ["b"]);
        // No renewal brings a back.
        assert!(matches!(a.renew(2).await, Err(Error::NotHere(_))));
        let live = b.peers(2).await.unwrap();
        assert_eq!(live.iter().map(|p| &*p.node).collect::<Vec<_>>(), ["b"]);
    });
}

#[test]
fn a_draining_node_serves_no_more_in_its_peers_listings() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let b = Registry::new(bucket.clone(), "b", "b:1", 10_000);
        a.renew(0).await.unwrap();
        b.renew(0).await.unwrap();
        b.peers(1).await.unwrap();
        let mut serving = b.serving_nodes(1);
        serving.sort();
        assert_eq!(serving, ["a", "b"]);
        a.start_draining();
        a.renew(2).await.unwrap();
        b.peers(3).await.unwrap();
        // Still live, so what it owns routes there, but chosen for nothing.
        assert!(b.listed_live("a", 3));
        assert_eq!(b.serving_nodes(3), ["b"]);
    });
}

/// What a fake bucket (`FakeS3`) does with a request it was told to stall:
/// it never answers it, and applies it at one of these points, if any.
#[derive(Clone, Copy, Debug)]
enum Lands {
    Never,
    /// Just before it serves the next request for the same key.
    BeforeNext,
    /// Just after it serves the next request for the same key.
    AfterNext,
    /// When the test says (`FakeS3::release`).
    OnRelease,
}

#[derive(Clone, Copy, Debug)]
enum Fault {
    Stall(Lands),
    /// Answers this status and does nothing.
    Refuse(u16),
    /// Serves the request at once and answers it this late.
    Slow(std::time::Duration),
}

/// One request as the fake bucket reads it.
struct Call {
    method: String,
    key: String,
    prefix: Option<String>,
    if_match: Option<String>,
    if_none_match: bool,
    body: Vec<u8>,
}

impl Call {
    fn of(request: &crate::fake_http::Request) -> Self {
        let header = |name| request.header(name).map(str::to_owned);
        let prefix = request.query.split('&').find_map(|pair| {
            let value = pair.strip_prefix("prefix=")?;
            Some(value.replace("%2F", "/"))
        });
        Self {
            method: request.method.clone(),
            // `/<bucket>/<key>`.
            key: request.path.splitn(3, '/').nth(2).unwrap_or("").to_owned(),
            prefix,
            if_match: header("if-match"),
            if_none_match: header("if-none-match").is_some(),
            body: request.body.to_vec(),
        }
    }

    /// `PUT nodes/a.json`, or `LIST nodes/` for a listing.
    fn what(&self) -> String {
        match &self.prefix {
            Some(prefix) if self.key.is_empty() => format!("LIST {prefix}"),
            _ => format!("{} {}", self.method, self.key),
        }
    }
}

/// A status, an `ETag` and a body.
type Answer = (u16, String, Vec<u8>);

/// `answer` as the HTTP server sends it, `late`.
fn sent((status, etag, body): Answer, late: std::time::Duration) -> crate::fake_http::Answer {
    let headers = if etag.is_empty() {
        Vec::new()
    } else {
        vec![("etag", etag)]
    };
    let body = String::from_utf8(body).unwrap();
    let reply = crate::fake_http::Answer::Reply {
        status,
        headers,
        body,
    };
    crate::fake_http::Answer::After(late, Box::new(reply))
}

#[derive(Default)]
struct FakeState {
    objects: std::collections::BTreeMap<String, (Vec<u8>, String)>,
    /// Every request, in the order it arrived.
    log: Vec<String>,
    /// Faults for the next requests they name, first match first.
    faults: Vec<(String, Fault)>,
    /// A stalled request still to land, and when.
    stalled: Option<(Call, Lands)>,
    /// How late every answer comes, a fault's aside.
    slow: std::time::Duration,
}

impl FakeState {
    /// S3's answer to `request`. A version is a hash of the body, as S3's
    /// `ETag` of an object written in one piece is.
    fn apply(&mut self, request: &Call) -> Answer {
        let current = self.objects.get(&request.key);
        match request.method.as_str() {
            "GET" if request.prefix.is_some() => {
                let prefix = request.prefix.as_deref().unwrap_or("");
                let keys = self.objects.keys().filter(|k| k.starts_with(prefix));
                let mut xml = "<ListBucketResult>".to_owned();
                for key in keys {
                    xml += &format!("<Contents><Key>{key}</Key></Contents>");
                }
                xml += "</ListBucketResult>";
                (200, String::new(), xml.into_bytes())
            }
            "GET" => match current {
                Some((body, etag)) => (200, etag.clone(), body.clone()),
                None => (404, String::new(), Vec::new()),
            },
            "PUT" => {
                let etag = current.map(|(_, etag)| etag);
                let holds = if request.if_none_match {
                    etag.is_none()
                } else {
                    request.if_match.as_ref().is_none_or(|m| etag == Some(m))
                };
                if !holds {
                    return (412, String::new(), Vec::new());
                }
                let etag = format!("\"{}\"", crate::hex(&crate::sha256(&request.body)));
                let entry = (request.body.clone(), etag.clone());
                self.objects.insert(request.key.clone(), entry);
                (200, etag, Vec::new())
            }
            _ => (405, String::new(), Vec::new()),
        }
    }

    /// What to answer `request`, and how late, or `None` to stall it.
    fn serve(&mut self, request: Call) -> Option<(Answer, std::time::Duration)> {
        let what = request.what();
        self.log.push(what.clone());
        let fault = self.faults.iter().position(|(w, _)| *w == what);
        let (mut refused, mut late) = (None, self.slow);
        match fault.map(|i| self.faults.remove(i).1) {
            Some(Fault::Stall(lands)) => {
                self.stalled = Some((request, lands));
                return None;
            }
            Some(Fault::Refuse(status)) => refused = Some(status),
            Some(Fault::Slow(after)) => late = after,
            None => {}
        }
        let next = matches!(&self.stalled, Some((s, _)) if s.key == request.key);
        let lands = |when: fn(Lands) -> bool, state: &mut Self| {
            if next && state.stalled.as_ref().is_some_and(|(_, l)| when(*l)) {
                let (stalled, _) = state.stalled.take().unwrap();
                state.apply(&stalled);
            }
        };
        lands(|l| matches!(l, Lands::BeforeNext), self);
        let answer = match refused {
            Some(status) => (status, String::new(), Vec::new()),
            None => self.apply(&request),
        };
        lands(|l| matches!(l, Lands::AfterNext), self);
        Some((answer, late))
    }
}

/// The S3 calls a registry makes, over HTTP, each served as the faults the
/// test arms say: the bucket a node's S3 client talks to, so that a stalled
/// request stalls the client's own connection.
#[derive(Clone)]
struct FakeS3(std::sync::Arc<std::sync::Mutex<FakeState>>);

impl FakeS3 {
    /// Serves on a local port; answers the bucket over it.
    async fn start() -> (Self, Bucket) {
        let fake = Self(std::sync::Arc::default());
        let state = fake.0.clone();
        let server = crate::fake_http::Server::serving(move |request, _| {
            let served = state.lock().unwrap().serve(Call::of(request));
            served.map_or(crate::fake_http::Answer::Stall, |(answer, late)| {
                sent(answer, late)
            })
        })
        .await;
        let options = [
            ("aws_endpoint", format!("http://{}", server.host)),
            ("aws_region", "us-east-1".to_owned()),
            ("aws_access_key_id", "k".to_owned()),
            ("aws_secret_access_key", "s".to_owned()),
        ]
        .map(|(name, value)| (name.to_owned(), value));
        let (s3, prefix) = crate::s3::S3::new("s3://b/", &options).unwrap();
        (fake, Bucket::new(s3, &prefix))
    }

    fn arm(&self, what: &str, fault: Fault) {
        self.0.lock().unwrap().faults.push((what.to_owned(), fault));
    }

    /// Answers every request `late`.
    fn slow(&self, late: std::time::Duration) {
        self.0.lock().unwrap().slow = late;
    }

    /// Applies the stalled request now.
    fn release(&self) {
        let mut state = self.0.lock().unwrap();
        let (stalled, _) = state.stalled.take().unwrap();
        state.apply(&stalled);
    }

    /// The requests since the last call.
    fn log(&self) -> Vec<String> {
        std::mem::take(&mut self.0.lock().unwrap().log)
    }

    fn record(&self, key: &str) -> NodeRecord {
        let state = self.0.lock().unwrap();
        serde_json::from_slice(&state.objects[key].0).unwrap()
    }
}

fn block_on_io<F: std::future::Future>(f: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
        .block_on(f)
}

/// A bound on a lease round in these tests: generous, as a stalled request
/// is sent again long before it, and well short of a request's own 20 s.
const ROUND: std::time::Duration = std::time::Duration::from_secs(10);

/// The bound on each request of a round (`http::prompt`): far longer than
/// a request the fake answers takes, however slow the machine, so that only
/// the requests it stalls are sent again.
const ATTEMPT: std::time::Duration = std::time::Duration::from_secs(1);

/// What a renewal sends twice (`Cluster::renew_round`).
const RENEWAL: &[Repeat] = &[Repeat::Read, Repeat::GuardedWrite];

/// A lease round's renewal, as `Cluster::maintain` runs it, each request
/// bounded at `ATTEMPT`.
async fn renew_round(registry: &Registry, now_ms: u64, round: std::time::Duration) -> Option<u64> {
    bounded_renew_round(registry, now_ms, ATTEMPT, round).await
}

async fn bounded_renew_round(
    registry: &Registry,
    now_ms: u64,
    attempt: std::time::Duration,
    round: std::time::Duration,
) -> Option<u64> {
    let renew = crate::http::prompt(attempt, RENEWAL, registry.renew(now_ms));
    tokio::time::timeout(round, renew)
        .await
        .ok()
        .map(Result::unwrap)
}

/// How long the renewals here wait before they write again to a record a
/// write of theirs is in progress on.
const CONTENDED_PAUSE: std::time::Duration = std::time::Duration::from_millis(200);

const PUT: &str = "PUT nodes/a.json";

/// A fake bucket with `a`'s record in it.
async fn registered() -> (FakeS3, Registry) {
    let (fake, bucket) = FakeS3::start().await;
    let a = Registry::new(bucket, "a", "a:1", 10_000).contended_pause(CONTENDED_PAUSE);
    a.renew(0).await.unwrap();
    fake.log();
    (fake, a)
}

#[test]
fn a_renewal_outlasts_a_stalled_write_whether_or_not_it_lands() {
    let reread = vec![PUT, PUT, "GET nodes/a.json", PUT];
    let cases = [
        // The bucket never applies it, or applies it after the write sent
        // again, whose version it then no longer names.
        (Lands::Never, None, vec![PUT, PUT]),
        (Lands::AfterNext, None, vec![PUT, PUT]),
        // It lands first: the write sent again fails its guard, and the
        // renewal rereads its own record.
        (Lands::BeforeNext, None, reread.clone()),
        // It lands as the write sent again fails as a concurrent one (R2's
        // 409).
        (Lands::AfterNext, Some(409), reread),
    ];
    for (lands, refused, expected) in cases {
        block_on_io(async {
            let (fake, a) = registered().await;
            fake.arm(PUT, Fault::Stall(lands));
            if let Some(status) = refused {
                fake.arm(PUT, Fault::Refuse(status));
            }
            assert_eq!(renew_round(&a, 1_000, ROUND).await, Some(11_000));
            assert_eq!(fake.log(), expected, "{lands:?} {refused:?}");
            let record = fake.record("nodes/a.json");
            assert_eq!((record.expires_ms, record.instance), (11_000, a.instance));
            // The renewal knows the version in the bucket: the next one
            // writes once.
            assert_eq!(renew_round(&a, 2_000, ROUND).await, Some(12_000));
            assert_eq!(fake.log(), [PUT], "{lands:?} {refused:?}");
        });
    }
}

#[test]
fn a_stalled_write_never_lands_on_a_record_its_clock_wrote_again() {
    block_on_io(async {
        let (fake, a) = registered().await;
        assert_eq!(renew_round(&a, 10_000, ROUND).await, Some(20_000));
        // The clock steps back: this renewal writes an earlier expiry, and
        // its first copy stalls.
        fake.arm(PUT, Fault::Stall(Lands::OnRelease));
        assert_eq!(renew_round(&a, 5_000, ROUND).await, Some(15_000));
        // Back where it was, the clock writes the expiry it wrote before.
        assert_eq!(renew_round(&a, 10_000, ROUND).await, Some(20_000));
        // The stalled copy names the version of the record at 10_000; the
        // one at 10_000 again is another.
        fake.release();
        assert_eq!(fake.record("nodes/a.json").expires_ms, 20_000);
    });
}

#[test]
fn a_renewal_whose_write_stalls_twice_times_out() {
    block_on_io(async {
        let (fake, a) = registered().await;
        fake.arm(PUT, Fault::Stall(Lands::Never));
        fake.arm(PUT, Fault::Stall(Lands::Never));
        let round = 4 * ATTEMPT;
        assert_eq!(renew_round(&a, 1_000, round).await, None);
        // Sent again once, not once per attempt's bound.
        assert_eq!(fake.log(), [PUT, PUT]);
    });
}

#[test]
fn a_renewal_racing_a_write_of_its_own_pauses_between_tries() {
    block_on_io(async {
        let (fake, a) = registered().await;
        // Each try is refused as a concurrent write while the record still
        // reads as before. Back to back, they would go a millisecond or so
        // apart, faster than R2 takes writes to one key.
        for _ in 0..3 {
            fake.arm(PUT, Fault::Refuse(409));
        }
        let started = std::time::Instant::now();
        assert_eq!(renew_round(&a, 1_000, ROUND).await, Some(11_000));
        assert!(started.elapsed() >= 3 * CONTENDED_PAUSE);
        let get = "GET nodes/a.json";
        assert_eq!(fake.log(), [PUT, get, PUT, get, PUT, get, PUT]);
    });
}

#[test]
fn a_listing_outlasts_a_stalled_list_or_read() {
    block_on_io(async {
        let (fake, bucket) = FakeS3::start().await;
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let b = Registry::new(bucket, "b", "b:1", 10_000);
        a.renew(0).await.unwrap();
        b.renew(0).await.unwrap();
        for stalled in ["LIST nodes/", "GET nodes/a.json"] {
            fake.log();
            fake.arm(stalled, Fault::Stall(Lands::Never));
            let listing = &[Repeat::Read, Repeat::Delete];
            let peers = crate::http::prompt(ATTEMPT, listing, b.peers(1));
            let peers = tokio::time::timeout(ROUND, peers).await.unwrap().unwrap();
            let mut nodes: Vec<_> = peers.into_iter().map(|p| p.node).collect();
            nodes.sort();
            assert_eq!(nodes, ["a", "b"], "{stalled}");
            let log = fake.log();
            assert_eq!(log.iter().filter(|w| *w == stalled).count(), 2, "{log:?}");
        }
    });
}

#[test]
fn a_refused_write_backs_off_without_being_sent_again() {
    block_on_io(async {
        let (fake, a) = registered().await;
        // Refused three times: sent at 0, 0.25 and 0.75 s, then answered at
        // 1.75 s. Were the pauses part of a request's wait, the 1 s one
        // would be cut short at 1 s, the write sent again then.
        for _ in 0..3 {
            fake.arm(PUT, Fault::Refuse(503));
        }
        let started = std::time::Instant::now();
        assert_eq!(renew_round(&a, 1_000, ROUND).await, Some(11_000));
        assert!(started.elapsed() >= std::time::Duration::from_millis(1_750));
        assert_eq!(fake.log(), [PUT; 4]);
    });
}

#[test]
fn a_renewal_outlasts_a_bucket_slow_to_answer_everything() {
    block_on_io(async {
        let (fake, a) = registered().await;
        // The default's proportions: every answer comes later than a
        // request's bound, and well within the round.
        let t = cluster_logic::lease::Timing::divided(4).unwrap();
        let (attempt, round) = (t.renew_attempt_ms, t.renew_timeout_ms);
        fake.slow(std::time::Duration::from_millis(attempt + attempt / 3));
        let ms = std::time::Duration::from_millis;
        let renewed = bounded_renew_round(&a, 1_000, ms(attempt), ms(round)).await;
        assert_eq!(renewed, Some(11_000));
        // The first copy landed and answered first; the second, refused,
        // is dropped.
        assert_eq!(fake.log(), [PUT, PUT]);
    });
}

#[test]
fn a_write_whose_copy_is_refused_waits_for_its_own_answer() {
    block_on_io(async {
        let (fake, a) = registered().await;
        // The first copy lands at once and answers after the second, which
        // its write fails.
        fake.arm(PUT, Fault::Slow(ATTEMPT + ATTEMPT / 2));
        assert_eq!(renew_round(&a, 1_000, ROUND).await, Some(11_000));
        // No reread: the first copy's answer is the renewal's.
        assert_eq!(fake.log(), [PUT, PUT]);
        let record = fake.record("nodes/a.json");
        assert_eq!(record.expires_ms, 11_000);
        assert_eq!(renew_round(&a, 2_000, ROUND).await, Some(12_000));
        assert_eq!(fake.log(), [PUT]);
    });
}
