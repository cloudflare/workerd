// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! What invoked a worker: the event infos of `workerd/io/trace.h` (`tracing::FetchEventInfo` and
//! the rest of `tracing::EventInfo`), the preview a buffered trace carries and a fetch's
//! response. Strings are bytes, since a `kj::String` need not be UTF-8.

pub use http_over_capnp_capnp::HttpMethod;
use worker_interface_capnp::trace;
pub use worker_interface_capnp::trace::on_hibernate_event_info::Reason as OnHibernateReason;
use worker_interface_capnp::trace::onset;

use crate::attribute::list_len;

fn text(reader: capnp::Result<capnp::text::Reader<'_>>) -> capnp::Result<Vec<u8>> {
    Ok(reader?.as_bytes().to_vec())
}

fn as_text(bytes: &[u8]) -> capnp::text::Reader<'_> {
    capnp::text::Reader::from(bytes)
}

/// One header of a fetch request.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FetchHeader {
    pub name: Vec<u8>,
    pub value: Vec<u8>,
}

/// A fetch request, `tracing::FetchEventInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FetchEventInfo {
    /// One of the methods C++ knows (`kj::HttpMethod`). It must not be `Other`, which C++ rejects.
    pub method: HttpMethod,
    pub url: Vec<u8>,
    /// The `cf` object as JSON; empty if the request had none.
    pub cf_json: Vec<u8>,
    pub headers: Vec<FetchHeader>,
}

impl FetchEventInfo {
    /// Reads the info. A method C++ doesn't know, `Other` included, is an error, as C++ requires.
    pub fn from_capnp(reader: trace::fetch_event_info::Reader<'_>) -> capnp::Result<Self> {
        let method = reader.get_method()?;
        if method == HttpMethod::Other {
            return Err(capnp::Error::failed("unknown method".into()));
        }
        Ok(Self {
            method,
            url: text(reader.get_url())?,
            cf_json: text(reader.get_cf_json())?,
            headers: reader
                .get_headers()?
                .iter()
                .map(|header| {
                    Ok(FetchHeader {
                        name: text(header.get_name())?,
                        value: text(header.get_value())?,
                    })
                })
                .collect::<capnp::Result<_>>()?,
        })
    }

    pub fn to_capnp(&self, mut builder: trace::fetch_event_info::Builder<'_>) {
        debug_assert_ne!(
            self.method,
            HttpMethod::Other,
            "C++ rejects the `other` method"
        );
        builder.set_method(self.method);
        builder.set_url(as_text(&self.url));
        builder.set_cf_json(as_text(&self.cf_json));
        let mut list = builder.init_headers(list_len(self.headers.len()));
        for (i, header) in (0..).zip(&self.headers) {
            let mut element = list.reborrow().get(i);
            element.set_name(as_text(&header.name));
            element.set_value(as_text(&header.value));
        }
    }
}

/// A JS RPC call, `tracing::JsRpcEventInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct JsRpcEventInfo {
    pub method_name: Vec<u8>,
}

impl JsRpcEventInfo {
    pub fn from_capnp(reader: trace::js_rpc_event_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            method_name: text(reader.get_method_name())?,
        })
    }

    pub fn to_capnp(&self, mut builder: trace::js_rpc_event_info::Builder<'_>) {
        builder.set_method_name(as_text(&self.method_name));
    }
}

/// A cron trigger, `tracing::ScheduledEventInfo`.
#[derive(Clone, Debug, PartialEq)]
pub struct ScheduledEventInfo {
    /// Milliseconds since the Unix epoch.
    pub scheduled_time: f64,
    pub cron: Vec<u8>,
}

impl ScheduledEventInfo {
    pub fn from_capnp(reader: trace::scheduled_event_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            scheduled_time: reader.get_scheduled_time(),
            cron: text(reader.get_cron())?,
        })
    }

    pub fn to_capnp(&self, mut builder: trace::scheduled_event_info::Builder<'_>) {
        builder.set_scheduled_time(self.scheduled_time);
        builder.set_cron(as_text(&self.cron));
    }
}

/// A Durable Object alarm, `tracing::AlarmEventInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AlarmEventInfo {
    /// Milliseconds since the Unix epoch. C++ holds it as a `kj::Date` in nanoseconds, so it
    /// cannot hold more than about 292 years either side of 1970.
    pub scheduled_time_ms: i64,
}

impl AlarmEventInfo {
    pub fn from_capnp(reader: trace::alarm_event_info::Reader<'_>) -> Self {
        Self {
            scheduled_time_ms: reader.get_scheduled_time_ms(),
        }
    }

    pub fn to_capnp(&self, mut builder: trace::alarm_event_info::Builder<'_>) {
        builder.set_scheduled_time_ms(self.scheduled_time_ms);
    }
}

/// A Durable Object's `onHibernate` handler, `tracing::OnHibernateEventInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OnHibernateEventInfo {
    pub reason: OnHibernateReason,
}

impl OnHibernateEventInfo {
    /// Reads the info. An unknown reason is an error: C++ keeps its number, which the Rust enum
    /// cannot hold.
    pub fn from_capnp(reader: trace::on_hibernate_event_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            reason: reader.get_reason()?,
        })
    }

    pub fn to_capnp(&self, mut builder: trace::on_hibernate_event_info::Builder<'_>) {
        builder.set_reason(self.reason);
    }
}

/// A queue batch, `tracing::QueueEventInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QueueEventInfo {
    pub queue_name: Vec<u8>,
    pub batch_size: u32,
}

impl QueueEventInfo {
    pub fn from_capnp(reader: trace::queue_event_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            queue_name: text(reader.get_queue_name())?,
            batch_size: reader.get_batch_size(),
        })
    }

    pub fn to_capnp(&self, mut builder: trace::queue_event_info::Builder<'_>) {
        builder.set_queue_name(as_text(&self.queue_name));
        builder.set_batch_size(self.batch_size);
    }
}

/// An incoming email, `tracing::EmailEventInfo`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EmailEventInfo {
    pub mail_from: Vec<u8>,
    pub rcpt_to: Vec<u8>,
    pub raw_size: u32,
}

impl EmailEventInfo {
    pub fn from_capnp(reader: trace::email_event_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            mail_from: text(reader.get_mail_from())?,
            rcpt_to: text(reader.get_rcpt_to())?,
            raw_size: reader.get_raw_size(),
        })
    }

    pub fn to_capnp(&self, mut builder: trace::email_event_info::Builder<'_>) {
        builder.set_mail_from(as_text(&self.mail_from));
        builder.set_rcpt_to(as_text(&self.rcpt_to));
        builder.set_raw_size(self.raw_size);
    }
}

/// One of the traces a buffered tail worker is handed, `tracing::TraceEventInfo::TraceItem`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TraceItem {
    /// `None` only for a trace without a script name; read from capnp, it is always present,
    /// and empty if the message had none, as in C++.
    pub script_name: Option<Vec<u8>>,
}

/// A buffered tail worker's invocation, `tracing::TraceEventInfo`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TraceEventInfo {
    pub traces: Vec<TraceItem>,
}

impl TraceEventInfo {
    pub fn from_capnp(reader: trace::trace_event_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            traces: reader
                .get_traces()?
                .iter()
                .map(|item| {
                    Ok(TraceItem {
                        script_name: Some(text(item.get_script_name())?),
                    })
                })
                .collect::<capnp::Result<_>>()?,
        })
    }

    pub fn to_capnp(&self, builder: trace::trace_event_info::Builder<'_>) {
        let mut list = builder.init_traces(list_len(self.traces.len()));
        for (i, item) in (0..).zip(&self.traces) {
            if let Some(name) = &item.script_name {
                list.reborrow().get(i).set_script_name(as_text(name));
            }
        }
    }
}

/// What a hibernatable WebSocket delivered, `tracing::HibernatableWebSocketEventInfo::Type`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HibernatableWebSocketEventInfo {
    Message,
    Close { code: u16, was_clean: bool },
    Error,
}

impl HibernatableWebSocketEventInfo {
    /// Reads the info. An unknown type is an error, as C++ treats it as unreachable.
    pub fn from_capnp(
        reader: trace::hibernatable_web_socket_event_info::Reader<'_>,
    ) -> capnp::Result<Self> {
        use trace::hibernatable_web_socket_event_info::type_;
        Ok(match reader.get_type().which()? {
            type_::Message(()) => Self::Message,
            type_::Close(close) => Self::Close {
                code: close.get_code(),
                was_clean: close.get_was_clean(),
            },
            type_::Error(()) => Self::Error,
        })
    }

    pub fn to_capnp(self, builder: trace::hibernatable_web_socket_event_info::Builder<'_>) {
        let mut type_ = builder.init_type();
        match self {
            Self::Message => type_.set_message(()),
            Self::Close { code, was_clean } => {
                let mut close = type_.init_close();
                close.set_code(code);
                close.set_was_clean(was_clean);
            }
            Self::Error => type_.set_error(()),
        }
    }
}

/// What invoked a worker, `tracing::EventInfo`: the info of an onset event.
///
/// Connect and custom events carry no details.
#[derive(Clone, Debug, PartialEq)]
pub enum EventInfo {
    Fetch(FetchEventInfo),
    JsRpc(JsRpcEventInfo),
    Scheduled(ScheduledEventInfo),
    Alarm(AlarmEventInfo),
    Queue(QueueEventInfo),
    Email(EmailEventInfo),
    Trace(TraceEventInfo),
    HibernatableWebSocket(HibernatableWebSocketEventInfo),
    Connect,
    OnHibernate(OnHibernateEventInfo),
    Custom,
}

impl EventInfo {
    /// Reads an onset's info. An unknown variant is an error, as C++ treats it as unreachable.
    pub fn from_capnp(reader: onset::info::Reader<'_>) -> capnp::Result<Self> {
        use onset::info::Which;
        Ok(match reader.which()? {
            Which::Fetch(info) => Self::Fetch(FetchEventInfo::from_capnp(info?)?),
            Which::JsRpc(info) => Self::JsRpc(JsRpcEventInfo::from_capnp(info?)?),
            Which::Scheduled(info) => Self::Scheduled(ScheduledEventInfo::from_capnp(info?)?),
            Which::Alarm(info) => Self::Alarm(AlarmEventInfo::from_capnp(info?)),
            Which::Queue(info) => Self::Queue(QueueEventInfo::from_capnp(info?)?),
            Which::Email(info) => Self::Email(EmailEventInfo::from_capnp(info?)?),
            Which::Trace(info) => Self::Trace(TraceEventInfo::from_capnp(info?)?),
            Which::HibernatableWebSocket(info) => {
                Self::HibernatableWebSocket(HibernatableWebSocketEventInfo::from_capnp(info?)?)
            }
            Which::Connect(info) => {
                info?;
                Self::Connect
            }
            Which::OnHibernate(info) => Self::OnHibernate(OnHibernateEventInfo::from_capnp(info?)?),
            // C++ doesn't read a custom event's info, so neither does this, not even to check it.
            Which::Custom(_) => Self::Custom,
        })
    }

    pub fn to_capnp(&self, builder: onset::info::Builder<'_>) {
        match self {
            Self::Fetch(info) => info.to_capnp(builder.init_fetch()),
            Self::JsRpc(info) => info.to_capnp(builder.init_js_rpc()),
            Self::Scheduled(info) => info.to_capnp(builder.init_scheduled()),
            Self::Alarm(info) => info.to_capnp(builder.init_alarm()),
            Self::Queue(info) => info.to_capnp(builder.init_queue()),
            Self::Email(info) => info.to_capnp(builder.init_email()),
            Self::Trace(info) => info.to_capnp(builder.init_trace()),
            Self::HibernatableWebSocket(info) => {
                info.to_capnp(builder.init_hibernatable_web_socket());
            }
            Self::Connect => {
                builder.init_connect();
            }
            Self::OnHibernate(info) => info.to_capnp(builder.init_on_hibernate()),
            Self::Custom => {
                builder.init_custom();
            }
        }
    }
}

/// The preview a trace was made in, `tracing::TracePreview`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TracePreview {
    pub id: Vec<u8>,
    pub slug: Vec<u8>,
    pub name: Vec<u8>,
}

impl TracePreview {
    pub fn from_capnp(reader: trace::trace_preview_info::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            id: text(reader.get_id())?,
            slug: text(reader.get_slug())?,
            name: text(reader.get_name())?,
        })
    }

    pub fn to_capnp(&self, mut builder: trace::trace_preview_info::Builder<'_>) {
        builder.set_id(as_text(&self.id));
        builder.set_slug(as_text(&self.slug));
        builder.set_name(as_text(&self.name));
    }
}

/// The response of a fetch, `tracing::FetchResponseInfo`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FetchResponseInfo {
    pub status_code: u16,
}

impl FetchResponseInfo {
    pub fn from_capnp(reader: trace::fetch_response_info::Reader<'_>) -> Self {
        Self {
            status_code: reader.get_status_code(),
        }
    }

    pub fn to_capnp(self, mut builder: trace::fetch_response_info::Builder<'_>) {
        builder.set_status_code(self.status_code);
    }
}

#[cfg(test)]
#[path = "event-test.rs"]
mod tests;
