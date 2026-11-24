// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use std::cell::Cell;
use std::cell::RefCell;

use super::*;
use crate::hex;
use crate::md5;

#[test]
fn md5_vectors() {
    // RFC 1321's suite: the last two pad into a second block.
    for (text, digest) in [
        ("", "d41d8cd98f00b204e9800998ecf8427e"),
        ("a", "0cc175b9c0f1b6a831c399e269772661"),
        ("abc", "900150983cd24fb0d6963f7d28e17f72"),
        ("message digest", "f96b697d7cb7938d525a2f31aaf161d0"),
        (
            "abcdefghijklmnopqrstuvwxyz",
            "c3fcd3d76192e4007dfb496cca67e13b",
        ),
        (
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
            "d174ab98d277d9f5a5611c2c9f419d9f",
        ),
        (
            "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
            "57edf4a22be3c955ac49da2e2107b67a",
        ),
    ] {
        assert_eq!(hex(&md5(text.as_bytes())), digest, "{text}");
    }
}

#[test]
fn ranges() {
    let from = |offset, length| Range::From { offset, length };
    assert_eq!(from(2, Some(3)).header(), "bytes=2-4");
    assert_eq!(from(2, None).header(), "bytes=2-");
    let end = u64::MAX - 1;
    assert_eq!(
        from(5, Some(u64::MAX - 4)).header(),
        format!("bytes=5-{end}")
    );
    assert_eq!(Range::Suffix(3).header(), "bytes=-3");
    for (range, of) in [
        (from(2, Some(3)), Some((2, 3))),
        (from(8, Some(100)), Some((8, 2))),
        (from(8, None), Some((8, 2))),
        (from(10, None), None),
        (Range::Suffix(3), Some((7, 3))),
        (Range::Suffix(100), Some((0, 10))),
        (Range::Suffix(0), None),
    ] {
        assert_eq!(range.of(10), of, "{range:?}");
    }
    assert_eq!(Range::Suffix(1).of(0), None);
}

/// A store that answers its first `held` guarded writes as of unknown
/// outcome without applying them yet, as the client answers a write whose
/// earlier copy the bucket is still applying (R2's 409 to its resend). A
/// held write is applied when the next write arrives if `lands`, and never
/// otherwise.
struct InProgress {
    inner: memory::Memory,
    held: Cell<usize>,
    lands: bool,
    copies: RefCell<Vec<(String, Payload, Option<Guard>)>>,
    puts: Rc<Cell<usize>>,
}

#[async_trait::async_trait(?Send)]
impl Store for InProgress {
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
        self.inner.get(key, to).await
    }

    async fn put(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        self.puts.set(self.puts.get() + 1);
        let copy = self.copies.borrow_mut().pop();
        if let Some((key, body, guard)) = copy.filter(|_| self.lands) {
            self.inner.put(&key, body, guard, &[]).await?;
        }
        if guard.is_some() && self.held.get() > 0 {
            self.held.set(self.held.get() - 1);
            self.copies.borrow_mut().push((key.to_owned(), body, guard));
            return Err(Error::OutcomeUnknown(format!("PUT {key}: 409")));
        }
        self.inner.put(key, body, guard, metadata).await
    }

    async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        self.inner.list(prefix).await
    }

    async fn delete(&self, key: &str) -> Result<()> {
        self.inner.delete(key).await
    }
}

fn bytes(body: &[u8]) -> Payload {
    Payload::Bytes(Bytes::copy_from_slice(body))
}

/// A write whose refusal may be of its own copy still in progress is
/// settled by what the object holds once that copy is done: while the
/// guarded version is still there, the write goes again, and it lands once,
/// whichever copy lands. Another writer's version refuses it at once.
#[test]
fn a_write_refused_beside_its_own_copy_in_progress_is_sent_again() {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_time()
        .build()
        .unwrap();
    runtime.block_on(async {
        let pause = Duration::from_millis(1);
        // The guard on the object as it is, and the bucket's store, with
        // the object `k` holding `old` unless `absent`, then `other` if
        // `replaced`.
        let setup = |absent: bool, replaced: bool, held: usize, lands: bool| async move {
            let inner = memory::Memory::default();
            let guard = if absent {
                Guard::Absent
            } else {
                let version = inner.put("k", bytes(b"old"), None, &[]).await.unwrap();
                Guard::Match(version.unwrap())
            };
            if replaced {
                inner.put("k", bytes(b"other"), None, &[]).await.unwrap();
            }
            let puts = Rc::new(Cell::new(0));
            let store = InProgress {
                inner,
                held: Cell::new(held),
                lands,
                copies: RefCell::new(Vec::new()),
                puts: puts.clone(),
            };
            (Bucket::new(store, ""), guard, puts)
        };
        for (absent, replaced, held, lands, landed, writes) in [
            // The held copy lands as the write goes again, and refuses it.
            (true, false, 1, true, true, 2),
            (false, false, 1, true, true, 2),
            // The held copy never lands: the write sent again does.
            (true, false, 1, false, true, 2),
            (false, false, 2, false, true, 3),
            // Another writer's version: refused, nothing sent again.
            (false, true, 1, false, false, 1),
        ] {
            let case = format!("absent {absent}, replaced {replaced}, held {held}, lands {lands}");
            let (bucket, guard, puts) = setup(absent, replaced, held, lands).await;
            let written = bucket
                .put_if_settled("k", b"new".to_vec(), guard, pause)
                .await;
            let written = written.unwrap();
            let current = bucket.get("k").await.unwrap().unwrap();
            if landed {
                assert_eq!(current.0, b"new", "{case}");
                assert_eq!(written, Some(current.1), "{case}");
            } else {
                assert_eq!(current.0, b"other", "{case}");
                assert_eq!(written, None, "{case}");
            }
            assert_eq!(puts.get(), writes, "{case}");
        }
        // Held every time: the outcome stays unknown, after a bounded
        // number of writes.
        let (bucket, guard, puts) = setup(false, false, usize::MAX, false).await;
        let written = bucket
            .put_if_settled("k", b"new".to_vec(), guard, pause)
            .await;
        assert!(
            matches!(written, Err(Error::OutcomeUnknown(_))),
            "{written:?}"
        );
        assert_eq!(puts.get(), SETTLE_WRITES as usize);
    });
}
