// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The names of the properties of the JavaScript values of tail events, made once per batch.
//!
//! Setting a property by a string makes an internalized V8 string of it each time, which looks
//! the name up in V8's string table. [`Keys`] makes each name once, on its first use, and reuses
//! it for every later event of the batch. It holds the names as `v8::Global`s, which are released
//! when it is dropped, so nothing outlives the batch.

use jsg::Lock;
use jsg::v8;

macro_rules! keys {
    ($($key:ident = $name:literal,)*) => {
        /// The name of a property the conversion sets.
        #[derive(Clone, Copy, Debug)]
        pub enum Key {
            $($key,)*
        }

        impl Key {
            const COUNT: usize = [$($name,)*].len();

            const fn name(self) -> &'static str {
                match self {
                    $(Self::$key => $name,)*
                }
            }
        }
    };
}

keys! {
    Attributes = "attributes",
    BatchSize = "batchSize",
    CfJson = "cfJson",
    Channel = "channel",
    Code = "code",
    Context = "context",
    Count = "count",
    CpuTime = "cpuTime",
    Cron = "cron",
    Diagnostic = "diagnostic",
    DiagnosticsType = "diagnosticsType",
    DispatchNamespace = "dispatchNamespace",
    DroppedAttributesCount = "droppedAttributesCount",
    DurableObjectId = "durableObjectId",
    Entrypoint = "entrypoint",
    ErrorInfo = "errorInfo",
    Event = "event",
    ExecutionModel = "executionModel",
    Headers = "headers",
    Id = "id",
    Info = "info",
    InvocationId = "invocationId",
    Level = "level",
    Links = "links",
    MailFrom = "mailFrom",
    Message = "message",
    Method = "method",
    Name = "name",
    Outcome = "outcome",
    Preview = "preview",
    QueueName = "queueName",
    RawSize = "rawSize",
    RcptTo = "rcptTo",
    Reason = "reason",
    ScheduledTime = "scheduledTime",
    ScriptName = "scriptName",
    ScriptTags = "scriptTags",
    ScriptVersion = "scriptVersion",
    Sequence = "sequence",
    Slug = "slug",
    SpanContext = "spanContext",
    SpanId = "spanId",
    Stack = "stack",
    Status = "status",
    StatusCode = "statusCode",
    Tag = "tag",
    Timestamp = "timestamp",
    TraceFlags = "traceFlags",
    TraceId = "traceId",
    Traces = "traces",
    Truncated = "truncated",
    Type = "type",
    Url = "url",
    Value = "value",
    WallTime = "wallTime",
    WasClean = "wasClean",
}

/// The property names made so far for a batch.
pub struct Keys {
    names: [Option<v8::Global<v8::Name>>; Key::COUNT],
}

impl Default for Keys {
    fn default() -> Self {
        Self {
            names: [const { None }; Key::COUNT],
        }
    }
}

impl Keys {
    /// Sets the property `key` of `object` to `value`, as `object[key] = value` does.
    ///
    /// # Errors
    ///
    /// As for [`v8::Local::<v8::Object>::set`].
    pub fn set<'s>(
        &mut self,
        lock: &mut Lock<'s>,
        object: v8::Local<'s, v8::Object>,
        key: Key,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        let slot = &mut self.names[key as usize];
        let name = if let Some(name) = slot {
            name
        } else {
            let name = v8::String::new_internalized_from_str(lock, key.name())
                .ok_or_else(jsg::Error::invalid_string_length)?;
            slot.insert(v8::Global::new(lock, name.into()))
        };
        object.set_by_name(lock, name, value)
    }
}
