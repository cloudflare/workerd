//! What a served connection's messages reuse from one to the next, so that a steady stream of
//! requests allocates for its heads and small bodies only when they outgrow what was kept.
//!
//! hyper keeps a written response's emptied header map for the next request it parses; the
//! connection keeps the request's for the next response ([`Scratch::take_headers`]), so one
//! map's room serves both. Header values and small body chunks are cut from a shared block
//! ([`Scratch::copy`]) instead of each being an allocation of its own.

use std::cell::Cell;
use std::cell::RefCell;
use std::sync::Arc;

use bytes::Bytes;
use bytes::BytesMut;

use crate::body::Pipe;

/// The size of a block small copies are cut from.
const BLOCK: usize = 4096;

/// The largest copy cut from a block; a larger one is an allocation of its own, so that a block
/// does not outlive its use by much.
const MAX_CUT: usize = 512;

/// The most headers a kept map or list may have room for: one that grew for a message with many
/// headers is not kept.
const MAX_KEPT_HEADERS: usize = 64;

#[derive(Default)]
pub struct Scratch {
    /// The rest of the block the last copy was cut from.
    block: RefCell<BytesMut>,
    /// An emptied header map.
    headers: Cell<Option<http::HeaderMap>>,
    /// The emptied list of a response head's headers past the builtins ([`crate::body::Head`]).
    others: Cell<Vec<(http::HeaderName, http::HeaderValue)>>,
    /// The pipe of an earlier response body, reused once nothing else holds it.
    pipe: Cell<Option<Arc<Pipe>>>,
}

impl Scratch {
    /// `data`, copied.
    pub fn copy(&self, data: &[u8]) -> Bytes {
        if data.len() > MAX_CUT {
            return Bytes::copy_from_slice(data);
        }
        let mut block = self.block.borrow_mut();
        // The block's earlier cuts are all gone once the connection has written what they were
        // for, so it is usually reclaimed rather than replaced.
        if block.capacity() - block.len() < data.len() && !block.try_reclaim(BLOCK) {
            *block = BytesMut::with_capacity(BLOCK);
        }
        block.extend_from_slice(data);
        block.split().freeze()
    }

    /// An empty header map, with the room of the last one kept.
    pub fn take_headers(&self) -> http::HeaderMap {
        self.headers.take().unwrap_or_default()
    }

    pub fn keep_headers(&self, mut map: http::HeaderMap) {
        if map.capacity() <= MAX_KEPT_HEADERS {
            map.clear();
            self.headers.set(Some(map));
        }
    }

    pub fn take_others(&self) -> Vec<(http::HeaderName, http::HeaderValue)> {
        self.others.take()
    }

    pub fn keep_others(&self, mut others: Vec<(http::HeaderName, http::HeaderValue)>) {
        if others.capacity() <= MAX_KEPT_HEADERS {
            others.clear();
            self.others.set(others);
        }
    }

    /// A new body pipe: the kept one if nothing else holds it any more.
    pub fn take_pipe(&self) -> Arc<Pipe> {
        if let Some(mut pipe) = self.pipe.take()
            && let Some(state) = Arc::get_mut(&mut pipe)
        {
            *state = Pipe::default();
            return pipe;
        }
        Arc::new(Pipe::default())
    }

    pub fn keep_pipe(&self, pipe: Arc<Pipe>) {
        self.pipe.set(Some(pipe));
    }
}

#[cfg(test)]
#[path = "scratch-test.rs"]
mod tests;
