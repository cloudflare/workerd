// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue"
)]

//! The C++ interface. C++ owns one [`Collector`] per request through `rust::Box`.
//!
//! Strings cross as bytes and are decoded lossily, because cxx's `rust::Str` constructor throws on
//! invalid UTF-8 and reporting an issue must never throw.

use std::borrow::Cow;
use std::time::Instant;

use crate::collector::Breadcrumb;
use crate::collector::CrumbKind;
use crate::collector::CrumbState as State;
use crate::collector::IssueCollector;
use crate::collector::Value;

#[cxx::bridge(namespace = "workerd::issues")]
mod bridge {
    enum FieldKind {
        Text,
        Int,
        Float,
        Bool,
    }

    enum CrumbState {
        Started,
        Done,
        Failed,
        /// Logs have no state.
        Stateless,
    }

    struct FieldRecord {
        name: String,
        kind: FieldKind,
        text: String,
        int_value: i64,
        float_value: f64,
        flag: bool,
    }

    struct ErrorRecord {
        name: String,
        message: String,
        /// Empty if the error has no stack.
        stack: String,
    }

    struct AttachmentRecord {
        name: String,
        body: String,
        truncated: bool,
    }

    struct BreadcrumbRecord {
        at_ms: u32,
        is_log: bool,
        state: CrumbState,
        /// The log level. Empty for a call.
        level: String,
        label: String,
    }

    /// Empty strings mean the value was never set.
    struct IssueRecord {
        code: String,
        message: String,
        fields: Vec<FieldRecord>,
        errors: Vec<ErrorRecord>,
        attachments: Vec<AttachmentRecord>,
        breadcrumbs: Vec<BreadcrumbRecord>,
        user_id: String,
        session_id: String,
        account_id: String,
    }

    struct EndRecord {
        /// 0 if no status was reported.
        status: u16,
        event: String,
        method: String,
        path: String,
        issues: Vec<IssueRecord>,
    }

    extern "Rust" {
        /// One per request that opted in to issues. Used only on the request's thread.
        type Collector;

        fn new_collector() -> Box<Collector>;

        /// Holds an issue for the end of the request. Returns false if it was dropped, capped or
        /// an exact repeat. Only if it returns true do the `add_*` calls below attach to it.
        fn report_issue(self: &mut Collector, code: &[u8], message: &[u8]) -> bool;
        fn add_field_string(self: &mut Collector, name: &[u8], value: &[u8]);
        fn add_field_int(self: &mut Collector, name: &[u8], value: i64);
        fn add_field_float(self: &mut Collector, name: &[u8], value: f64);
        fn add_field_bool(self: &mut Collector, name: &[u8], value: bool);
        fn add_error(self: &mut Collector, name: &[u8], message: &[u8], stack: &[u8]);
        fn add_attachment(self: &mut Collector, name: &[u8], body: &[u8]);

        /// `kind` is the value of `issues::BreadcrumbKind`. Only starts and logs are recorded.
        fn breadcrumb(self: &mut Collector, kind: u8, detail: &[u8], label: &[u8]);
        fn note_user_context(self: &mut Collector, key: &[u8], value: &[u8]);
        fn set_event(self: &mut Collector, event: &[u8]);
        fn set_request(self: &mut Collector, method: &[u8], path: &[u8]);
        fn set_status(self: &mut Collector, status: u16);
        fn end(self: &mut Collector) -> EndRecord;
    }
}

const CALL_STARTED: u8 = 0;
const LOG: u8 = 3;

pub struct Collector {
    inner: IssueCollector,
    start: Instant,
    /// The last `report_issue` held its issue and nothing has been reported since.
    open: bool,
}

fn new_collector() -> Box<Collector> {
    Box::new(Collector {
        inner: IssueCollector::default(),
        start: Instant::now(),
        open: false,
    })
}

fn text(bytes: &[u8]) -> Cow<'_, str> {
    String::from_utf8_lossy(bytes)
}

impl Collector {
    fn at_ms(&self) -> u32 {
        u32::try_from(self.start.elapsed().as_millis()).unwrap_or(u32::MAX)
    }

    fn report_issue(&mut self, code: &[u8], message: &[u8]) -> bool {
        self.open = self.inner.report(&text(code), &text(message)).is_some();
        self.open
    }

    fn add_field(&mut self, name: &[u8], value: Value) {
        if self.open
            && let Some(issue) = self.inner.last_held()
        {
            issue.add_field(&text(name), value);
        }
    }

    fn add_field_string(&mut self, name: &[u8], value: &[u8]) {
        self.add_field(name, Value::Str(text(value).into_owned()));
    }

    fn add_field_int(&mut self, name: &[u8], value: i64) {
        self.add_field(name, Value::Int(value));
    }

    fn add_field_float(&mut self, name: &[u8], value: f64) {
        self.add_field(name, Value::Float(value));
    }

    fn add_field_bool(&mut self, name: &[u8], value: bool) {
        self.add_field(name, Value::Bool(value));
    }

    /// An empty `stack` means the error has none.
    fn add_error(&mut self, name: &[u8], message: &[u8], stack: &[u8]) {
        if self.open
            && let Some(issue) = self.inner.last_held()
        {
            let stack = (!stack.is_empty()).then(|| text(stack));
            issue.add_error(&text(name), &text(message), stack.as_deref());
        }
    }

    fn add_attachment(&mut self, name: &[u8], body: &[u8]) {
        if self.open
            && let Some(issue) = self.inner.last_held()
        {
            issue.add_attachment(&text(name), &text(body));
        }
    }

    fn breadcrumb(&mut self, kind: u8, detail: &[u8], label: &[u8]) {
        let at_ms = self.at_ms();
        match kind {
            CALL_STARTED => {
                self.inner
                    .push_breadcrumb(Breadcrumb::call(at_ms, &text(label)));
            }
            LOG => {
                self.inner
                    .push_breadcrumb(Breadcrumb::log(at_ms, &text(detail), &text(label)));
            }
            _ => {}
        }
    }

    fn note_user_context(&mut self, key: &[u8], value: &[u8]) {
        self.inner.note_user_context(&text(key), &text(value));
    }

    fn set_event(&mut self, event: &[u8]) {
        self.inner.set_event(&text(event));
    }

    fn set_request(&mut self, method: &[u8], path: &[u8]) {
        self.inner.set_request(&text(method), &text(path));
    }

    fn set_status(&mut self, status: u16) {
        self.inner.set_status(status);
    }

    fn end(&mut self) -> bridge::EndRecord {
        self.open = false;
        let end = self.inner.end();
        bridge::EndRecord {
            status: end.status.unwrap_or(0),
            event: end.invocation.event,
            method: end.invocation.method,
            path: end.invocation.path,
            issues: end.issues.into_iter().map(issue_record).collect(),
        }
    }
}

fn issue_record(issue: crate::collector::Issue) -> bridge::IssueRecord {
    bridge::IssueRecord {
        code: issue.code,
        message: issue.message,
        fields: issue.fields.into_iter().map(field_record).collect(),
        errors: issue
            .errors
            .into_iter()
            .map(|error| bridge::ErrorRecord {
                name: error.name,
                message: error.message,
                stack: error.stack.unwrap_or_default(),
            })
            .collect(),
        attachments: issue
            .attachments
            .into_iter()
            .map(|attachment| bridge::AttachmentRecord {
                name: attachment.name,
                body: attachment.body,
                truncated: attachment.truncated,
            })
            .collect(),
        breadcrumbs: issue.breadcrumbs.iter().map(breadcrumb_record).collect(),
        user_id: issue.user.user_id.unwrap_or_default(),
        session_id: issue.user.session_id.unwrap_or_default(),
        account_id: issue.user.account_id.unwrap_or_default(),
    }
}

fn field_record(field: crate::collector::Field) -> bridge::FieldRecord {
    let mut record = bridge::FieldRecord {
        name: field.name,
        kind: bridge::FieldKind::Text,
        text: String::new(),
        int_value: 0,
        float_value: 0.0,
        flag: false,
    };
    match field.value {
        Value::Str(text) => record.text = text,
        Value::Int(int) => {
            record.kind = bridge::FieldKind::Int;
            record.int_value = int;
        }
        Value::Float(float) => {
            record.kind = bridge::FieldKind::Float;
            record.float_value = float;
        }
        Value::Bool(flag) => {
            record.kind = bridge::FieldKind::Bool;
            record.flag = flag;
        }
    }
    record
}

fn breadcrumb_record(crumb: &Breadcrumb) -> bridge::BreadcrumbRecord {
    let (is_log, level) = match crumb.kind {
        CrumbKind::Call => (false, String::new()),
        CrumbKind::Log(level) => (true, level.as_str().to_owned()),
    };
    bridge::BreadcrumbRecord {
        at_ms: crumb.at_ms,
        is_log,
        state: match crumb.state {
            State::Started => bridge::CrumbState::Started,
            State::Done => bridge::CrumbState::Done,
            State::Failed => bridge::CrumbState::Failed,
            State::None => bridge::CrumbState::Stateless,
        },
        level,
        label: crumb.label.as_str().to_owned(),
    }
}
