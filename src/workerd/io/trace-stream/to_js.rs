// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The conversion of tail events to JavaScript values.
//!
//! Each function builds an object and sets its properties in the order the C++ conversion in
//! `trace-stream.c++` does, converting a property's value just before setting it, so that the
//! setters a script defined on `Object.prototype` see the same calls in the same order with
//! either conversion.

use jsg::Lock;
use jsg::v8;
use jsg::v8::Local;
use worker_interface_capnp::trace;

use crate::ConvertError;
use crate::DecodeError;
use crate::Options;
use crate::ids;
use crate::keys::Key;
use crate::keys::Keys;

type Result<T> = std::result::Result<T, ConvertError>;
type Value<'s> = Local<'s, v8::Value>;

/// The names of `kj::HttpMethod`, indexed by `capnp::HttpMethod`, which lists them in the same
/// order.
const HTTP_METHODS: [&str; 28] = [
    "GET",
    "HEAD",
    "POST",
    "PUT",
    "DELETE",
    "PATCH",
    "PURGE",
    "OPTIONS",
    "TRACE",
    "COPY",
    "LOCK",
    "MKCOL",
    "MOVE",
    "PROPFIND",
    "PROPPATCH",
    "SEARCH",
    "UNLOCK",
    "ACL",
    "REPORT",
    "MKACTIVITY",
    "CHECKOUT",
    "MERGE",
    "MSEARCH",
    "NOTIFY",
    "SUBSCRIBE",
    "UNSUBSCRIBE",
    "QUERY",
    "BAN",
];

const NANOS_PER_MILLI: i64 = 1_000_000;

fn unknown(what: &str, value: capnp::NotInSchema) -> ConvertError {
    ConvertError::Invalid(DecodeError::new(format!("unknown {what} {}", value.0)))
}

/// A string of UTF-8 `bytes`, in which each invalid sequence becomes U+FFFD, as `jsg::Lock::str()`
/// makes it.
fn str<'s>(lock: &mut Lock<'s>, bytes: &[u8]) -> Result<Value<'s>> {
    Ok(v8::String::new_from_utf8(lock, bytes)
        .ok_or_else(jsg::Error::invalid_string_length)?
        .into())
}

/// An internalized string, as `StringCache::get()` gives it.
fn name<'s>(lock: &mut Lock<'s>, name: &str) -> Result<Value<'s>> {
    Ok(v8::String::new_internalized_from_str(lock, name)
        .ok_or_else(jsg::Error::invalid_string_length)?
        .into())
}

fn text<'s>(
    lock: &mut Lock<'s>,
    text: capnp::Result<capnp::text::Reader<'_>>,
) -> Result<Value<'s>> {
    str(lock, text?.as_bytes())
}

fn num<'s>(lock: &mut Lock<'s>, value: f64) -> Value<'s> {
    v8::Number::new(lock, value).into()
}

/// A date `millis` milliseconds after the Unix epoch, as `jsg::Lock::date(double)` makes it.
fn date<'s>(lock: &mut Lock<'s>, millis: f64) -> Result<Value<'s>> {
    Ok(v8::Date::new(lock, millis)?.into())
}

/// A date `nanos` nanoseconds after the Unix epoch, truncated to milliseconds toward zero, as
/// `jsg::Lock::date(kj::Date)` makes it.
fn date_from_nanos<'s>(lock: &mut Lock<'s>, nanos: i64) -> Result<Value<'s>> {
    #[expect(clippy::cast_precision_loss, reason = "a JavaScript date is a double")]
    date(lock, (nanos / NANOS_PER_MILLI) as f64)
}

/// The JSON value of UTF-8 `bytes`, as `jsg::Lock::parseJson()` parses it.
fn parse_json<'s>(lock: &mut Lock<'s>, bytes: &[u8]) -> Result<Value<'s>> {
    Ok(jsg::json::parse(lock, &String::from_utf8_lossy(bytes))?)
}

/// The name of a schema enumerant, or its number if the schema does not know it, as `kj::str()`
/// writes a Cap'n Proto enum.
fn enumerant<T>(
    value: std::result::Result<T, capnp::NotInSchema>,
    name: fn(T) -> &'static str,
) -> String {
    match value {
        Ok(value) => name(value).to_owned(),
        Err(capnp::NotInSchema(number)) => number.to_string(),
    }
}

/// The values `f` makes of `items`, in a vector allocated once for all of them: collecting the
/// results directly would grow the vector, since a fallible iterator knows no lower bound.
fn collect<'s, T>(
    items: impl Iterator<Item = T>,
    mut f: impl FnMut(T) -> Result<Value<'s>>,
) -> Result<Vec<Value<'s>>> {
    let mut values = Vec::with_capacity(items.size_hint().0);
    for item in items {
        values.push(f(item)?);
    }
    Ok(values)
}

fn array<'s>(lock: &mut Lock<'s>, elements: &[Value<'s>]) -> Value<'s> {
    v8::Array::with_elements(lock, elements).into()
}

pub fn tail_event<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    event: trace::tail_event::Reader<'_>,
    options: Options,
) -> Result<Value<'s>> {
    use trace::tail_event::event::Which;
    let obj = lock.new_object();

    let span_context = event.get_span_context()?;
    let context = lock.new_object();
    let trace_id = span_context.get_trace_id()?;
    let value = str(
        lock,
        ids::trace_id_go(trace_id.get_high(), trace_id.get_low()).as_bytes(),
    )?;
    keys.set(lock, context, Key::TraceId, value)?;
    if let Ok(worker_interface_capnp::span_context::info::SpanId(span_id)) =
        span_context.get_info().which()
    {
        let value = str(lock, ids::span_id_go(span_id).as_bytes())?;
        keys.set(lock, context, Key::SpanId, value)?;
    }
    if let Some(flags) = trace_flags(
        span_context.has_trace_flags(),
        span_context.get_trace_flags(),
    )? {
        let value = num(lock, f64::from(flags));
        keys.set(lock, context, Key::TraceFlags, value)?;
    }
    keys.set(lock, obj, Key::SpanContext, context.into())?;

    let invocation_id = event.get_invocation_id()?;
    let value = str(
        lock,
        ids::trace_id_go(invocation_id.get_high(), invocation_id.get_low()).as_bytes(),
    )?;
    keys.set(lock, obj, Key::InvocationId, value)?;
    let value = date_from_nanos(lock, event.get_timestamp_ns())?;
    keys.set(lock, obj, Key::Timestamp, value)?;
    let value = num(lock, f64::from(event.get_sequence()));
    keys.set(lock, obj, Key::Sequence, value)?;

    let value = match event
        .get_event()
        .which()
        .map_err(|err| unknown("tail event type", err))?
    {
        Which::Onset(onset) => self::onset(lock, keys, onset?, options)?,
        Which::Outcome(outcome) => self::outcome(lock, keys, outcome?)?,
        Which::SpanOpen(span_open) => self::span_open(lock, keys, span_open?)?,
        Which::SpanClose(span_close) => self::span_close(lock, keys, span_close?)?,
        Which::SpanUpdate(span_update) => self::span_update(lock, keys, span_update?)?,
        Which::SpanLinks(span_links) => self::span_links(lock, keys, span_links?)?,
        Which::DiagnosticChannelEvent(event) => diagnostic_channel_event(lock, keys, event?)?,
        Which::Exception(exception) => self::exception(lock, keys, exception?)?,
        Which::Log(log) => self::log(lock, keys, log?)?,
        Which::StreamDiagnostics(diagnostics) => stream_diagnostics(lock, keys, diagnostics?)?,
        Which::Return(ret) => self::ret(lock, keys, ret?)?,
        Which::Attribute(list) => attributes_event(lock, keys, list?)?,
    };
    keys.set(lock, obj, Key::Event, value)?;
    Ok(obj.into())
}

/// The trace flags of a span context, if set, as `SpanContext::fromCapnp()` reads them.
fn trace_flags(
    has: bool,
    flags: capnp::Result<worker_interface_capnp::trace_flags::Reader<'_>>,
) -> Result<Option<u8>> {
    if !has {
        return Ok(None);
    }
    match flags?.get_value().which() {
        Ok(worker_interface_capnp::trace_flags::value::Set(flags)) => Ok(Some(flags)),
        _ => Ok(None),
    }
}

fn tag_value<'s>(
    lock: &mut Lock<'s>,
    value: trace_capnp::tag_value::Reader<'_>,
) -> Result<Value<'s>> {
    use trace_capnp::tag_value::Which;
    Ok(
        match value
            .which()
            .map_err(|err| unknown("attribute value type", err))?
        {
            Which::String(string) => text(lock, string)?,
            Which::Bool(value) => v8::Boolean::new(lock, value).into(),
            Which::Float64(value) => num(lock, value),
            Which::Int64(value) => v8::BigInt::new(lock, value).into(),
            // An empty element is null, as is one of a kind the schema doesn't know, which
            // `deserializeTagValue()` leaves empty.
            Which::StringArray(list) => {
                let elements = collect(list?.iter(), |element| match element.which() {
                    Ok(trace_capnp::optional_string::Value(value)) => text(lock, value),
                    Ok(trace_capnp::optional_string::None(())) | Err(_) => Ok(lock.null()),
                })?;
                array(lock, &elements)
            }
            Which::BoolArray(list) => {
                let elements = collect(list?.iter(), |element| match element.which() {
                    Ok(trace_capnp::optional_bool::Value(value)) => {
                        Ok(v8::Boolean::new(lock, value).into())
                    }
                    Ok(trace_capnp::optional_bool::None(())) | Err(_) => Ok(lock.null()),
                })?;
                array(lock, &elements)
            }
            Which::Float64Array(list) => {
                let elements = collect(list?.iter(), |element| match element.which() {
                    Ok(trace_capnp::optional_float64::Value(value)) => Ok(num(lock, value)),
                    Ok(trace_capnp::optional_float64::None(())) | Err(_) => Ok(lock.null()),
                })?;
                array(lock, &elements)
            }
        },
    )
}

fn attribute<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    attribute: trace::attribute::Reader<'_>,
) -> Result<Value<'s>> {
    if !attribute.has_value() {
        return Err(DecodeError::new("attribute has no value").into());
    }
    let obj = lock.new_object();
    let value = text(lock, attribute.get_name())?;
    keys.set(lock, obj, Key::Name, value)?;
    let values = attribute.get_value()?;
    let value = if values.len() == 1 {
        tag_value(lock, values.get(0))?
    } else {
        let elements = collect(values.iter(), |value| tag_value(lock, value))?;
        array(lock, &elements)
    };
    keys.set(lock, obj, Key::Value, value)?;
    Ok(obj.into())
}

fn attribute_list<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    list: capnp::struct_list::Reader<'_, trace::attribute::Owned>,
) -> Result<Value<'s>> {
    let elements = collect(list.iter(), |attr| attribute(lock, keys, attr))?;
    Ok(array(lock, &elements))
}

fn attributes_event<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    list: capnp::struct_list::Reader<'_, trace::attribute::Owned>,
) -> Result<Value<'s>> {
    let obj = lock.new_object();
    let value = name(lock, "attributes")?;
    keys.set(lock, obj, Key::Type, value)?;
    let value = attribute_list(lock, keys, list)?;
    keys.set(lock, obj, Key::Info, value)?;
    Ok(obj.into())
}

/// An object with only a `type`.
fn typed<'s>(lock: &mut Lock<'s>, keys: &mut Keys, type_: &str) -> Result<Value<'s>> {
    let obj = lock.new_object();
    let value = name(lock, type_)?;
    keys.set(lock, obj, Key::Type, value)?;
    Ok(obj.into())
}

fn fetch_event_info<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    info: trace::fetch_event_info::Reader<'_>,
) -> Result<Value<'s>> {
    crate::check::fetch_event_info(info)?;
    let obj = lock.new_object();
    let value = name(lock, "fetch")?;
    keys.set(lock, obj, Key::Type, value)?;
    let method = match info.get_method() {
        Ok(method) => method as usize,
        Err(capnp::NotInSchema(method)) => usize::from(method),
    };
    let value = name(lock, HTTP_METHODS[method])?;
    keys.set(lock, obj, Key::Method, value)?;
    let value = text(lock, info.get_url())?;
    keys.set(lock, obj, Key::Url, value)?;
    let cf_json = info.get_cf_json()?.as_bytes();
    if !cf_json.is_empty() {
        let value = parse_json(lock, cf_json)?;
        keys.set(lock, obj, Key::CfJson, value)?;
    }
    let headers = collect(info.get_headers()?.iter(), |header| {
        let obj = lock.new_object();
        let value = str(lock, header.get_name()?.as_bytes())?;
        keys.set(lock, obj, Key::Name, value)?;
        let value = text(lock, header.get_value())?;
        keys.set(lock, obj, Key::Value, value)?;
        Ok(obj.into())
    })?;
    let value = array(lock, &headers);
    keys.set(lock, obj, Key::Headers, value)?;
    Ok(obj.into())
}

fn scheduled_time<'s>(lock: &mut Lock<'s>, options: Options, millis: f64) -> Result<Value<'s>> {
    date(lock, if options.predictable { 0.0 } else { millis })
}

fn onset_info<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    info: trace::onset::info::Reader<'_>,
    options: Options,
) -> Result<Value<'s>> {
    use trace::onset::info::Which;
    Ok(
        match info.which().map_err(|err| unknown("onset info", err))? {
            Which::Fetch(fetch) => fetch_event_info(lock, keys, fetch?)?,
            Which::JsRpc(_) => typed(lock, keys, "jsrpc")?,
            Which::Scheduled(scheduled) => {
                let scheduled = scheduled?;
                let obj = lock.new_object();
                let value = name(lock, "scheduled")?;
                keys.set(lock, obj, Key::Type, value)?;
                let value = scheduled_time(lock, options, scheduled.get_scheduled_time())?;
                keys.set(lock, obj, Key::ScheduledTime, value)?;
                let value = text(lock, scheduled.get_cron())?;
                keys.set(lock, obj, Key::Cron, value)?;
                obj.into()
            }
            Which::Alarm(alarm) => {
                // AlarmEventInfo holds a kj::Date, in nanoseconds.
                let millis =
                    alarm?.get_scheduled_time_ms().wrapping_mul(NANOS_PER_MILLI) / NANOS_PER_MILLI;
                let obj = lock.new_object();
                let value = name(lock, "alarm")?;
                keys.set(lock, obj, Key::Type, value)?;
                #[expect(clippy::cast_precision_loss, reason = "a JavaScript date is a double")]
                let value = scheduled_time(lock, options, millis as f64)?;
                keys.set(lock, obj, Key::ScheduledTime, value)?;
                obj.into()
            }
            Which::OnHibernate(info) => {
                use trace::on_hibernate_event_info::Reason;
                let reason = enumerant(info?.get_reason(), |reason| match reason {
                    Reason::Inactive => "inactive",
                    Reason::CodeUpdated => "codeUpdated",
                    Reason::System => "system",
                });
                let obj = lock.new_object();
                let value = name(lock, "onHibernate")?;
                keys.set(lock, obj, Key::Type, value)?;
                let value = name(lock, &reason)?;
                keys.set(lock, obj, Key::Reason, value)?;
                obj.into()
            }
            Which::Queue(queue) => {
                let queue = queue?;
                let obj = lock.new_object();
                let value = name(lock, "queue")?;
                keys.set(lock, obj, Key::Type, value)?;
                let value = text(lock, queue.get_queue_name())?;
                keys.set(lock, obj, Key::QueueName, value)?;
                let value = num(lock, f64::from(queue.get_batch_size()));
                keys.set(lock, obj, Key::BatchSize, value)?;
                obj.into()
            }
            Which::Email(email) => {
                let email = email?;
                let obj = lock.new_object();
                let value = name(lock, "email")?;
                keys.set(lock, obj, Key::Type, value)?;
                let value = text(lock, email.get_mail_from())?;
                keys.set(lock, obj, Key::MailFrom, value)?;
                let value = text(lock, email.get_rcpt_to())?;
                keys.set(lock, obj, Key::RcptTo, value)?;
                let value = num(lock, f64::from(email.get_raw_size()));
                keys.set(lock, obj, Key::RawSize, value)?;
                obj.into()
            }
            Which::Trace(info) => {
                let obj = lock.new_object();
                let value = name(lock, "trace")?;
                keys.set(lock, obj, Key::Type, value)?;
                // A decoded TraceItem always has a script name.
                let traces = collect(info?.get_traces()?.iter(), |item| {
                    text(lock, item.get_script_name())
                })?;
                let value = array(lock, &traces);
                keys.set(lock, obj, Key::Traces, value)?;
                obj.into()
            }
            Which::HibernatableWebSocket(info) => {
                hibernatable_web_socket_event_info(lock, keys, info?)?
            }
            Which::Connect(_) => typed(lock, keys, "connect")?,
            Which::Custom(_) => typed(lock, keys, "custom")?,
        },
    )
}

fn hibernatable_web_socket_event_info<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    info: trace::hibernatable_web_socket_event_info::Reader<'_>,
) -> Result<Value<'s>> {
    use trace::hibernatable_web_socket_event_info::type_::Which;
    let obj = lock.new_object();
    let value = name(lock, "hibernatableWebSocket")?;
    keys.set(lock, obj, Key::Type, value)?;
    let value = match info
        .get_type()
        .which()
        .map_err(|err| unknown("hibernatable WebSocket event type", err))?
    {
        Which::Message(()) => typed(lock, keys, "message")?,
        Which::Error(()) => typed(lock, keys, "error")?,
        Which::Close(close) => {
            let info = lock.new_object();
            let value = name(lock, "close")?;
            keys.set(lock, info, Key::Type, value)?;
            let value = num(lock, f64::from(close.get_code()));
            keys.set(lock, info, Key::Code, value)?;
            let value = v8::Boolean::new(lock, close.get_was_clean()).into();
            keys.set(lock, info, Key::WasClean, value)?;
            info.into()
        }
    };
    keys.set(lock, obj, Key::Info, value)?;
    Ok(obj.into())
}

fn onset<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    onset: trace::onset::Reader<'_>,
    options: Options,
) -> Result<Value<'s>> {
    use trace::ExecutionModel;
    let obj = lock.new_object();
    let value = name(lock, "onset")?;
    keys.set(lock, obj, Key::Type, value)?;
    let execution_model = enumerant(onset.get_execution_model(), |model| match model {
        ExecutionModel::Stateless => "stateless",
        ExecutionModel::DurableObject => "durableObject",
        ExecutionModel::Workflow => "workflow",
    });
    let value = name(lock, &execution_model)?;
    keys.set(lock, obj, Key::ExecutionModel, value)?;
    let value = str(lock, ids::span_id_go(onset.get_span_id()).as_bytes())?;
    keys.set(lock, obj, Key::SpanId, value)?;

    if onset.has_dispatch_namespace() {
        let value = text(lock, onset.get_dispatch_namespace())?;
        keys.set(lock, obj, Key::DispatchNamespace, value)?;
    }
    if onset.has_entry_point() {
        let value = text(lock, onset.get_entry_point())?;
        keys.set(lock, obj, Key::Entrypoint, value)?;
    }
    if onset.has_durable_object_id() {
        let value = text(lock, onset.get_durable_object_id())?;
        keys.set(lock, obj, Key::DurableObjectId, value)?;
    }
    if onset.has_script_name() {
        let value = text(lock, onset.get_script_name())?;
        keys.set(lock, obj, Key::ScriptName, value)?;
    }
    if onset.has_script_tags() {
        let tags = collect(onset.get_script_tags()?.iter(), |tag| text(lock, tag))?;
        let value = array(lock, &tags);
        keys.set(lock, obj, Key::ScriptTags, value)?;
    }
    if onset.has_script_version() {
        let version = onset.get_script_version()?;
        let vobj = lock.new_object();
        let id = version.get_id();
        if let Some(uuid) = ids::uuid(id.get_upper(), id.get_lower()) {
            let value = str(lock, uuid.as_bytes())?;
            keys.set(lock, vobj, Key::Id, value)?;
        }
        if version.has_tag() {
            let value = text(lock, version.get_tag())?;
            keys.set(lock, vobj, Key::Tag, value)?;
        }
        if version.has_message() {
            let value = text(lock, version.get_message())?;
            keys.set(lock, vobj, Key::Message, value)?;
        }
        keys.set(lock, obj, Key::ScriptVersion, vobj.into())?;
    }
    if onset.has_preview() {
        let preview = onset.get_preview()?;
        let pobj = lock.new_object();
        let value = text(lock, preview.get_id())?;
        keys.set(lock, pobj, Key::Id, value)?;
        let value = text(lock, preview.get_slug())?;
        keys.set(lock, pobj, Key::Slug, value)?;
        let value = text(lock, preview.get_name())?;
        keys.set(lock, pobj, Key::Name, value)?;
        keys.set(lock, obj, Key::Preview, pobj.into())?;
    }

    let value = onset_info(lock, keys, onset.get_info()?, options)?;
    keys.set(lock, obj, Key::Info, value)?;

    let attributes = onset.get_attributes()?;
    if !attributes.is_empty() {
        let value = attribute_list(lock, keys, attributes)?;
        keys.set(lock, obj, Key::Attributes, value)?;
    }
    Ok(obj.into())
}

fn event_outcome<'s>(
    lock: &mut Lock<'s>,
    outcome: std::result::Result<outcome_capnp::EventOutcome, capnp::NotInSchema>,
) -> Result<Value<'s>> {
    use outcome_capnp::EventOutcome;
    let outcome = match outcome.map_err(|_| ConvertError::Unreachable)? {
        EventOutcome::Unknown => "unknown",
        EventOutcome::Ok => "ok",
        EventOutcome::Exception => "exception",
        EventOutcome::ExceededCpu => "exceededCpu",
        EventOutcome::KillSwitch => "killSwitch",
        EventOutcome::DaemonDown => "daemonDown",
        EventOutcome::ScriptNotFound => "scriptNotFound",
        EventOutcome::Canceled => "canceled",
        EventOutcome::ExceededMemory => "exceededMemory",
        EventOutcome::LoadShed => "loadShed",
        EventOutcome::ResponseStreamDisconnected => "responseStreamDisconnected",
        EventOutcome::InternalError => "internalError",
        EventOutcome::ExceededWallTime => "exceededWallTime",
        EventOutcome::Aborted => "aborted",
    };
    name(lock, outcome)
}

/// A duration of `millis` milliseconds in whole milliseconds, as `Outcome` stores it in a
/// `kj::Duration` and the C++ conversion divides it.
fn duration_millis(millis: u64) -> f64 {
    #[expect(
        clippy::cast_possible_wrap,
        clippy::cast_precision_loss,
        reason = "kj::Duration is a signed count of nanoseconds, and a JavaScript number a double"
    )]
    let millis = (millis.wrapping_mul(1_000_000) as i64 / NANOS_PER_MILLI) as f64;
    millis
}

fn outcome<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    outcome: trace::outcome::Reader<'_>,
) -> Result<Value<'s>> {
    let obj = lock.new_object();
    let value = name(lock, "outcome")?;
    keys.set(lock, obj, Key::Type, value)?;
    let value = event_outcome(lock, outcome.get_outcome())?;
    keys.set(lock, obj, Key::Outcome, value)?;
    let value = num(lock, duration_millis(outcome.get_cpu_time()));
    keys.set(lock, obj, Key::CpuTime, value)?;
    let value = num(lock, duration_millis(outcome.get_wall_time()));
    keys.set(lock, obj, Key::WallTime, value)?;
    Ok(obj.into())
}

fn span_open<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    span_open: trace::span_open::Reader<'_>,
) -> Result<Value<'s>> {
    use trace::span_open::info::Which;
    let obj = lock.new_object();
    let value = name(lock, "spanOpen")?;
    keys.set(lock, obj, Key::Type, value)?;
    let value = text(lock, span_open.get_operation_name())?;
    keys.set(lock, obj, Key::Name, value)?;
    let value = str(lock, ids::span_id_go(span_open.get_span_id()).as_bytes())?;
    keys.set(lock, obj, Key::SpanId, value)?;
    let info = match span_open
        .get_info()
        .which()
        .map_err(|err| unknown("span open info", err))?
    {
        Which::Empty(()) => None,
        Which::Fetch(fetch) => Some(fetch_event_info(lock, keys, fetch?)?),
        Which::JsRpc(_) => Some(typed(lock, keys, "jsrpc")?),
        Which::Custom(list) => Some(attributes_event(lock, keys, list?)?),
    };
    if let Some(value) = info {
        keys.set(lock, obj, Key::Info, value)?;
    }
    Ok(obj.into())
}

fn span_close<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    span_close: trace::span_close::Reader<'_>,
) -> Result<Value<'s>> {
    let obj = lock.new_object();
    let value = name(lock, "spanClose")?;
    keys.set(lock, obj, Key::Type, value)?;
    let value = event_outcome(lock, span_close.get_outcome())?;
    keys.set(lock, obj, Key::Outcome, value)?;
    Ok(obj.into())
}

fn span_status<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    status: trace_capnp::span_status::Reader<'_>,
) -> Result<Value<'s>> {
    use trace_capnp::SpanStatusCode;
    let obj = lock.new_object();
    // The C++ decoder reads a code the schema does not know as UNSET, and keeps the message of
    // an ERROR only.
    let (code, keeps_message) = match status.get_code() {
        Ok(SpanStatusCode::Ok) => ("ok", false),
        Ok(SpanStatusCode::Error) => ("error", true),
        Ok(SpanStatusCode::Unset) | Err(_) => ("unset", false),
    };
    let value = name(lock, code)?;
    keys.set(lock, obj, Key::Code, value)?;
    if keeps_message
        && let Ok(trace_capnp::span_status::message::Text(message)) = status.get_message().which()
    {
        let value = text(lock, message)?;
        keys.set(lock, obj, Key::Message, value)?;
    }
    Ok(obj.into())
}

fn span_update<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    span_update: trace::span_update::Reader<'_>,
) -> Result<Value<'s>> {
    use trace::span_update::info::Which;
    let obj = lock.new_object();
    let value = name(lock, "spanUpdate")?;
    keys.set(lock, obj, Key::Type, value)?;
    let info = lock.new_object();
    match span_update
        .get_info()
        .which()
        .map_err(|err| unknown("span update info", err))?
    {
        Which::Name(operation_name) => {
            let value = name(lock, "name")?;
            keys.set(lock, info, Key::Type, value)?;
            let value = text(lock, operation_name)?;
            keys.set(lock, info, Key::Name, value)?;
        }
        Which::Status(status) => {
            let value = name(lock, "status")?;
            keys.set(lock, info, Key::Type, value)?;
            let value = span_status(lock, keys, status?)?;
            keys.set(lock, info, Key::Status, value)?;
        }
    }
    keys.set(lock, obj, Key::Info, info.into())?;
    Ok(obj.into())
}

fn span_links<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    span_links: trace::span_links::Reader<'_>,
) -> Result<Value<'s>> {
    let obj = lock.new_object();
    let value = name(lock, "spanLinks")?;
    keys.set(lock, obj, Key::Type, value)?;
    let links = collect(span_links.get_links()?.iter(), |link| {
        let value = lock.new_object();
        let context = lock.new_object();
        let link_context = link.get_context()?;
        let trace_id = link_context.get_trace_id()?;
        let id = str(
            lock,
            ids::trace_id_w3c(trace_id.get_high(), trace_id.get_low()).as_bytes(),
        )?;
        keys.set(lock, context, Key::TraceId, id)?;
        if let Ok(worker_interface_capnp::span_context::info::SpanId(span_id)) =
            link_context.get_info().which()
        {
            let id = str(lock, ids::span_id_go(span_id).as_bytes())?;
            keys.set(lock, context, Key::SpanId, id)?;
        }
        if let Some(flags) = trace_flags(
            link_context.has_trace_flags(),
            link_context.get_trace_flags(),
        )? {
            let flags = num(lock, f64::from(flags));
            keys.set(lock, context, Key::TraceFlags, flags)?;
        }
        keys.set(lock, value, Key::Context, context.into())?;
        let attributes = attribute_list(lock, keys, link.get_attributes()?)?;
        keys.set(lock, value, Key::Attributes, attributes)?;
        let count = num(lock, f64::from(link.get_dropped_attributes_count()));
        keys.set(lock, value, Key::DroppedAttributesCount, count)?;
        Ok(value.into())
    })?;
    let value = array(lock, &links);
    keys.set(lock, obj, Key::Links, value)?;
    Ok(obj.into())
}

fn diagnostic_channel_event<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    event: trace::diagnostic_channel_event::Reader<'_>,
) -> Result<Value<'s>> {
    let obj = lock.new_object();
    let value = name(lock, "diagnosticChannel")?;
    keys.set(lock, obj, Key::Type, value)?;
    let channel = event.get_channel()?;
    let value = v8::String::new_internalized_from_utf8(lock, channel.as_bytes())
        .ok_or_else(jsg::Error::invalid_string_length)?
        .into();
    keys.set(lock, obj, Key::Channel, value)?;
    let value = match jsg::ser::deserialize(lock, event.get_message()?) {
        Ok(value) => value,
        // Only a missing or unsupported header fails without an exception pending.
        Err(err) if !err.is_pending() => return Err(ConvertError::DiagnosticChannelHeader),
        Err(err) => return Err(err.into()),
    };
    keys.set(lock, obj, Key::Message, value)?;
    Ok(obj.into())
}

fn exception<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    exception: trace::exception::Reader<'_>,
) -> Result<Value<'s>> {
    use trace::exception::code::Which;
    let obj = lock.new_object();
    let value = name(lock, "exception")?;
    keys.set(lock, obj, Key::Type, value)?;
    match exception.get_code().which() {
        Ok(Which::Text(code)) => {
            let value = text(lock, code)?;
            keys.set(lock, obj, Key::Code, value)?;
        }
        Ok(Which::Number(code)) => {
            let value = num(lock, code);
            keys.set(lock, obj, Key::Code, value)?;
        }
        // The C++ decoder reads a code the schema does not know as none.
        Ok(Which::None(())) | Err(_) => {}
    }
    let value = text(lock, exception.get_name())?;
    keys.set(lock, obj, Key::Name, value)?;
    let value = text(lock, exception.get_message())?;
    keys.set(lock, obj, Key::Message, value)?;
    if exception.has_stack() {
        let value = text(lock, exception.get_stack())?;
        keys.set(lock, obj, Key::Stack, value)?;
    }
    Ok(obj.into())
}

fn log<'s>(lock: &mut Lock<'s>, keys: &mut Keys, log: trace::log::Reader<'_>) -> Result<Value<'s>> {
    use trace::log::Level;
    let obj = lock.new_object();
    let value = name(lock, "log")?;
    keys.set(lock, obj, Key::Type, value)?;
    let level = match log.get_log_level().map_err(|_| ConvertError::Unreachable)? {
        Level::Debug => "debug",
        Level::Info => "info",
        Level::Log => "log",
        Level::Warn => "warn",
        Level::Error => "error",
    };
    let value = name(lock, level)?;
    keys.set(lock, obj, Key::Level, value)?;
    let message = log.get_message()?.as_bytes();
    if log.get_truncated() {
        // The receiver gets truncated JSON as the raw string. Absence means not truncated.
        let value = str(lock, message)?;
        keys.set(lock, obj, Key::Message, value)?;
        let value = v8::Boolean::new(lock, true).into();
        keys.set(lock, obj, Key::Truncated, value)?;
    } else {
        let value = parse_json(lock, message)?;
        keys.set(lock, obj, Key::Message, value)?;
    }
    if log.has_error_info() {
        // A positional array: each slot is either { name, message, stack? } for an argument
        // that was a native Error, or null for any other argument.
        let slots = collect(log.get_error_info()?.iter(), |slot| match slot.which() {
            Ok(trace::error_info_slot::Info(info)) => {
                let info = info?;
                let err = lock.new_object();
                let value = text(lock, info.get_name())?;
                keys.set(lock, err, Key::Name, value)?;
                let value = text(lock, info.get_message())?;
                keys.set(lock, err, Key::Message, value)?;
                if info.has_stack() {
                    let value = text(lock, info.get_stack())?;
                    keys.set(lock, err, Key::Stack, value)?;
                }
                Ok(err.into())
            }
            // The C++ decoder reads a slot the schema does not know as none.
            Ok(trace::error_info_slot::None(())) | Err(_) => Ok(lock.null()),
        })?;
        let value = array(lock, &slots);
        keys.set(lock, obj, Key::ErrorInfo, value)?;
    }
    Ok(obj.into())
}

fn stream_diagnostics<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    diagnostics: trace::stream_diagnostics_event::Reader<'_>,
) -> Result<Value<'s>> {
    use trace::stream_diagnostics_event::diagnostic::Which;
    let Ok(Which::DroppedEvents(dropped)) = diagnostics.get_diagnostic().which() else {
        return Err(DecodeError::new("received invalid diagnostics event").into());
    };
    let count = dropped?.get_count();
    let obj = lock.new_object();
    let value = name(lock, "streamDiagnostic")?;
    keys.set(lock, obj, Key::Type, value)?;
    let diagnostic = lock.new_object();
    let value = name(lock, "droppedEvents")?;
    keys.set(lock, diagnostic, Key::DiagnosticsType, value)?;
    let value = num(lock, f64::from(count));
    keys.set(lock, diagnostic, Key::Count, value)?;
    keys.set(lock, obj, Key::Diagnostic, diagnostic.into())?;
    Ok(obj.into())
}

fn ret<'s>(
    lock: &mut Lock<'s>,
    keys: &mut Keys,
    ret: trace::return_::Reader<'_>,
) -> Result<Value<'s>> {
    use trace::return_::info::Which;
    let obj = lock.new_object();
    let value = name(lock, "return")?;
    keys.set(lock, obj, Key::Type, value)?;
    match ret
        .get_info()
        .which()
        .map_err(|err| unknown("return info", err))?
    {
        Which::Empty(()) => {}
        Which::Fetch(fetch) => {
            let info = lock.new_object();
            let value = name(lock, "fetch")?;
            keys.set(lock, info, Key::Type, value)?;
            let value = num(lock, f64::from(fetch?.get_status_code()));
            keys.set(lock, info, Key::StatusCode, value)?;
            keys.set(lock, obj, Key::Info, info.into())?;
        }
    }
    Ok(obj.into())
}

#[cfg(test)]
#[path = "to_js-test.rs"]
mod tests;
