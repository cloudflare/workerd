// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Purges of Workers Caching's entries, by what their puts named: a namespace, the request's path
//! and its `Cache-Tag`. A fill takes a [`Ticket`] as it begins, from the miss that starts it, and
//! its put returns it: so once [`Purges::fence`] returns on every node, no fill begun before stores
//! what the purge removes.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::collections::VecDeque;
use std::rc::Rc;
use std::time::Duration;
use std::time::Instant;

use tokio::sync::Notify;

use crate::Result;

/// What a put names its entry by, for purges: `{ns}\n{path}\n{tags}` in its value.
#[derive(Default)]
pub(super) struct Meta {
    ns: String,
    path: String,
    tags: String,
}

impl Meta {
    /// From a put's `X-Purge-Namespace`, `X-Purge-Path` and `X-Purge-Tags`; `None` if it has
    /// none of them.
    pub(super) fn from(get: impl Fn(&str) -> Option<String>) -> Option<Self> {
        let [ns, path, tags] = ["Namespace", "Path", "Tags"].map(|n| get(&format!("X-Purge-{n}")));
        if ns.is_none() && path.is_none() && tags.is_none() {
            return None;
        }
        let text = |v: Option<String>| v.unwrap_or_default().replace('\n', " ");
        Some(Self {
            ns: text(ns),
            path: text(path),
            tags: text(tags),
        })
    }

    pub(super) fn bytes(&self) -> Vec<u8> {
        format!("{}\n{}\n{}", self.ns, self.path, self.tags).into_bytes()
    }

    pub(super) fn parse(bytes: &[u8]) -> Option<Self> {
        let text = std::str::from_utf8(bytes).ok()?;
        let mut lines = text.splitn(3, '\n').map(str::to_owned);
        Some(Self {
            ns: lines.next()?,
            path: lines.next()?,
            tags: lines.next()?,
        })
    }
}

/// What a purge removes from namespace `ns`: entries with any of `tags`, or whose path starts with
/// any of `prefixes`, or every one.
#[derive(Clone, Default)]
pub(super) struct Purge {
    ns: String,
    tags: Vec<String>,
    prefixes: Vec<String>,
    everything: bool,
}

impl Purge {
    /// From the query of `url`: `ns`, `tag` and `prefix`, each repeated, and `everything`.
    pub(super) fn parse(url: &[u8]) -> Result<Self> {
        let url = std::str::from_utf8(url).map_err(|_| kj::failed!("purge URL is not UTF-8"))?;
        let url = url::Url::parse(url).map_err(|e| kj::failed!("{e}: {url}"))?;
        let mut purge = Self::default();
        for (name, value) in url.query_pairs() {
            match &*name {
                "ns" => purge.ns = value.into_owned(),
                "tag" => purge.tags.push(value.into_owned()),
                "prefix" => purge.prefixes.push(value.into_owned()),
                "everything" => purge.everything = true,
                _ => {}
            }
        }
        Ok(purge)
    }

    /// The query [`Purge::parse`] reads back.
    pub(super) fn query(&self) -> String {
        let mut query = url::form_urlencoded::Serializer::new(String::new());
        query.append_pair("ns", &self.ns);
        for tag in &self.tags {
            query.append_pair("tag", tag);
        }
        for prefix in &self.prefixes {
            query.append_pair("prefix", prefix);
        }
        if self.everything {
            query.append_key_only("everything");
        }
        query.finish()
    }

    pub(super) fn matches(&self, meta: &Meta) -> bool {
        let tag = |t: &str| self.tags.iter().any(|tag| tag.eq_ignore_ascii_case(t.trim()));
        meta.ns == self.ns
            && (self.everything
                || meta.tags.split(',').any(tag)
                || self.prefixes.iter().any(|p| meta.path.starts_with(p.as_str())))
    }
}

/// How long a purge is remembered for the fills begun before it. One older stores nothing.
const KEEP: Duration = Duration::from_mins(10);

/// The purges begun on this node, which a fill begun before one checks before it stores, and the
/// fills storing.
#[derive(Default)]
pub(super) struct Purges {
    begun: Cell<u64>,
    /// The purges begun within `KEEP`, by number.
    recent: RefCell<VecDeque<(u64, Instant, Purge)>>,
    /// The number of the latest purge no longer in `recent`.
    forgot: Cell<u64>,
    /// The fills storing, by how many purges had begun when each began.
    storing: RefCell<BTreeMap<u64, usize>>,
    stored: Notify,
}

/// When a fill began: after how many purges, if its node was active. As `X-Ticket`, the number
/// or `none`.
pub(super) struct Ticket(pub(super) Option<u64>);

impl Ticket {
    pub(super) fn parse(text: &str) -> Self {
        Self(text.parse().ok())
    }

    pub(super) fn text(&self) -> String {
        self.0.map_or_else(|| "none".to_owned(), |n| n.to_string())
    }
}

/// A fill storing, until dropped.
pub(super) struct Storing(Rc<Purges>, u64);

impl Purges {
    pub(super) fn ticket(&self, active: bool) -> Ticket {
        Ticket(active.then(|| self.begun.get()))
    }

    /// Lets the fill of `ticket` store an entry named by `meta`, unless its node was not active
    /// as it began, so may be in no purger's view, or a purge begun since matches it.
    pub(super) fn store(self: &Rc<Self>, ticket: &Ticket, meta: &Meta) -> Option<Storing> {
        let after = ticket.0?;
        let recent = self.recent.borrow();
        let mut since = recent.iter().filter(|(n, ..)| *n > after);
        if after < self.forgot.get() || since.any(|(.., p)| p.matches(meta)) {
            return None;
        }
        *self.storing.borrow_mut().entry(after).or_default() += 1;
        Some(Storing(Rc::clone(self), after))
    }

    /// Begins `purge`, and waits for the fills begun before it that are storing.
    pub(super) async fn fence(&self, purge: &Purge) {
        let n = self.begun.get() + 1;
        self.begun.set(n);
        {
            let mut recent = self.recent.borrow_mut();
            while let Some((old, ..)) = recent.pop_front_if(|(_, at, _)| at.elapsed() > KEEP) {
                self.forgot.set(old);
            }
            recent.push_back((n, Instant::now(), purge.clone()));
        }
        loop {
            let stored = self.stored.notified();
            let first = self.storing.borrow().first_key_value().map(|(a, _)| *a);
            if first.is_none_or(|after| after >= n) {
                return;
            }
            stored.await;
        }
    }
}

impl Drop for Storing {
    fn drop(&mut self) {
        let mut storing = self.0.storing.borrow_mut();
        if let Some(count) = storing.get_mut(&self.1) {
            *count -= 1;
            if *count == 0 {
                storing.remove(&self.1);
            }
        }
        self.0.stored.notify_waiters();
    }
}
