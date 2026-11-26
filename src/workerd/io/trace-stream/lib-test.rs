// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use capnp::message::HeapAllocator;
use jsg_test::EvalContext;
use jsg_test::Harness;
use trace::tail_event;

use super::*;

/// Prints a JavaScript value as `trace-stream-js-test.c++` does, so that the expectations below
/// read like its golden file.
const DUMP: &str = r#"
  globalThis.dump = function dump(value) {
    if (value === null) return "null";
    switch (typeof value) {
      case "undefined": return "undefined";
      case "string": return JSON.stringify(value);
      case "number": return Object.is(value, -0) ? "-0" : String(value);
      case "bigint": return `${value}n`;
      case "boolean": return String(value);
    }
    if (value instanceof Date) return `Date(${value.getTime()})`;
    if (Array.isArray(value)) return `[${value.map(dump).join(", ")}]`;
    if (value instanceof Map) {
      return `Map{${[...value].map(([k, v]) => `${dump(k)} => ${dump(v)}`).join(", ")}}`;
    }
    if (value instanceof Set) return `Set{${[...value].map(dump).join(", ")}}`;
    if (value instanceof RegExp) return `RegExp(${value})`;
    const proto = Object.getPrototypeOf(value);
    const tag = proto === Object.prototype ? "" : proto === null ? "<null> " : `${proto.constructor.name} `;
    const keys = Reflect.ownKeys(value).map((key) => {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      const flags = (descriptor.writable === false ? "!w" : "") +
          (descriptor.enumerable ? "" : "!e") + (descriptor.configurable ? "" : "!c");
      return `${String(key)}${flags ? `(${flags})` : ""}: ${dump(value[key])}`;
    });
    return `${tag}{${keys.join(", ")}}`;
  };
"#;

/// The `spanContext`, `invocationId` and `timestamp` that [`set_common`] sets.
const COMMON: &str = r#"spanContext: {traceId: "0123456789abcdeffedcba9876543210", spanId: "00000000000000ab", traceFlags: 1}, invocationId: "11111111111111112222222222222222", timestamp: Date(1700000000123)"#;

/// The attributes that [`set_attributes`] sets.
const ATTRIBUTES: &str = r#"[{name: "str", value: "text"}, {name: "bool", value: true}, {name: "int", value: -9007199254740993n}, {name: "float", value: -0}, {name: "many", value: ["a", 2n]}, {name: "none", value: []}, {name: "strs", value: ["x", null]}, {name: "bools", value: [false, null]}, {name: "floats", value: [1.5, null]}]"#;

/// The fetch event info that [`set_fetch_info`] sets.
const FETCH_INFO: &str = r#"{type: "fetch", method: "POST", url: "https://example.com/path?q=1", cfJson: {colo: "LHR", asn: 13335}, headers: [{name: "content-type", value: "text/plain"}, {name: "x-custom", value: "vé"}]}"#;

/// Builds the info of an onset event.
type BuildInfo = fn(trace::onset::info::Builder<'_>);

/// Builds an event.
type BuildEvent = fn(tail_event::event::Builder<'_>);

/// A message holding `TailStreamParams` with `count` events, each built by `build`.
fn params(count: u32, build: impl Fn(u32, tail_event::Builder<'_>)) -> Builder<HeapAllocator> {
    let mut message = Builder::new_default();
    let params = message.init_root::<tail_stream_params::Builder<'_>>();
    let mut events = params.init_events(count);
    for index in 0..count {
        build(index, events.reborrow().get(index));
    }
    message
}

fn read(message: &Builder<HeapAllocator>) -> tail_stream_params::Reader<'_> {
    message
        .get_root_as_reader::<tail_stream_params::Reader<'_>>()
        .unwrap()
}

/// Sets the field `name` of the struct `builder` to the enumerant `value`, which the schema need
/// not know.
fn set_raw_enum<'a>(builder: impl Into<capnp::dynamic_value::Builder<'a>>, name: &str, value: u16) {
    let mut builder = builder
        .into()
        .downcast::<capnp::dynamic_struct::Builder<'a>>();
    let field = builder.get_schema().get_field_by_name(name).unwrap();
    let capnp::introspect::TypeVariant::Enum(schema) = field.get_type().which() else {
        panic!("{name} is not an enum");
    };
    builder
        .set(
            field,
            capnp::dynamic_value::Enum::new(value, capnp::schema::EnumSchema::new(schema)).into(),
        )
        .unwrap();
}

fn set_common(mut event: tail_event::Builder<'_>, sequence: u32) {
    let mut context = event.reborrow().init_span_context();
    let mut trace_id = context.reborrow().init_trace_id();
    trace_id.set_high(0x0123_4567_89ab_cdef);
    trace_id.set_low(0xfedc_ba98_7654_3210);
    context.reborrow().get_info().set_span_id(0xab);
    context.init_trace_flags().get_value().set_set(1);
    let mut invocation_id = event.reborrow().init_invocation_id();
    invocation_id.set_high(0x1111_1111_1111_1111);
    invocation_id.set_low(0x2222_2222_2222_2222);
    event.set_timestamp_ns(1_700_000_000_123_456_789);
    event.set_sequence(sequence);
}

fn set_attributes(mut list: capnp::struct_list::Builder<'_, trace::attribute::Owned>) {
    fn one<'a>(
        mut attribute: trace::attribute::Builder<'a>,
        name: &str,
    ) -> trace_capnp::tag_value::Builder<'a> {
        attribute.set_name(name);
        attribute.init_value(1).get(0)
    }
    one(list.reborrow().get(0), "str").set_string("text");
    one(list.reborrow().get(1), "bool").set_bool(true);
    one(list.reborrow().get(2), "int").set_int64(-9_007_199_254_740_993);
    one(list.reborrow().get(3), "float").set_float64(-0.0);
    let mut many = list.reborrow().get(4);
    many.set_name("many");
    let mut values = many.init_value(2);
    values.reborrow().get(0).set_string("a");
    values.get(1).set_int64(2);
    let mut none = list.reborrow().get(5);
    none.set_name("none");
    none.init_value(0);
    let mut strs = one(list.reborrow().get(6), "strs").init_string_array(2);
    strs.reborrow().get(0).set_value("x");
    strs.get(1).set_none(());
    let mut bools = one(list.reborrow().get(7), "bools").init_bool_array(2);
    bools.reborrow().get(0).set_value(false);
    bools.get(1).set_none(());
    let mut floats = one(list.get(8), "floats").init_float64_array(2);
    floats.reborrow().get(0).set_value(1.5);
    floats.get(1).set_none(());
}

fn set_fetch_info(mut fetch: trace::fetch_event_info::Builder<'_>) {
    set_raw_enum(fetch.reborrow(), "method", 2);
    fetch.set_url("https://example.com/path?q=1");
    fetch.set_cf_json(r#"{"colo":"LHR","asn":13335}"#);
    let mut headers = fetch.init_headers(2);
    headers.reborrow().get(0).set_name("content-type");
    headers.reborrow().get(0).set_value("text/plain");
    headers.reborrow().get(1).set_name("x-custom");
    headers.get(1).set_value("vé");
}

/// Converts each event of `params` with `options` and returns their dumps.
fn dump_events(
    lock: &mut jsg::Lock<'_>,
    ctx: &EvalContext<'_>,
    params: tail_stream_params::Reader<'_>,
    options: Options,
) -> Vec<String> {
    ctx.eval_raw(lock, DUMP).unwrap();
    let events = TailEvents::new(params).unwrap();
    // One `Keys` for the batch, as `TailStreamTarget` passes, so later events reuse its names.
    let mut keys = Keys::default();
    (0..events.len())
        .map(|index| {
            let value = events
                .get(index)
                .unwrap()
                .to_js(lock, &mut keys, options)
                .unwrap();
            ctx.set_global(lock, "value", value);
            ctx.eval::<String>(lock, "dump(value)").unwrap()
        })
        .collect()
}

/// The dump of the `event` of the single event that `build` builds.
fn dump_event(build: impl Fn(tail_event::event::Builder<'_>)) -> String {
    dump_event_with(Options::default(), build)
}

fn dump_event_with(options: Options, build: impl Fn(tail_event::event::Builder<'_>)) -> String {
    let message = params(1, |_, event| build(event.get_event()));
    let mut dumps = Vec::new();
    Harness::new().run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, DUMP).unwrap();
        let events = TailEvents::new(read(&message)).unwrap();
        let value = events
            .get(0)
            .unwrap()
            .to_js(lock, &mut Keys::default(), options)
            .unwrap();
        ctx.set_global(lock, "value", value);
        dumps.push(ctx.eval::<String>(lock, "dump(value.event)").unwrap());
        Ok(())
    });
    dumps.pop().unwrap()
}

/// The error that checking the single event that `build` builds fails with.
fn check_error(build: impl Fn(tail_event::event::Builder<'_>)) -> String {
    let message = params(1, |_, event| build(event.get_event()));
    match TailEvents::new(read(&message)) {
        Ok(_) => panic!("the event should be rejected"),
        Err(err) => err.to_string(),
    }
}

#[test]
fn params_without_events_are_rejected() {
    let mut message = Builder::new_default();
    message.init_root::<tail_stream_params::Builder<'_>>();
    let Err(err) = TailEvents::new(read(&message)) else {
        panic!("params without events should be rejected");
    };
    assert_eq!(err.to_string(), "Events are required.");
}

#[test]
fn an_empty_batch_is_accepted() {
    let message = params(0, |_, _| {});
    let events = TailEvents::new(read(&message)).unwrap();
    assert!(events.is_empty());
    assert_eq!(events.len(), 0);
}

#[test]
fn an_http_method_past_ban_is_rejected() {
    assert_eq!(
        check_error(|event| set_raw_enum(
            event.init_onset().init_info().init_fetch(),
            "method",
            28
        )),
        "unknown method; method = 28"
    );
    assert_eq!(
        check_error(|event| {
            set_raw_enum(
                event.init_span_open().get_info().init_fetch(),
                "method",
                1000,
            );
        }),
        "unknown method; method = 1000"
    );
}

#[test]
fn every_http_method_has_its_kj_name() {
    let names = [(0, "GET"), (22, "MSEARCH"), (26, "QUERY"), (27, "BAN")];
    for (method, name) in names {
        let dump = dump_event(|event| {
            set_raw_enum(
                event.init_span_open().get_info().init_fetch(),
                "method",
                method,
            );
        });
        assert!(dump.contains(&format!(r#"method: "{name}""#)), "{dump}");
    }
}

#[test]
fn a_stream_diagnostics_event_without_a_diagnostic_is_rejected() {
    assert_eq!(
        check_error(|event| {
            event.init_stream_diagnostics();
        }),
        "received invalid diagnostics event"
    );
}

#[test]
fn an_attribute_without_a_value_is_rejected() {
    assert_eq!(
        check_error(|event| {
            event.init_attribute(1).get(0).set_name("bare");
        }),
        "attribute has no value"
    );
    assert_eq!(
        check_error(|event| {
            event
                .init_onset()
                .init_attributes(1)
                .get(0)
                .set_name("bare");
        }),
        "attribute has no value"
    );
    assert_eq!(
        check_error(|event| {
            let mut links = event.init_span_links().init_links(1);
            links
                .reborrow()
                .get(0)
                .init_attributes(1)
                .get(0)
                .set_name("bare");
        }),
        "attribute has no value"
    );
}

#[test]
fn a_bad_event_anywhere_in_the_batch_rejects_it() {
    let message = params(3, |index, mut event| {
        set_common(event.reborrow(), index);
        if index == 2 {
            event.get_event().init_stream_diagnostics();
        } else {
            event.get_event().init_span_close();
        }
    });
    assert!(TailEvents::new(read(&message)).is_err());
}

#[test]
fn kinds_and_handler_names() {
    let message = params(12, |index, event| {
        let event = event.get_event();
        match index {
            0 => {
                event.init_onset();
            }
            1 => {
                event.init_outcome();
            }
            2 => {
                event.init_span_open();
            }
            3 => {
                event.init_span_close();
            }
            4 => event.init_span_update().get_info().set_name("x"),
            5 => {
                event.init_span_links();
            }
            6 => {
                event.init_diagnostic_channel_event();
            }
            7 => {
                event.init_exception();
            }
            8 => {
                event.init_log();
            }
            9 => {
                event
                    .init_stream_diagnostics()
                    .get_diagnostic()
                    .init_dropped_events()
                    .set_count(1);
            }
            10 => {
                event.init_return();
            }
            _ => {
                event.init_attribute(0);
            }
        }
    });
    let events = TailEvents::new(read(&message)).unwrap();
    let found: Vec<_> = (0..events.len())
        .map(|index| {
            (
                events.get(index).unwrap().kind(),
                events.get(index).unwrap().handler_name(),
            )
        })
        .collect();
    assert_eq!(
        found,
        [
            (Kind::Onset, None),
            (Kind::Outcome, Some("outcome")),
            (Kind::Other, Some("spanOpen")),
            (Kind::Other, Some("spanClose")),
            (Kind::Other, Some("spanUpdate")),
            (Kind::Other, Some("spanLinks")),
            (Kind::Other, Some("diagnosticChannel")),
            (Kind::Other, Some("exception")),
            (Kind::Other, Some("log")),
            (Kind::Other, Some("streamDiagEvent")),
            (Kind::Other, Some("return")),
            (Kind::Other, Some("attributes")),
        ]
    );
}

#[test]
fn a_full_onset() {
    let message = params(1, |index, mut event| {
        set_common(event.reborrow(), index);
        let mut onset = event.get_event().init_onset();
        onset.set_execution_model(trace::ExecutionModel::DurableObject);
        onset.set_span_id(0xcd);
        onset.set_dispatch_namespace("my-namespace");
        onset.set_entry_point("function");
        onset.set_durable_object_id("do-id");
        onset.set_script_name("my-script");
        let mut tags = onset.reborrow().init_script_tags(2);
        tags.set(0, "a");
        tags.set(1, "b");
        let mut version = onset.reborrow().init_script_version();
        version.reborrow().get_id().set_upper(0x0123_4567_89ab_cdef);
        version.reborrow().get_id().set_lower(0x0fed_cba9_8765_4321);
        version.set_tag("v1");
        version.set_message("first");
        let mut preview = onset.reborrow().init_preview();
        preview.set_id("preview-id");
        preview.set_slug("preview-slug");
        preview.set_name("preview-name");
        set_fetch_info(onset.reborrow().init_info().init_fetch());
        set_attributes(onset.init_attributes(9));
    });
    Harness::new().run_in_context(|lock, ctx| {
        let dumps = dump_events(lock, ctx, read(&message), Options::default());
        assert_eq!(
            dumps,
            [format!(
                r#"{{{COMMON}, sequence: 0, event: {{type: "onset", executionModel: "durableObject", spanId: "00000000000000cd", dispatchNamespace: "my-namespace", entrypoint: "function", durableObjectId: "do-id", scriptName: "my-script", scriptTags: ["a", "b"], scriptVersion: {{id: "01234567-89ab-cdef-0fed-cba987654321", tag: "v1", message: "first"}}, preview: {{id: "preview-id", slug: "preview-slug", name: "preview-name"}}, info: {FETCH_INFO}, attributes: {ATTRIBUTES}}}}}"#
            )]
        );
        Ok(())
    });
}

#[test]
fn a_bare_event_has_short_ids_and_no_optional_fields() {
    let message = params(1, |_, mut event| {
        let mut context = event.reborrow().init_span_context();
        context.reborrow().init_trace_id().set_low(0x2a);
        event.reborrow().init_invocation_id().set_low(0x2b);
        event.set_timestamp_ns(-1_500_000);
        let mut onset = event.get_event().init_onset();
        onset.reborrow().init_info().init_custom();
        // A script version without an ID, tag or message.
        onset.init_script_version();
    });
    Harness::new().run_in_context(|lock, ctx| {
        let dumps = dump_events(lock, ctx, read(&message), Options::default());
        assert_eq!(
            dumps,
            [r#"{spanContext: {traceId: "000000000000002a"}, invocationId: "000000000000002b", timestamp: Date(-1), sequence: 0, event: {type: "onset", executionModel: "stateless", spanId: "0000000000000000", scriptVersion: {}, info: {type: "custom"}}}"#]
        );
        Ok(())
    });
}

#[test]
fn unknown_execution_models_and_reasons_are_numbers() {
    let dump = dump_event(|event| {
        let mut onset = event.init_onset();
        set_raw_enum(onset.reborrow(), "executionModel", 7);
        set_raw_enum(onset.init_info().init_on_hibernate(), "reason", 9);
    });
    assert_eq!(
        dump,
        r#"{type: "onset", executionModel: "7", spanId: "0000000000000000", info: {type: "onHibernate", reason: "9"}}"#
    );
}

#[test]
fn onset_infos() {
    let infos: [(&str, BuildInfo); 12] = [
        (r#"{type: "jsrpc"}"#, |info| {
            info.init_js_rpc();
        }),
        (r#"{type: "connect"}"#, |info| {
            info.init_connect();
        }),
        (r#"{type: "custom"}"#, |info| {
            info.init_custom();
        }),
        (
            r#"{type: "scheduled", scheduledTime: Date(1234), cron: "* * * * *"}"#,
            |info| {
                let mut scheduled = info.init_scheduled();
                scheduled.set_scheduled_time(1234.5);
                scheduled.set_cron("* * * * *");
            },
        ),
        (
            r#"{type: "alarm", scheduledTime: Date(1700000000123)}"#,
            |info| {
                info.init_alarm().set_scheduled_time_ms(1_700_000_000_123);
            },
        ),
        (r#"{type: "onHibernate", reason: "codeUpdated"}"#, |info| {
            info.init_on_hibernate()
                .set_reason(trace::on_hibernate_event_info::Reason::CodeUpdated);
        }),
        (
            r#"{type: "queue", queueName: "q", batchSize: 4294967295}"#,
            |info| {
                let mut queue = info.init_queue();
                queue.set_queue_name("q");
                queue.set_batch_size(u32::MAX);
            },
        ),
        (
            r#"{type: "email", mailFrom: "a@example.com", rcptTo: "b@example.com", rawSize: 42}"#,
            |info| {
                let mut email = info.init_email();
                email.set_mail_from("a@example.com");
                email.set_rcpt_to("b@example.com");
                email.set_raw_size(42);
            },
        ),
        (r#"{type: "trace", traces: ["traced", ""]}"#, |info| {
            let mut traces = info.init_trace().init_traces(2);
            traces.reborrow().get(0).set_script_name("traced");
            // A trace item without a script name.
            traces.get(1);
        }),
        (
            r#"{type: "hibernatableWebSocket", info: {type: "message"}}"#,
            |info| {
                info.init_hibernatable_web_socket()
                    .get_type()
                    .set_message(());
            },
        ),
        (
            r#"{type: "hibernatableWebSocket", info: {type: "error"}}"#,
            |info| {
                info.init_hibernatable_web_socket().get_type().set_error(());
            },
        ),
        (
            r#"{type: "hibernatableWebSocket", info: {type: "close", code: 1011, wasClean: true}}"#,
            |info| {
                let mut close = info.init_hibernatable_web_socket().get_type().init_close();
                close.set_code(1011);
                close.set_was_clean(true);
            },
        ),
    ];
    for (expected, build) in infos {
        let dump = dump_event(|event| build(event.init_onset().init_info()));
        assert_eq!(
            dump,
            format!(
                r#"{{type: "onset", executionModel: "stateless", spanId: "0000000000000000", info: {expected}}}"#
            )
        );
    }
}

#[test]
fn predictable_mode_hides_scheduled_times() {
    let options = Options { predictable: true };
    let scheduled = dump_event_with(options, |event| {
        event
            .init_onset()
            .init_info()
            .init_scheduled()
            .set_scheduled_time(1234.0);
    });
    assert!(scheduled.contains("scheduledTime: Date(0)"), "{scheduled}");
    let alarm = dump_event_with(options, |event| {
        event
            .init_onset()
            .init_info()
            .init_alarm()
            .set_scheduled_time_ms(1234);
    });
    assert!(alarm.contains("scheduledTime: Date(0)"), "{alarm}");
}

#[test]
fn spans() {
    let events: [(&str, BuildEvent); 8] = [
        (
            r#"{type: "spanOpen", name: "", spanId: "0000000000000000"}"#,
            |event| {
                event.init_span_open();
            },
        ),
        (
            r#"{type: "spanOpen", name: "rpc", spanId: "00000000000000ef", info: {type: "jsrpc"}}"#,
            |event| {
                let mut open = event.init_span_open();
                open.set_operation_name("rpc");
                open.set_span_id(0xef);
                open.get_info().init_js_rpc();
            },
        ),
        (
            r#"{type: "spanUpdate", info: {type: "name", name: "renamed"}}"#,
            |event| {
                event.init_span_update().get_info().set_name("renamed");
            },
        ),
        (
            r#"{type: "spanUpdate", info: {type: "status", status: {code: "unset"}}}"#,
            |event| {
                let mut status = event.init_span_update().get_info().init_status();
                status.set_code(trace_capnp::SpanStatusCode::Unset);
                status.get_message().set_text("dropped");
            },
        ),
        (
            r#"{type: "spanUpdate", info: {type: "status", status: {code: "ok"}}}"#,
            |event| {
                let mut status = event.init_span_update().get_info().init_status();
                status.set_code(trace_capnp::SpanStatusCode::Ok);
                status.get_message().set_text("dropped");
            },
        ),
        (
            r#"{type: "spanUpdate", info: {type: "status", status: {code: "error", message: "failed"}}}"#,
            |event| {
                let mut status = event.init_span_update().get_info().init_status();
                status.set_code(trace_capnp::SpanStatusCode::Error);
                status.get_message().set_text("failed");
            },
        ),
        (
            r#"{type: "spanUpdate", info: {type: "status", status: {code: "unset"}}}"#,
            |event| {
                let mut status = event.init_span_update().get_info().init_status();
                set_raw_enum(status.reborrow(), "code", 3);
                status.get_message().set_text("dropped");
            },
        ),
        (
            r#"{type: "spanClose", outcome: "exceededWallTime"}"#,
            |event| {
                event
                    .init_span_close()
                    .set_outcome(outcome_capnp::EventOutcome::ExceededWallTime);
            },
        ),
    ];
    for (expected, build) in events {
        assert_eq!(dump_event(build), expected);
    }
}

#[test]
fn fetch_span_info() {
    let dump = dump_event(|event| {
        let mut open = event.init_span_open();
        open.set_operation_name("fetch");
        set_fetch_info(open.get_info().init_fetch());
    });
    assert_eq!(
        dump,
        format!(
            r#"{{type: "spanOpen", name: "fetch", spanId: "0000000000000000", info: {FETCH_INFO}}}"#
        )
    );
    let without_cf = dump_event(|event| {
        event.init_span_open().get_info().init_fetch().set_url("u");
    });
    assert_eq!(
        without_cf,
        r#"{type: "spanOpen", name: "", spanId: "0000000000000000", info: {type: "fetch", method: "GET", url: "u", headers: []}}"#
    );
}

#[test]
fn custom_span_info_and_attributes() {
    let open = dump_event(|event| {
        let mut open = event.init_span_open();
        open.set_operation_name("custom");
        set_attributes(open.get_info().init_custom(9));
    });
    assert_eq!(
        open,
        format!(
            r#"{{type: "spanOpen", name: "custom", spanId: "0000000000000000", info: {{type: "attributes", info: {ATTRIBUTES}}}}}"#
        )
    );
    let attributes = dump_event(|event| set_attributes(event.init_attribute(9)));
    assert_eq!(
        attributes,
        format!(r#"{{type: "attributes", info: {ATTRIBUTES}}}"#)
    );
}

#[test]
fn span_links() {
    let dump = dump_event(|event| {
        let mut links = event.init_span_links().init_links(2);
        let mut first = links.reborrow().get(0);
        let mut context = first.reborrow().init_context();
        context.reborrow().init_trace_id().set_low(0x2a);
        context.reborrow().get_info().set_span_id(0x3b);
        context.init_trace_flags().get_value().set_set(3);
        set_attributes(first.reborrow().init_attributes(9));
        first.set_dropped_attributes_count(7);
        let mut second = links.get(1).init_context();
        second.reborrow().init_trace_id().set_high(1);
        // Trace flags that are not set.
        second.init_trace_flags().get_value().set_unset(());
    });
    assert_eq!(
        dump,
        format!(
            r#"{{type: "spanLinks", links: [{{context: {{traceId: "0000000000000000000000000000002a", spanId: "000000000000003b", traceFlags: 3}}, attributes: {ATTRIBUTES}, droppedAttributesCount: 7}}, {{context: {{traceId: "00000000000000010000000000000000"}}, attributes: [], droppedAttributesCount: 0}}]}}"#
        )
    );
}

#[test]
fn outcomes() {
    use outcome_capnp::EventOutcome;
    let outcomes = [
        (EventOutcome::Unknown, "unknown"),
        (EventOutcome::Ok, "ok"),
        (EventOutcome::Exception, "exception"),
        (EventOutcome::ExceededCpu, "exceededCpu"),
        (EventOutcome::KillSwitch, "killSwitch"),
        (EventOutcome::DaemonDown, "daemonDown"),
        (EventOutcome::ScriptNotFound, "scriptNotFound"),
        (EventOutcome::Canceled, "canceled"),
        (EventOutcome::ExceededMemory, "exceededMemory"),
        (EventOutcome::LoadShed, "loadShed"),
        (
            EventOutcome::ResponseStreamDisconnected,
            "responseStreamDisconnected",
        ),
        (EventOutcome::InternalError, "internalError"),
        (EventOutcome::ExceededWallTime, "exceededWallTime"),
        (EventOutcome::Aborted, "aborted"),
    ];
    for (outcome, name) in outcomes {
        let dump = dump_event(|event| {
            let mut event = event.init_outcome();
            event.set_outcome(outcome);
            event.set_cpu_time(12);
            event.set_wall_time(345);
        });
        assert_eq!(
            dump,
            format!(r#"{{type: "outcome", outcome: "{name}", cpuTime: 12, wallTime: 345}}"#)
        );
    }
}

#[test]
fn logs() {
    use trace::log::Level;
    let levels = [
        (Level::Debug, "debug"),
        (Level::Info, "info"),
        (Level::Log, "log"),
        (Level::Warn, "warn"),
        (Level::Error, "error"),
    ];
    for (level, name) in levels {
        let dump = dump_event(|event| {
            let mut log = event.init_log();
            log.set_log_level(level);
            log.set_message(r#"["hello",1]"#);
        });
        assert_eq!(
            dump,
            format!(r#"{{type: "log", level: "{name}", message: ["hello", 1]}}"#)
        );
    }

    let truncated = dump_event(|event| {
        let mut log = event.init_log();
        log.set_message(r#"["cut"#);
        log.set_truncated(true);
    });
    assert_eq!(
        truncated,
        r#"{type: "log", level: "debug", message: "[\"cut", truncated: true}"#
    );

    let error_info = dump_event(|event| {
        let mut log = event.init_log();
        log.set_message("[]");
        let mut slots = log.init_error_info(3);
        slots.reborrow().get(0).set_none(());
        let mut info = slots.reborrow().get(1).init_info();
        info.set_name("TypeError");
        info.set_message("bad");
        info.set_stack("at f");
        let mut info = slots.get(2).init_info();
        info.set_name("Error");
        info.set_message("no stack");
    });
    assert_eq!(
        error_info,
        r#"{type: "log", level: "debug", message: [], errorInfo: [null, {name: "TypeError", message: "bad", stack: "at f"}, {name: "Error", message: "no stack"}]}"#
    );
}

#[test]
fn exceptions() {
    let text = dump_event(|event| {
        let mut exception = event.init_exception();
        exception.set_name("Error");
        exception.set_message("boom");
        exception.set_stack("at f");
        exception.get_code().set_text("E_BOOM");
    });
    assert_eq!(
        text,
        r#"{type: "exception", code: "E_BOOM", name: "Error", message: "boom", stack: "at f"}"#
    );
    let number = dump_event(|event| {
        let mut exception = event.init_exception();
        exception.set_name("Error");
        exception.get_code().set_number(-0.0);
    });
    assert_eq!(
        number,
        r#"{type: "exception", code: -0, name: "Error", message: ""}"#
    );
    let none = dump_event(|event| {
        event.init_exception();
    });
    assert_eq!(none, r#"{type: "exception", name: "", message: ""}"#);
}

#[test]
fn returns_and_stream_diagnostics() {
    assert_eq!(
        dump_event(|event| {
            event.init_return();
        }),
        r#"{type: "return"}"#
    );
    assert_eq!(
        dump_event(|event| event
            .init_return()
            .get_info()
            .init_fetch()
            .set_status_code(404)),
        r#"{type: "return", info: {type: "fetch", statusCode: 404}}"#
    );
    assert_eq!(
        dump_event(|event| {
            event
                .init_stream_diagnostics()
                .get_diagnostic()
                .init_dropped_events()
                .set_count(5);
        }),
        r#"{type: "streamDiagnostic", diagnostic: {diagnosticsType: "droppedEvents", count: 5}}"#
    );
}

#[test]
fn diagnostics_channel_messages_are_deserialized() {
    Harness::new().run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, DUMP).unwrap();
        let payload = ctx
            .eval_raw(lock, "({ map: new Map([[1, 'one']]), set: new Set([2n]), date: new Date(5), re: /x/g, u: undefined })")
            .unwrap();
        let data = jsg::ser::serialize(lock, payload)?;
        let message = params(1, |_, event| {
            let mut dce = event.get_event().init_diagnostic_channel_event();
            dce.set_channel("my-channel");
            dce.set_message(&data);
        });
        let events = TailEvents::new(read(&message)).unwrap();
        let value = events.get(0).unwrap().to_js(lock, &mut Keys::default(), Options::default()).unwrap();
        ctx.set_global(lock, "value", value);
        assert_eq!(
            ctx.eval::<String>(lock, "dump(value.event)").unwrap(),
            r#"{type: "diagnosticChannel", channel: "my-channel", message: {map: Map{1 => "one"}, set: Set{2n}, date: Date(5), re: RegExp(/x/g), u: undefined}}"#
        );
        Ok(())
    });
}

/// What `to_js` fails with for the single event that `build` builds, which the check accepts:
/// the exception thrown, an error that is not pending, or the message of an invalid event.
fn to_js_error(build: impl Fn(tail_event::event::Builder<'_>)) -> String {
    let message = params(1, |_, event| build(event.get_event()));
    let mut found = None;
    Harness::new().run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, DUMP).unwrap();
        let events = TailEvents::new(read(&message)).unwrap();
        let mut not_thrown = None;
        let caught = lock.try_catch(|lock| {
            match events
                .get(0)
                .unwrap()
                .to_js(lock, &mut Keys::default(), Options::default())
            {
                Ok(_) => panic!("the event should not convert"),
                Err(ConvertError::Js(err)) if err.is_pending() => Err(err),
                Err(ConvertError::Js(err)) => {
                    not_thrown = Some(format!("not pending: {err}"));
                    Ok(())
                }
                Err(ConvertError::Unreachable) => {
                    not_thrown = Some("unreachable".to_owned());
                    Ok(())
                }
                Err(ConvertError::DiagnosticChannelHeader) => {
                    not_thrown = Some("diagnostics channel header".to_owned());
                    Ok(())
                }
                Err(ConvertError::Invalid(err)) => {
                    not_thrown = Some(format!("invalid: {err}"));
                    Ok(())
                }
            }
        })?;
        found = Some(match (caught, not_thrown) {
            (Err(exception), None) => {
                ctx.set_global(lock, "value", exception);
                ctx.eval::<String>(lock, "`${value.name}: ${value.message}`")
                    .unwrap()
            }
            (Ok(()), Some(not_thrown)) => not_thrown,
            _ => unreachable!(),
        });
        Ok(())
    });
    found.unwrap()
}

#[test]
fn invalid_json_throws_a_syntax_error() {
    let log = to_js_error(|event| event.init_log().set_message("{"));
    assert!(log.starts_with("SyntaxError: "), "{log}");
    let cf = to_js_error(|event| {
        let mut fetch = event.init_span_open().get_info().init_fetch();
        fetch.set_cf_json("not json");
    });
    assert!(cf.starts_with("SyntaxError: "), "{cf}");
}

#[test]
fn malformed_diagnostics_channel_data_throws() {
    let error = to_js_error(|event| {
        event
            .init_diagnostic_channel_event()
            .set_message(&[0xff, 0x0f, 0x7b]);
    });
    assert!(!error.starts_with("invalid: "), "{error}");
}

#[test]
fn diagnostics_channel_data_without_a_header_fails_without_throwing() {
    let error = to_js_error(|event| {
        event
            .init_diagnostic_channel_event()
            .set_message(&[0x22, 0x01, 0x61]);
    });
    assert_eq!(error, "diagnostics channel header");
}

#[test]
fn unknown_outcomes_and_log_levels_are_unreachable() {
    assert_eq!(
        to_js_error(|event| set_raw_enum(event.init_outcome(), "outcome", 99)),
        "unreachable"
    );
    assert_eq!(
        to_js_error(|event| set_raw_enum(event.init_span_close(), "outcome", 14)),
        "unreachable"
    );
    assert_eq!(
        to_js_error(|event| {
            let mut log = event.init_log();
            log.set_message("1");
            set_raw_enum(log, "logLevel", 5);
        }),
        "unreachable"
    );
}

#[test]
fn setters_on_object_prototype_see_each_assignment_in_order() {
    let message = params(1, |index, mut event| {
        set_common(event.reborrow(), index);
        let mut log = event.get_event().init_log();
        log.set_message("1");
        log.init_error_info(1).get(0).init_info().set_name("E");
    });
    Harness::new().run_in_context(|lock, ctx| {
        ctx.eval_raw(
            lock,
            r#"
              globalThis.sets = [];
              for (const key of ["spanContext", "traceId", "spanId", "traceFlags", "invocationId",
                                 "timestamp", "sequence", "event", "type", "level", "message",
                                 "errorInfo", "name"]) {
                Object.defineProperty(Object.prototype, key, {
                  set(value) {
                    sets.push(key);
                    Object.defineProperty(this, key, { value, writable: true, enumerable: true,
                                                        configurable: true });
                  },
                  configurable: true,
                });
              }
            "#,
        )
        .unwrap();
        let events = TailEvents::new(read(&message)).unwrap();
        events
            .get(0)
            .unwrap()
            .to_js(lock, &mut Keys::default(), Options::default())
            .unwrap();
        assert_eq!(
            ctx.eval::<String>(lock, "sets.join(' ')").unwrap(),
            "traceId spanId traceFlags spanContext invocationId timestamp sequence type level \
             message name message errorInfo event"
        );
        Ok(())
    });
}
