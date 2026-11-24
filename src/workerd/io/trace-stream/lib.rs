// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `trace-stream.c++`. So far it holds the JavaScript values a streaming tail
//! worker receives for its tail events.
//!
//! `TailStreamTarget::report()` receives a batch of `Trace.TailEvent`s. The first batch holds
//! only the onset event, which goes to the worker's `tailStream()` handler; each later event goes
//! to the handler that `tailStream()` returned, either a function or an object whose method for
//! the event is named by [`TailEvent::handler_name`].
//!
//! [`TailEvents::new`] checks the whole batch first, failing on what the C++ decoders of
//! `trace.h` reject (an unknown event type or HTTP method, a stream diagnostics event without a
//! diagnostic, a malformed pointer, text that is not NUL-terminated), so that a bad batch reaches
//! no handler. [`TailEvent::to_js`] can then fail only on what JavaScript rejects (a log message
//! or `cf` object that is not JSON, diagnostics channel data that does not deserialize), on an
//! exception thrown by a setter that a script defined on `Object.prototype`, or on an unknown
//! outcome or log level.

mod ffi;
mod ids;
mod keys;
mod to_js;

use std::fmt;

use capnp::struct_list;
use worker_interface_capnp::tail_stream_target::tail_stream_params;
use worker_interface_capnp::trace;

pub use crate::keys::Keys;

/// The type of a tail event, as far as `TailStreamTarget` needs to know it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// The first event of a tail session, for the `tailStream()` handler.
    Onset,
    /// The last event of a tail session.
    Outcome,
    /// Any other event.
    Other,
}

/// How [`TailEvent::to_js`] converts an event.
#[derive(Clone, Copy, Debug, Default)]
pub struct Options {
    /// Whether the process runs in predictable mode for tests, in which scheduled and alarm
    /// times are the Unix epoch, as `isPredictableModeForTest()` says.
    pub predictable: bool,
}

/// A batch that the C++ decoders of `trace.h` would reject.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DecodeError(String);

impl DecodeError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for DecodeError {}

impl From<capnp::Error> for DecodeError {
    fn from(err: capnp::Error) -> Self {
        Self(err.to_string())
    }
}

/// Why [`TailEvent::to_js`] failed. No exception is pending unless the error is a
/// [`ConvertError::Js`] for which [`jsg::Error::is_pending`] is true.
#[derive(Debug)]
pub enum ConvertError {
    /// JavaScript rejected a value, or V8 failed.
    Js(jsg::Error),
    /// The event has an outcome or log level that the schema does not know, which the C++
    /// conversion rejects with `KJ_UNREACHABLE`.
    Unreachable,
    /// The data of a diagnostics channel event has no header, or one of an unsupported version,
    /// which `jsg::Deserializer` rejects.
    DiagnosticChannelHeader,
    /// The event is one that [`TailEvents::new`] would reject.
    Invalid(DecodeError),
}

impl From<jsg::Error> for ConvertError {
    fn from(err: jsg::Error) -> Self {
        Self::Js(err)
    }
}

impl From<DecodeError> for ConvertError {
    fn from(err: DecodeError) -> Self {
        Self::Invalid(err)
    }
}

impl From<capnp::Error> for ConvertError {
    fn from(err: capnp::Error) -> Self {
        Self::Invalid(err.into())
    }
}

/// A checked batch of tail events.
#[derive(Clone, Copy)]
pub struct TailEvents<'a> {
    events: struct_list::Reader<'a, trace::tail_event::Owned>,
}

impl<'a> TailEvents<'a> {
    /// Checks the events of `params`, as decoding each into a `tracing::TailEvent` does.
    ///
    /// # Errors
    ///
    /// If `params` has no events, or an event is one that the C++ decoders reject: one of a type
    /// the schema does not know, holding a union member it does not know (outside the unions
    /// that the decoders read as empty), an HTTP method past `BAN` or an attribute without a
    /// value, or a stream diagnostics event without a diagnostic.
    pub fn new(params: tail_stream_params::Reader<'a>) -> Result<Self, DecodeError> {
        if !params.has_events() {
            return Err(DecodeError::new("Events are required."));
        }
        let events = params.get_events()?;
        for event in events {
            check::tail_event(event)?;
            check::every_field(event.into())?;
        }
        Ok(Self { events })
    }

    /// The events of `params`, unchecked. [`TailEvent::to_js`] checks the event it converts.
    fn unchecked(params: tail_stream_params::Reader<'a>) -> Result<Self, DecodeError> {
        Ok(Self {
            events: params.get_events()?,
        })
    }

    /// The number of events.
    pub fn len(&self) -> usize {
        self.events.len() as usize
    }

    /// Whether there are no events.
    pub fn is_empty(&self) -> bool {
        self.events.is_empty()
    }

    /// The event at `index`, or `None` if `index` is not less than [`len`](Self::len).
    pub fn get(&self, index: usize) -> Option<TailEvent<'a>> {
        let index = u32::try_from(index).ok()?;
        self.events
            .try_get(index)
            .map(|reader| TailEvent { reader })
    }
}

/// An event of a checked batch.
#[derive(Clone, Copy)]
pub struct TailEvent<'a> {
    reader: trace::tail_event::Reader<'a>,
}

impl TailEvent<'_> {
    /// The event's type.
    pub fn kind(&self) -> Kind {
        match self.reader.get_event().which() {
            Ok(trace::tail_event::event::Onset(_)) => Kind::Onset,
            Ok(trace::tail_event::event::Outcome(_)) => Kind::Outcome,
            _ => Kind::Other,
        }
    }

    /// The name of the method of a handler object that receives the event: `None` for the onset
    /// event, which only the `tailStream()` handler receives.
    pub fn handler_name(&self) -> Option<&'static str> {
        use trace::tail_event::event::Which;
        Some(match self.reader.get_event().which().ok()? {
            Which::Onset(_) => return None,
            Which::Outcome(_) => "outcome",
            Which::SpanOpen(_) => "spanOpen",
            Which::SpanClose(_) => "spanClose",
            Which::SpanUpdate(_) => "spanUpdate",
            Which::SpanLinks(_) => "spanLinks",
            Which::DiagnosticChannelEvent(_) => "diagnosticChannel",
            Which::Exception(_) => "exception",
            Which::Log(_) => "log",
            Which::StreamDiagnostics(_) => "streamDiagEvent",
            Which::Return(_) => "return",
            Which::Attribute(_) => "attributes",
        })
    }

    /// The JavaScript value of the event, which the tail worker's handler receives.
    ///
    /// Sets properties as JavaScript assignments do, in the order the C++ conversion does, so
    /// that a setter defined on `Object.prototype` sees the same calls.
    ///
    /// # Errors
    ///
    /// As described in [`ConvertError`].
    pub fn to_js<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        keys: &mut Keys,
        options: Options,
    ) -> Result<jsg::v8::Local<'s, jsg::v8::Value>, ConvertError> {
        to_js::tail_event(lock, keys, self.reader, options)
    }
}

/// What the C++ decoders of `trace.c++` check.
mod check {
    use worker_interface_capnp::trace;

    use super::DecodeError;

    /// The last HTTP method `kj::HttpMethod` and the decoders know.
    const BAN: u16 = 27;

    fn unknown(what: &str, value: capnp::NotInSchema) -> DecodeError {
        DecodeError::new(format!("unknown {what} {}", value.0))
    }

    /// Reads every field of `value` and of the structs and lists it holds, so that a malformed
    /// pointer, or text that is not NUL-terminated, fails here, as it does where decoding into a
    /// `tracing::TailEvent` reads the field.
    pub fn every_field(value: capnp::dynamic_value::Reader<'_>) -> capnp::Result<()> {
        use capnp::dynamic_value::Reader;
        match value {
            Reader::Struct(value) => {
                let fields = value.get_schema().get_non_union_fields()?.iter();
                for field in fields.chain(value.which()?) {
                    if value.has(field)? {
                        every_field(value.get(field)?)?;
                    }
                }
                Ok(())
            }
            Reader::List(list) => {
                for element in list {
                    every_field(element?)?;
                }
                Ok(())
            }
            _ => Ok(()),
        }
    }

    pub fn tail_event(event: trace::tail_event::Reader<'_>) -> Result<(), DecodeError> {
        use trace::tail_event::event::Which;
        match event
            .get_event()
            .which()
            .map_err(|err| unknown("tail event type", err))?
        {
            Which::Onset(onset) => self::onset(onset?),
            Which::SpanOpen(span_open) => self::span_open(span_open?),
            Which::SpanUpdate(span_update) => self::span_update(span_update?),
            Which::SpanLinks(span_links) => {
                for link in span_links?.get_links()? {
                    attributes(link.get_attributes()?)?;
                }
                Ok(())
            }
            Which::Attribute(list) => attributes(list?),
            Which::Return(ret) => {
                use trace::return_::info::Which;
                match ret?
                    .get_info()
                    .which()
                    .map_err(|err| unknown("return info", err))?
                {
                    Which::Empty(()) | Which::Fetch(_) => Ok(()),
                }
            }
            Which::StreamDiagnostics(diagnostics) => {
                use trace::stream_diagnostics_event::diagnostic::Which;
                match diagnostics?.get_diagnostic().which() {
                    Ok(Which::DroppedEvents(_)) => Ok(()),
                    // The C++ decoder leaves the count of a diagnostic it does not know unset.
                    Ok(Which::Undefined(())) | Err(_) => {
                        Err(DecodeError::new("received invalid diagnostics event"))
                    }
                }
            }
            Which::Outcome(_)
            | Which::SpanClose(_)
            | Which::DiagnosticChannelEvent(_)
            | Which::Exception(_)
            | Which::Log(_) => Ok(()),
        }
    }

    fn onset(onset: trace::onset::Reader<'_>) -> Result<(), DecodeError> {
        use trace::onset::info::Which;
        match onset
            .get_info()?
            .which()
            .map_err(|err| unknown("onset info", err))?
        {
            Which::Fetch(fetch) => fetch_event_info(fetch?)?,
            Which::HibernatableWebSocket(info) => {
                use trace::hibernatable_web_socket_event_info::type_::Which;
                match info?
                    .get_type()
                    .which()
                    .map_err(|err| unknown("hibernatable WebSocket event type", err))?
                {
                    Which::Message(()) | Which::Close(_) | Which::Error(()) => {}
                }
            }
            Which::JsRpc(_)
            | Which::Scheduled(_)
            | Which::Alarm(_)
            | Which::Queue(_)
            | Which::Email(_)
            | Which::Trace(_)
            | Which::Custom(_)
            | Which::Connect(_)
            | Which::OnHibernate(_) => {}
        }
        attributes(onset.get_attributes()?)
    }

    fn span_open(span_open: trace::span_open::Reader<'_>) -> Result<(), DecodeError> {
        use trace::span_open::info::Which;
        match span_open
            .get_info()
            .which()
            .map_err(|err| unknown("span open info", err))?
        {
            Which::Empty(()) | Which::JsRpc(_) => Ok(()),
            Which::Fetch(fetch) => fetch_event_info(fetch?),
            Which::Custom(list) => attributes(list?),
        }
    }

    fn span_update(span_update: trace::span_update::Reader<'_>) -> Result<(), DecodeError> {
        use trace::span_update::info::Which;
        match span_update
            .get_info()
            .which()
            .map_err(|err| unknown("span update info", err))?
        {
            Which::Name(_) | Which::Status(_) => Ok(()),
        }
    }

    pub fn fetch_event_info(fetch: trace::fetch_event_info::Reader<'_>) -> Result<(), DecodeError> {
        let method = match fetch.get_method() {
            Ok(method) => method as u16,
            Err(capnp::NotInSchema(method)) => method,
        };
        if method > BAN {
            return Err(DecodeError::new(format!(
                "unknown method; method = {method}"
            )));
        }
        Ok(())
    }

    pub fn attributes(
        list: capnp::struct_list::Reader<'_, trace::attribute::Owned>,
    ) -> Result<(), DecodeError> {
        for attribute in list {
            if !attribute.has_value() {
                return Err(DecodeError::new("attribute has no value"));
            }
            for value in attribute.get_value()? {
                use trace_capnp::tag_value::Which;
                match value
                    .which()
                    .map_err(|err| unknown("attribute value type", err))?
                {
                    Which::String(_)
                    | Which::Bool(_)
                    | Which::Int64(_)
                    | Which::Float64(_)
                    | Which::StringArray(_)
                    | Which::BoolArray(_)
                    | Which::Float64Array(_) => {}
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
