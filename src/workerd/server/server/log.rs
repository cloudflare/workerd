// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The server's `tracing` events as KJ log lines, so that Rust and C++ log through one logger
//! with one set of levels and one structured-logging format.

use std::fmt::Write as _;
use std::sync::Once;

use tracing::Level;
use tracing::field::Field;
use tracing::field::Visit;
use tracing::span;

use crate::bridge::ffi;

/// `kj::LogSeverity`, without `FATAL`: an event is never fatal.
#[derive(Clone, Copy)]
pub(crate) enum Severity {
    Info = 0,
    Warning = 1,
    Error = 2,
    Dbg = 4,
}

impl Severity {
    fn of(level: Level) -> Self {
        match level {
            Level::ERROR => Self::Error,
            Level::WARN => Self::Warning,
            Level::INFO => Self::Info,
            Level::DEBUG | Level::TRACE => Self::Dbg,
        }
    }
}

/// Renders an event's fields the way `KJ_LOG` renders its arguments: the message first, then
/// `name = value` for every other field, separated by `; `.
#[derive(Default)]
struct Line {
    message: String,
    fields: String,
}

impl Visit for Line {
    fn record_debug(&mut self, field: &Field, value: &dyn std::fmt::Debug) {
        if field.name() == "message" {
            let _ = write!(self.message, "{value:?}");
        } else {
            if !self.fields.is_empty() {
                self.fields.push_str("; ");
            }
            let _ = write!(self.fields, "{} = {value:?}", field.name());
        }
    }

    fn record_str(&mut self, field: &Field, value: &str) {
        if field.name() == "message" {
            self.message.push_str(value);
        } else {
            self.record_debug(field, &value);
        }
    }
}

struct KjLogger;

impl tracing::Subscriber for KjLogger {
    /// KJ always prints its debug severity, which suits the server's own `debug!` lines (the
    /// test runner's results) but not the chatter of the crates it uses (hyper-util narrates
    /// every pooled connection): those are heard from INFO up.
    fn enabled(&self, metadata: &tracing::Metadata<'_>) -> bool {
        *metadata.level() <= Level::INFO || metadata.target().starts_with("workerd_")
    }

    fn new_span(&self, _span: &span::Attributes<'_>) -> span::Id {
        span::Id::from_u64(1)
    }

    fn record(&self, _span: &span::Id, _values: &span::Record<'_>) {}

    fn record_follows_from(&self, _span: &span::Id, _follows: &span::Id) {}

    fn event(&self, event: &tracing::Event<'_>) {
        let mut line = Line::default();
        event.record(&mut line);
        if !line.fields.is_empty() {
            if !line.message.is_empty() {
                line.message.push_str("; ");
            }
            line.message.push_str(&line.fields);
        }
        let metadata = event.metadata();
        ffi::kj_log(
            Severity::of(*metadata.level()) as u8,
            metadata.file().unwrap_or("<rust>"),
            metadata.line().unwrap_or(0),
            &line.message,
        );
    }

    fn enter(&self, _span: &span::Id) {}

    fn exit(&self, _span: &span::Id) {}
}

/// Routes every `tracing` event of the process to KJ's logger. Idempotent.
pub fn install() {
    static INSTALLED: Once = Once::new();
    INSTALLED.call_once(|| {
        // A failure means another subscriber is already in place, which is what a test harness
        // embedding the server would want.
        let _ = tracing::subscriber::set_global_default(KjLogger);
    });
}
