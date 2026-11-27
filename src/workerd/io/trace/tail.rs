// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The events of a streaming tail session, `tracing::TailEvent` of `workerd/io/trace.h`, and their
//! parts: the onset and outcome of the invocation, child spans opening, changing and closing,
//! logs, exceptions, diagnostics channel messages, returns and attributes. Strings are bytes, as
//! elsewhere in the crate.
//!
//! An enum value the schema doesn't know is an error when read, where C++ keeps its number.

use capnp::message::Builder;
use capnp::message::ReaderOptions;
pub use outcome_capnp::EventOutcome;
use worker_interface_capnp::trace;
pub use worker_interface_capnp::trace::ExecutionModel;
pub use worker_interface_capnp::trace::log::Level as LogLevel;

use crate::Attribute;
use crate::EventInfo;
use crate::FetchEventInfo;
use crate::FetchResponseInfo;
use crate::JsRpcEventInfo;
use crate::SpanContext;
use crate::SpanId;
use crate::SpanLinks;
use crate::SpanStatus;
use crate::TraceId;
use crate::TracePreview;
use crate::attribute::list_len;
use crate::event::as_text;
use crate::event::text;

fn attributes_from_capnp(
    list: capnp::struct_list::Reader<'_, trace::attribute::Owned>,
) -> capnp::Result<Vec<Attribute>> {
    list.iter().map(Attribute::from_capnp).collect()
}

fn attributes_to_capnp(
    attributes: &[Attribute],
    mut list: capnp::struct_list::Builder<'_, trace::attribute::Owned>,
) {
    for (i, attribute) in (0..).zip(attributes) {
        attribute.to_capnp(list.reborrow().get(i));
    }
}

fn optional_text(
    present: bool,
    reader: capnp::Result<capnp::text::Reader<'_>>,
) -> capnp::Result<Option<Vec<u8>>> {
    if present {
        text(reader).map(Some)
    } else {
        Ok(None)
    }
}

/// A message published to a `node:diagnostics_channel` channel, `tracing::DiagnosticChannelEvent`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiagnosticChannelEvent {
    /// Nanoseconds since the Unix epoch.
    pub timestamp_ns: i64,
    pub channel: Vec<u8>,
    /// The message, serialized.
    pub message: Vec<u8>,
}

impl DiagnosticChannelEvent {
    pub fn from_capnp(reader: trace::diagnostic_channel_event::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            timestamp_ns: reader.get_timestamp_ns(),
            channel: text(reader.get_channel())?,
            message: reader.get_message()?.to_vec(),
        })
    }

    pub fn to_capnp(&self, mut builder: trace::diagnostic_channel_event::Builder<'_>) {
        builder.set_timestamp_ns(self.timestamp_ns);
        builder.set_channel(as_text(&self.channel));
        builder.set_message(&self.message);
    }
}

/// How many events a session dropped, `tracing::StreamDiagnosticsEvent`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StreamDiagnosticsEvent {
    pub dropped_events_count: u32,
}

impl StreamDiagnosticsEvent {
    /// Reads the event. An `undefined` or unknown diagnostic is an error: C++ throws on the first
    /// and leaves the count unset on the second.
    pub fn from_capnp(reader: trace::stream_diagnostics_event::Reader<'_>) -> capnp::Result<Self> {
        use trace::stream_diagnostics_event::diagnostic;
        match reader.get_diagnostic().which()? {
            diagnostic::Undefined(()) => Err(capnp::Error::failed(
                "received invalid diagnostics event".into(),
            )),
            diagnostic::DroppedEvents(dropped) => Ok(Self {
                dropped_events_count: dropped?.get_count(),
            }),
        }
    }

    pub fn to_capnp(self, builder: trace::stream_diagnostics_event::Builder<'_>) {
        builder
            .init_diagnostic()
            .init_dropped_events()
            .set_count(self.dropped_events_count);
    }
}

/// The fields of an `Error` passed to a `console` call, `tracing::ErrorInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ErrorInfo {
    pub name: Vec<u8>,
    pub message: Vec<u8>,
    pub stack: Option<Vec<u8>>,
}

impl ErrorInfo {
    pub fn from_capnp(reader: trace::error_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            name: text(reader.get_name())?,
            message: text(reader.get_message())?,
            stack: optional_text(reader.has_stack(), reader.get_stack())?,
        })
    }

    pub fn to_capnp(&self, mut builder: trace::error_info::Builder<'_>) {
        builder.set_name(as_text(&self.name));
        builder.set_message(as_text(&self.message));
        if let Some(stack) = &self.stack {
            builder.set_stack(as_text(stack));
        }
    }
}

/// A `console` call, `tracing::Log`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Log {
    /// Nanoseconds since the Unix epoch.
    pub timestamp_ns: i64,
    pub level: LogLevel,
    pub message: Vec<u8>,
    /// Whether `message` was cut short.
    pub truncated: bool,
    /// One slot per argument of the call, holding the argument's fields if it was an `Error`;
    /// `None` if no argument was.
    pub error_info: Option<Vec<Option<ErrorInfo>>>,
}

impl Log {
    /// Reads the log. A slot whose variant is unknown reads as `None`, as in C++.
    pub fn from_capnp(reader: trace::log::Reader<'_>) -> capnp::Result<Self> {
        use trace::error_info_slot;
        let error_info = if reader.has_error_info() {
            Some(
                reader
                    .get_error_info()?
                    .iter()
                    .map(|slot| match slot.which() {
                        Ok(error_info_slot::Info(info)) => ErrorInfo::from_capnp(info?).map(Some),
                        Ok(error_info_slot::None(())) | Err(_) => Ok(None),
                    })
                    .collect::<capnp::Result<_>>()?,
            )
        } else {
            None
        };
        Ok(Self {
            timestamp_ns: reader.get_timestamp_ns(),
            level: reader.get_log_level()?,
            message: text(reader.get_message())?,
            truncated: reader.get_truncated(),
            error_info,
        })
    }

    pub fn to_capnp(&self, mut builder: trace::log::Builder<'_>) {
        builder.set_timestamp_ns(self.timestamp_ns);
        builder.set_log_level(self.level);
        builder.set_message(as_text(&self.message));
        builder.set_truncated(self.truncated);
        if let Some(slots) = &self.error_info {
            let mut list = builder.init_error_info(list_len(slots.len()));
            for (i, slot) in (0..).zip(slots) {
                let mut element = list.reborrow().get(i);
                match slot {
                    Some(info) => info.to_capnp(element.init_info()),
                    None => element.set_none(()),
                }
            }
        }
    }
}

/// The `code` of an exception.
#[derive(Clone, Debug, PartialEq)]
pub enum ExceptionCode {
    Text(Vec<u8>),
    Number(f64),
}

/// An uncaught exception, `tracing::Exception`.
#[derive(Clone, Debug, PartialEq)]
pub struct Exception {
    /// Nanoseconds since the Unix epoch.
    pub timestamp_ns: i64,
    pub name: Vec<u8>,
    pub message: Vec<u8>,
    pub stack: Option<Vec<u8>>,
    pub code: Option<ExceptionCode>,
}

impl Exception {
    /// Reads the exception. A code whose variant is unknown reads as `None`, as in C++.
    pub fn from_capnp(reader: trace::exception::Reader<'_>) -> capnp::Result<Self> {
        use trace::exception::code;
        Ok(Self {
            timestamp_ns: reader.get_timestamp_ns(),
            name: text(reader.get_name())?,
            message: text(reader.get_message())?,
            stack: optional_text(reader.has_stack(), reader.get_stack())?,
            code: match reader.get_code().which() {
                Ok(code::Text(code)) => Some(ExceptionCode::Text(text(code)?)),
                Ok(code::Number(code)) => Some(ExceptionCode::Number(code)),
                Ok(code::None(())) | Err(_) => None,
            },
        })
    }

    pub fn to_capnp(&self, mut builder: trace::exception::Builder<'_>) {
        builder.set_timestamp_ns(self.timestamp_ns);
        builder.set_name(as_text(&self.name));
        builder.set_message(as_text(&self.message));
        if let Some(stack) = &self.stack {
            builder.set_stack(as_text(stack));
        }
        match &self.code {
            Some(ExceptionCode::Text(code)) => builder.init_code().set_text(as_text(code)),
            Some(ExceptionCode::Number(code)) => builder.init_code().set_number(*code),
            None => {}
        }
    }
}

/// The point at which a span's operation returned, `tracing::Return`; for a fetch, with its
/// response.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Return {
    pub info: Option<FetchResponseInfo>,
}

impl Return {
    pub fn from_capnp(reader: trace::return_::Reader<'_>) -> capnp::Result<Self> {
        use trace::return_::info;
        Ok(Self {
            info: match reader.get_info().which()? {
                info::Empty(()) => None,
                info::Fetch(fetch) => Some(FetchResponseInfo::from_capnp(fetch?)),
            },
        })
    }

    pub fn to_capnp(self, builder: trace::return_::Builder<'_>) {
        if let Some(fetch) = self.info {
            fetch.to_capnp(builder.init_info().init_fetch());
        }
    }
}

/// What a child span is for, if it is a subrequest or has attributes from the start.
#[derive(Clone, Debug, PartialEq)]
pub enum SpanOpenInfo {
    Fetch(FetchEventInfo),
    JsRpc(JsRpcEventInfo),
    Custom(Vec<Attribute>),
}

/// A child span opening, `tracing::SpanOpen`.
#[derive(Clone, Debug, PartialEq)]
pub struct SpanOpen {
    pub operation_name: Vec<u8>,
    pub info: Option<SpanOpenInfo>,
    pub span_id: SpanId,
}

impl SpanOpen {
    pub fn from_capnp(reader: trace::span_open::Reader<'_>) -> capnp::Result<Self> {
        use trace::span_open::info;
        Ok(Self {
            operation_name: text(reader.get_operation_name())?,
            info: match reader.get_info().which()? {
                info::Empty(()) => None,
                info::Fetch(fetch) => {
                    Some(SpanOpenInfo::Fetch(FetchEventInfo::from_capnp(fetch?)?))
                }
                info::JsRpc(js_rpc) => {
                    Some(SpanOpenInfo::JsRpc(JsRpcEventInfo::from_capnp(js_rpc?)?))
                }
                info::Custom(custom) => Some(SpanOpenInfo::Custom(attributes_from_capnp(custom?)?)),
            },
            span_id: SpanId(reader.get_span_id()),
        })
    }

    pub fn to_capnp(&self, mut builder: trace::span_open::Builder<'_>) {
        builder.set_operation_name(as_text(&self.operation_name));
        builder.set_span_id(self.span_id.0);
        if let Some(info) = &self.info {
            let info_builder = builder.init_info();
            match info {
                SpanOpenInfo::Fetch(fetch) => fetch.to_capnp(info_builder.init_fetch()),
                SpanOpenInfo::JsRpc(js_rpc) => js_rpc.to_capnp(info_builder.init_js_rpc()),
                SpanOpenInfo::Custom(custom) => {
                    attributes_to_capnp(custom, info_builder.init_custom(list_len(custom.len())));
                }
            }
        }
    }
}

/// A child span closing, `tracing::SpanClose`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SpanClose {
    pub outcome: EventOutcome,
}

impl SpanClose {
    pub fn from_capnp(reader: trace::span_close::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            outcome: reader.get_outcome()?,
        })
    }

    pub fn to_capnp(self, mut builder: trace::span_close::Builder<'_>) {
        builder.set_outcome(self.outcome);
    }
}

/// A change to a span, `tracing::SpanUpdate`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SpanUpdate {
    Name(Vec<u8>),
    Status(SpanStatus),
}

impl SpanUpdate {
    pub fn from_capnp(reader: trace::span_update::Reader<'_>) -> capnp::Result<Self> {
        use trace::span_update::info;
        Ok(match reader.get_info().which()? {
            info::Name(name) => Self::Name(text(name)?),
            info::Status(status) => Self::Status(SpanStatus::from_capnp(status?)?),
        })
    }

    pub fn to_capnp(&self, builder: trace::span_update::Builder<'_>) {
        let mut info = builder.init_info();
        match self {
            Self::Name(name) => info.set_name(as_text(name)),
            Self::Status(status) => status.to_capnp(info.init_status()),
        }
    }
}

/// The version of the script being tailed, kept as the message it came in, as C++ does, so that
/// fields this crate doesn't know survive.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ScriptVersion {
    /// A flat single-segment message whose root is the `ScriptVersion`.
    message: Vec<u8>,
}

impl ScriptVersion {
    pub fn from_capnp(
        reader: script_version_capnp::script_version::Reader<'_>,
    ) -> capnp::Result<Self> {
        let mut message = Builder::new_default();
        message.set_root(reader)?;
        Ok(Self {
            message: capnp::serialize::write_message_to_words(&message),
        })
    }

    /// Calls `f` with the version.
    pub fn read<R>(
        &self,
        f: impl FnOnce(script_version_capnp::script_version::Reader<'_>) -> R,
    ) -> capnp::Result<R> {
        let mut slice = self.message.as_slice();
        let message =
            capnp::serialize::read_message_from_flat_slice(&mut slice, ReaderOptions::new())?;
        Ok(f(message.get_root()?))
    }
}

/// The worker being tailed, `tracing::Onset::WorkerInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WorkerInfo {
    pub execution_model: ExecutionModel,
    pub script_name: Option<Vec<u8>>,
    pub script_version: Option<ScriptVersion>,
    pub preview: Option<TracePreview>,
    pub dispatch_namespace: Option<Vec<u8>>,
    pub script_id: Option<Vec<u8>>,
    pub script_tags: Option<Vec<Vec<u8>>>,
    pub entrypoint: Option<Vec<u8>>,
    pub durable_object_id: Option<Vec<u8>>,
}

/// The start of the invocation, the first event of a session, `tracing::Onset`.
#[derive(Clone, Debug, PartialEq)]
pub struct Onset {
    pub span_id: SpanId,
    pub info: EventInfo,
    pub worker: WorkerInfo,
    pub attributes: Vec<Attribute>,
}

impl Onset {
    pub fn from_capnp(reader: trace::onset::Reader<'_>) -> capnp::Result<Self> {
        let script_tags = if reader.has_script_tags() {
            Some(
                reader
                    .get_script_tags()?
                    .iter()
                    .map(text)
                    .collect::<capnp::Result<_>>()?,
            )
        } else {
            None
        };
        Ok(Self {
            span_id: SpanId(reader.get_span_id()),
            info: EventInfo::from_capnp(reader.get_info()?)?,
            worker: WorkerInfo {
                execution_model: reader.get_execution_model()?,
                script_name: optional_text(reader.has_script_name(), reader.get_script_name())?,
                script_version: if reader.has_script_version() {
                    Some(ScriptVersion::from_capnp(reader.get_script_version()?)?)
                } else {
                    None
                },
                preview: if reader.has_preview() {
                    Some(TracePreview::from_capnp(reader.get_preview()?)?)
                } else {
                    None
                },
                dispatch_namespace: optional_text(
                    reader.has_dispatch_namespace(),
                    reader.get_dispatch_namespace(),
                )?,
                script_id: optional_text(reader.has_script_id(), reader.get_script_id())?,
                script_tags,
                entrypoint: optional_text(reader.has_entry_point(), reader.get_entry_point())?,
                durable_object_id: optional_text(
                    reader.has_durable_object_id(),
                    reader.get_durable_object_id(),
                )?,
            },
            attributes: attributes_from_capnp(reader.get_attributes()?)?,
        })
    }

    /// Writes the onset. Only copying the script version can fail, if its message is invalid.
    pub fn to_capnp(&self, mut builder: trace::onset::Builder<'_>) -> capnp::Result<()> {
        let worker = &self.worker;
        builder.set_execution_model(worker.execution_model);
        builder.set_span_id(self.span_id.0);
        if let Some(name) = &worker.script_name {
            builder.set_script_name(as_text(name));
        }
        if let Some(version) = &worker.script_version {
            version.read(|version| builder.set_script_version(version))??;
        }
        if let Some(namespace) = &worker.dispatch_namespace {
            builder.set_dispatch_namespace(as_text(namespace));
        }
        if let Some(id) = &worker.script_id {
            builder.set_script_id(as_text(id));
        }
        if let Some(tags) = &worker.script_tags {
            let mut list = builder.reborrow().init_script_tags(list_len(tags.len()));
            for (i, tag) in (0..).zip(tags) {
                list.set(i, as_text(tag));
            }
        }
        if let Some(entrypoint) = &worker.entrypoint {
            builder.set_entry_point(as_text(entrypoint));
        }
        if let Some(preview) = &worker.preview {
            preview.to_capnp(builder.reborrow().init_preview());
        }
        if let Some(id) = &worker.durable_object_id {
            builder.set_durable_object_id(as_text(id));
        }
        self.info.to_capnp(builder.reborrow().init_info());
        attributes_to_capnp(
            &self.attributes,
            builder.init_attributes(list_len(self.attributes.len())),
        );
        Ok(())
    }
}

/// The end of the invocation, the last event of a session, `tracing::Outcome`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Outcome {
    pub outcome: EventOutcome,
    /// C++ holds the times as `kj::Duration`s in nanoseconds, so it cannot hold more than about
    /// 292 years of either.
    pub cpu_time_ms: u64,
    pub wall_time_ms: u64,
}

impl Outcome {
    pub fn from_capnp(reader: trace::outcome::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            outcome: reader.get_outcome()?,
            cpu_time_ms: reader.get_cpu_time(),
            wall_time_ms: reader.get_wall_time(),
        })
    }

    pub fn to_capnp(self, mut builder: trace::outcome::Builder<'_>) {
        builder.set_outcome(self.outcome);
        builder.set_cpu_time(self.cpu_time_ms);
        builder.set_wall_time(self.wall_time_ms);
    }
}

/// What happened, `tracing::TailEvent::Event`.
#[derive(Clone, Debug, PartialEq)]
pub enum TailEventKind {
    /// Boxed, as in C++, because it is several times larger than any other event.
    Onset(Box<Onset>),
    Outcome(Outcome),
    SpanOpen(SpanOpen),
    SpanClose(SpanClose),
    SpanUpdate(SpanUpdate),
    SpanLinks(SpanLinks),
    DiagnosticChannel(DiagnosticChannelEvent),
    Exception(Exception),
    Log(Log),
    StreamDiagnostics(StreamDiagnosticsEvent),
    Return(Return),
    Attributes(Vec<Attribute>),
}

/// One event of a streaming tail session, `tracing::TailEvent`.
#[derive(Clone, Debug, PartialEq)]
pub struct TailEvent {
    /// The span the event belongs to.
    pub span_context: SpanContext,
    pub invocation_id: TraceId,
    /// Nanoseconds since the Unix epoch.
    pub timestamp_ns: i64,
    /// The event's place in the session, from zero.
    pub sequence: u32,
    pub event: TailEventKind,
}

impl TailEvent {
    /// Reads the event. An unknown variant is an error, as C++ treats it as unreachable.
    pub fn from_capnp(reader: trace::tail_event::Reader<'_>) -> capnp::Result<Self> {
        use trace::tail_event::event::Which;
        let event = match reader.get_event().which()? {
            Which::Onset(onset) => TailEventKind::Onset(Box::new(Onset::from_capnp(onset?)?)),
            Which::Outcome(outcome) => TailEventKind::Outcome(Outcome::from_capnp(outcome?)?),
            Which::SpanOpen(open) => TailEventKind::SpanOpen(SpanOpen::from_capnp(open?)?),
            Which::SpanClose(close) => TailEventKind::SpanClose(SpanClose::from_capnp(close?)?),
            Which::SpanUpdate(update) => {
                TailEventKind::SpanUpdate(SpanUpdate::from_capnp(update?)?)
            }
            Which::SpanLinks(links) => TailEventKind::SpanLinks(SpanLinks::from_capnp(links?)?),
            Which::Attribute(attributes) => {
                TailEventKind::Attributes(attributes_from_capnp(attributes?)?)
            }
            Which::Return(ret) => TailEventKind::Return(Return::from_capnp(ret?)?),
            Which::DiagnosticChannelEvent(event) => {
                TailEventKind::DiagnosticChannel(DiagnosticChannelEvent::from_capnp(event?)?)
            }
            Which::Exception(exception) => {
                TailEventKind::Exception(Exception::from_capnp(exception?)?)
            }
            Which::Log(log) => TailEventKind::Log(Log::from_capnp(log?)?),
            Which::StreamDiagnostics(event) => {
                TailEventKind::StreamDiagnostics(StreamDiagnosticsEvent::from_capnp(event?)?)
            }
        };
        Ok(Self {
            span_context: SpanContext::from_capnp(reader.get_span_context()?)?,
            invocation_id: TraceId::from_capnp(reader.get_invocation_id()?),
            timestamp_ns: reader.get_timestamp_ns(),
            sequence: reader.get_sequence(),
            event,
        })
    }

    /// Writes the event. Only an onset's script version can fail to copy, if its message is
    /// invalid.
    pub fn to_capnp(&self, mut builder: trace::tail_event::Builder<'_>) -> capnp::Result<()> {
        self.span_context
            .to_capnp(builder.reborrow().init_span_context());
        self.invocation_id
            .to_capnp(builder.reborrow().init_invocation_id());
        builder.set_timestamp_ns(self.timestamp_ns);
        builder.set_sequence(self.sequence);
        let event = builder.init_event();
        match &self.event {
            TailEventKind::Onset(onset) => onset.to_capnp(event.init_onset())?,
            TailEventKind::Outcome(outcome) => outcome.to_capnp(event.init_outcome()),
            TailEventKind::SpanOpen(open) => open.to_capnp(event.init_span_open()),
            TailEventKind::SpanClose(close) => close.to_capnp(event.init_span_close()),
            TailEventKind::SpanUpdate(update) => update.to_capnp(event.init_span_update()),
            TailEventKind::SpanLinks(links) => links.to_capnp(event.init_span_links()),
            TailEventKind::DiagnosticChannel(diagnostic) => {
                diagnostic.to_capnp(event.init_diagnostic_channel_event());
            }
            TailEventKind::Exception(exception) => exception.to_capnp(event.init_exception()),
            TailEventKind::Log(log) => log.to_capnp(event.init_log()),
            TailEventKind::StreamDiagnostics(diagnostics) => {
                diagnostics.to_capnp(event.init_stream_diagnostics());
            }
            TailEventKind::Return(ret) => ret.to_capnp(event.init_return()),
            TailEventKind::Attributes(attributes) => {
                attributes_to_capnp(attributes, event.init_attribute(list_len(attributes.len())));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
#[path = "tail-test.rs"]
mod tests;
