// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! State shared by every tracker of one isolate: ID allocation and stack deduplication.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;

use crate::AsyncId;

/// Identifies an isolate within the process.
pub type IsolateId = u64;

/// Identifies a deduplicated creation stack within an isolate. Never `0`.
pub type StackId = u32;

static NEXT_ISOLATE: AtomicU64 = AtomicU64::new(1);

/// Default cap on the distinct stacks an isolate keeps.
pub const DEFAULT_MAX_STACKS: usize = 10_000;

/// Default cap on the approximate memory, in bytes, of the stacks an isolate keeps.
pub const DEFAULT_MAX_STACK_BYTES: usize = 16 << 20;

/// One stack frame, innermost first within a stack.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Frame {
    #[serde(rename = "fn")]
    pub function: String,
    pub script: String,
    pub script_id: i32,
    pub line: u32,
    #[serde(rename = "col")]
    pub column: u32,
}

/// The identity of a frame for deduplication. Two frames at the same position in the same script
/// are the same frame, so names don't need comparing (or copying, on a hit).
type FrameKey = (i32, u32, u32);

#[derive(Default)]
struct Stacks {
    ids: HashMap<Box<[FrameKey]>, StackId>,
    frames: Vec<Arc<[Frame]>>,
    /// Approximate memory held by `ids` and `frames`; see [`stack_bytes`].
    bytes: usize,
}

/// Approximately what keeping `frames` costs: each frame, its key and its strings.
fn stack_bytes(frames: &[Frame]) -> usize {
    frames
        .iter()
        .map(|f| size_of::<Frame>() + size_of::<FrameKey>() + f.function.len() + f.script.len())
        .sum()
}

/// Per-isolate state. Shared by the isolate's trackers through an `Arc`.
pub struct IsolateState {
    id: IsolateId,
    next_async_id: AtomicU64,
    stacks: Mutex<Stacks>,
    max_stacks: usize,
    max_stack_bytes: usize,
}

impl IsolateState {
    #[must_use]
    pub fn new() -> Self {
        Self {
            id: NEXT_ISOLATE.fetch_add(1, Ordering::Relaxed),
            next_async_id: AtomicU64::new(1),
            stacks: Mutex::new(Stacks::default()),
            max_stacks: DEFAULT_MAX_STACKS,
            max_stack_bytes: DEFAULT_MAX_STACK_BYTES,
        }
    }

    /// Caps the stacks the isolate keeps, by count and by approximate memory. Stacks already kept
    /// still get their IDs; [`IsolateState::intern_stack`] returns `None` for a new one that
    /// would exceed either cap.
    pub const fn set_stack_limits(&mut self, max_stacks: usize, max_bytes: usize) {
        self.max_stacks = max_stacks;
        self.max_stack_bytes = max_bytes;
    }

    #[must_use]
    pub const fn id(&self) -> IsolateId {
        self.id
    }

    /// Allocates a fresh, never-zero resource ID.
    pub fn next_async_id(&self) -> AsyncId {
        self.next_async_id.fetch_add(1, Ordering::Relaxed)
    }

    /// Returns the ID for `frames`, registering them on first sight. Returns `None` for an empty
    /// stack, or for a new one when the isolate keeps as many stacks as its limits allow (see
    /// [`IsolateState::set_stack_limits`]). Kept stacks live as long as the isolate.
    pub fn intern_stack(&self, frames: Vec<Frame>) -> Option<StackId> {
        if frames.is_empty() {
            return None;
        }
        let key: Box<[FrameKey]> = frames
            .iter()
            .map(|f| (f.script_id, f.line, f.column))
            .collect();
        let mut stacks = self.stacks.lock().unwrap_or_else(PoisonError::into_inner);
        if let Some(&id) = stacks.ids.get(&key) {
            return Some(id);
        }
        let bytes = stacks.bytes.saturating_add(stack_bytes(&frames));
        if stacks.frames.len() >= self.max_stacks || bytes > self.max_stack_bytes {
            return None;
        }
        // IDs start at 1, so frames[id - 1] holds stack `id`.
        let id = StackId::try_from(stacks.frames.len() + 1).ok()?;
        stacks.frames.push(frames.into());
        stacks.ids.insert(key, id);
        stacks.bytes = bytes;
        drop(stacks);
        Some(id)
    }

    /// The frames registered for `id`.
    pub fn stack(&self, id: StackId) -> Option<Arc<[Frame]>> {
        let index = usize::try_from(id).ok()?.checked_sub(1)?;
        let stacks = self.stacks.lock().unwrap_or_else(PoisonError::into_inner);
        stacks.frames.get(index).cloned()
    }
}

impl Default for IsolateState {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
#[path = "isolate-test.rs"]
mod tests;
