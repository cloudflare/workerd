// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The per-request issue collector. It holds reported issues until the request ends, then hands
//! back the ones it kept.
//!
//! A code is a plain string. The collector knows three by name: the fallbacks [`ERROR_OUTCOME`]
//! and [`HTTP_SERVER_ERROR`], which exist so a failure is never silent, and [`ERROR_LOG`], which it
//! caps. Every other code is a fact. A fallback reported after any fact is dropped on the spot,
//! because the fact already explains the failure.

use std::mem;

pub const ERROR_OUTCOME: &str = "error_outcome";
pub const HTTP_SERVER_ERROR: &str = "http_server_error";
pub const ERROR_LOG: &str = "error_log";

pub const MAX_HELD: usize = 8;
pub const MAX_ERROR_LOGS: usize = 5;
pub const MAX_MESSAGE: usize = 1024;
pub const MAX_FIELDS: usize = 8;
pub const MAX_FIELD_STRING: usize = 256;
pub const MAX_ERRORS: usize = 4;
pub const MAX_STACK: usize = 16 * 1024;
pub const MAX_ATTACHMENTS: usize = 4;
pub const MAX_ATTACHMENT_BYTES: usize = 16 * 1024;
pub const MAX_USER_CONTEXT: usize = 128;
pub const CRUMB_LABEL: usize = 128;
pub const CRUMB_LEVEL: usize = 8;
pub const MAX_EVENT: usize = 32;
pub const MAX_METHOD: usize = 16;
pub const MAX_PATH: usize = 256;
const RING_SLOTS: usize = 3;

/// Returns the longest prefix of `text` that is at most `max` bytes and ends on a character
/// boundary.
pub fn truncate(text: &str, max: usize) -> &str {
    let mut end = max.min(text.len());
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    text.get(..end).unwrap_or_default()
}

/// A string stored inline, so recording a breadcrumb never allocates.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InlineStr<const N: usize> {
    len: u8,
    buf: [u8; N],
}

impl<const N: usize> InlineStr<N> {
    pub fn new(text: &str) -> Self {
        let text = truncate(text, N.min(usize::from(u8::MAX)));
        let mut buf = [0; N];
        if let Some(dest) = buf.get_mut(..text.len()) {
            dest.copy_from_slice(text.as_bytes());
        }
        Self {
            len: u8::try_from(text.len()).unwrap_or(u8::MAX),
            buf,
        }
    }

    pub fn as_str(&self) -> &str {
        self.buf
            .get(..usize::from(self.len))
            .and_then(|bytes| std::str::from_utf8(bytes).ok())
            .unwrap_or_default()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CrumbKind {
    Call,
    Log(InlineStr<CRUMB_LEVEL>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CrumbState {
    Started,
    Done,
    Failed,
    /// Logs have no state.
    None,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Breadcrumb {
    pub at_ms: u32,
    pub kind: CrumbKind,
    pub state: CrumbState,
    /// Which push filled this slot.
    pub generation: u32,
    pub label: InlineStr<CRUMB_LABEL>,
}

impl Breadcrumb {
    pub fn call(at_ms: u32, label: &str) -> Self {
        Self {
            at_ms,
            kind: CrumbKind::Call,
            state: CrumbState::Started,
            generation: 0,
            label: InlineStr::new(label),
        }
    }

    pub fn log(at_ms: u32, level: &str, text: &str) -> Self {
        Self {
            at_ms,
            kind: CrumbKind::Log(InlineStr::new(level)),
            state: CrumbState::None,
            generation: 0,
            label: InlineStr::new(text),
        }
    }
}

/// Names a slot and the push that filled it, so a late `finish` can tell the slot was reused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CrumbId {
    slot: usize,
    generation: u32,
}

/// The last three breadcrumbs. Each push overwrites the oldest.
#[derive(Debug, Default)]
pub struct BreadcrumbRing {
    slots: [Option<Breadcrumb>; RING_SLOTS],
    next: usize,
    generation: u32,
}

impl BreadcrumbRing {
    pub fn push(&mut self, mut crumb: Breadcrumb) -> CrumbId {
        self.generation += 1;
        crumb.generation = self.generation;
        let id = CrumbId {
            slot: self.next,
            generation: self.generation,
        };
        if let Some(slot) = self.slots.get_mut(self.next) {
            *slot = Some(crumb);
        }
        self.next = (self.next + 1) % RING_SLOTS;
        id
    }

    /// Marks a call done or failed, unless its slot was reused.
    pub fn finish(&mut self, id: CrumbId, ok: bool) {
        if let Some(Some(crumb)) = self.slots.get_mut(id.slot)
            && crumb.generation == id.generation
        {
            crumb.state = if ok {
                CrumbState::Done
            } else {
                CrumbState::Failed
            };
        }
    }

    /// The current slots, oldest first.
    pub fn snapshot(&self) -> Vec<Breadcrumb> {
        (0..RING_SLOTS)
            .filter_map(|i| {
                self.slots
                    .get((self.next + i) % RING_SLOTS)
                    .copied()
                    .flatten()
            })
            .collect()
    }
}

/// Who the request is for, from the `user.id`, `session.id` and `account.id` span attributes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct UserContext {
    pub user_id: Option<String>,
    pub session_id: Option<String>,
    pub account_id: Option<String>,
}

impl UserContext {
    /// The last value for a key wins. Other keys are ignored.
    fn note(&mut self, key: &str, value: &str) {
        let slot = match key {
            "user.id" => &mut self.user_id,
            "session.id" => &mut self.session_id,
            "account.id" => &mut self.account_id,
            _ => return,
        };
        *slot = Some(truncate(value, MAX_USER_CONTEXT).to_owned());
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Str(String),
    Int(i64),
    Float(f64),
    Bool(bool),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Field {
    pub name: String,
    pub value: Value,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ErrorInfo {
    pub name: String,
    pub message: String,
    pub stack: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Attachment {
    pub name: String,
    pub body: String,
    /// The body was cut at the size cap.
    pub truncated: bool,
}

#[derive(Clone, Debug)]
pub struct Issue {
    pub code: String,
    pub message: String,
    pub fields: Vec<Field>,
    pub errors: Vec<ErrorInfo>,
    pub attachments: Vec<Attachment>,
    /// The ring as it was when the issue was reported, oldest first.
    pub breadcrumbs: Vec<Breadcrumb>,
    pub user: UserContext,
    attachment_bytes: usize,
}

impl Issue {
    /// Returns false if the issue already has the maximum number of fields.
    pub fn add_field(&mut self, name: &str, value: Value) -> bool {
        if self.fields.len() >= MAX_FIELDS {
            return false;
        }
        let value = match value {
            Value::Str(text) => Value::Str(truncate(&text, MAX_FIELD_STRING).to_owned()),
            other => other,
        };
        self.fields.push(Field {
            name: name.to_owned(),
            value,
        });
        true
    }

    /// Returns false if the issue already has the maximum number of errors.
    pub fn add_error(&mut self, name: &str, message: &str, stack: Option<&str>) -> bool {
        if self.errors.len() >= MAX_ERRORS {
            return false;
        }
        self.errors.push(ErrorInfo {
            name: name.to_owned(),
            message: message.to_owned(),
            stack: stack.map(|stack| truncate(stack, MAX_STACK).to_owned()),
        });
        true
    }

    /// Returns false if the issue already has the maximum number of attachments, or the 16 KB
    /// total is spent. A body that does not fit is cut and marked truncated.
    pub fn add_attachment(&mut self, name: &str, body: &str) -> bool {
        let room = MAX_ATTACHMENT_BYTES - self.attachment_bytes;
        if self.attachments.len() >= MAX_ATTACHMENTS || room == 0 {
            return false;
        }
        let kept = truncate(body, room);
        self.attachment_bytes += kept.len();
        self.attachments.push(Attachment {
            name: name.to_owned(),
            body: kept.to_owned(),
            truncated: kept.len() < body.len(),
        });
        true
    }
}

/// What event started the request. Set by the code that delivers it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Invocation {
    pub event: String,
    pub method: String,
    pub path: String,
}

/// What the collector kept.
#[derive(Debug, Default)]
pub struct End {
    pub status: Option<u16>,
    pub invocation: Invocation,
    pub issues: Vec<Issue>,
}

#[derive(Debug, Default)]
pub struct IssueCollector {
    held: Vec<Issue>,
    /// Some non-fallback issue was reported, even if it was then capped or a repeat.
    fact_reported: bool,
    ring: BreadcrumbRing,
    user: UserContext,
    invocation: Invocation,
    status: Option<u16>,
}

fn is_fallback(code: &str) -> bool {
    code == ERROR_OUTCOME || code == HTTP_SERVER_ERROR
}

impl IssueCollector {
    /// Reports an issue. Returns the held issue so the caller can add fields, errors and
    /// attachments, or `None` if it was dropped, capped or an exact repeat.
    ///
    /// A fallback reported after a fact is dropped. A fact that is capped or a repeat still counts
    /// as a fact, so it still explains the fallbacks reported after it.
    pub fn report(&mut self, code: &str, message: &str) -> Option<&mut Issue> {
        if is_fallback(code) {
            if self.fact_reported {
                return None;
            }
        } else {
            self.fact_reported = true;
        }

        let message = truncate(message, MAX_MESSAGE);
        if self
            .held
            .iter()
            .any(|held| held.code == code && held.message == message)
        {
            return None;
        }
        let error_logs = self
            .held
            .iter()
            .filter(|held| held.code == ERROR_LOG)
            .count();
        if self.held.len() >= MAX_HELD || (code == ERROR_LOG && error_logs >= MAX_ERROR_LOGS) {
            return None;
        }

        self.held.push(Issue {
            code: code.to_owned(),
            message: message.to_owned(),
            fields: Vec::new(),
            errors: Vec::new(),
            attachments: Vec::new(),
            breadcrumbs: self.ring.snapshot(),
            user: self.user.clone(),
            attachment_bytes: 0,
        });
        self.held.last_mut()
    }

    /// The issue the last successful `report` returned, if nothing has been reported since.
    pub fn last_held(&mut self) -> Option<&mut Issue> {
        self.held.last_mut()
    }

    pub fn push_breadcrumb(&mut self, crumb: Breadcrumb) -> CrumbId {
        self.ring.push(crumb)
    }

    pub fn finish_breadcrumb(&mut self, id: CrumbId, ok: bool) {
        self.ring.finish(id, ok);
    }

    pub fn note_user_context(&mut self, key: &str, value: &str) {
        self.user.note(key, value);
    }

    pub fn set_event(&mut self, event: &str) {
        truncate(event, MAX_EVENT).clone_into(&mut self.invocation.event);
    }

    pub fn set_request(&mut self, method: &str, path: &str) {
        truncate(method, MAX_METHOD).clone_into(&mut self.invocation.method);
        truncate(path, MAX_PATH).clone_into(&mut self.invocation.path);
    }

    pub fn set_status(&mut self, status: u16) {
        self.status = Some(status);
    }

    /// Ends the request and returns every held issue. A second call returns none.
    pub fn end(&mut self) -> End {
        End {
            status: self.status,
            invocation: self.invocation.clone(),
            issues: mem::take(&mut self.held),
        }
    }
}

#[cfg(test)]
#[path = "collector-test.rs"]
mod tests;
