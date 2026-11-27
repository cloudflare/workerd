// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use capnp::message::HeapAllocator;

use super::*;

fn round_trip(info: &EventInfo) -> EventInfo {
    let mut message = Builder::new(HeapAllocator::new());
    info.to_capnp(message.init_root());
    EventInfo::from_capnp(message.get_root_as_reader().unwrap()).unwrap()
}

#[test]
fn every_variant_round_trips() {
    let infos = [
        EventInfo::Fetch(FetchEventInfo {
            method: HttpMethod::Post,
            url: b"https://example.com/\xff".to_vec(),
            cf_json: Vec::new(),
            headers: vec![FetchHeader {
                name: b"content-type".to_vec(),
                value: b"text/plain".to_vec(),
            }],
        }),
        EventInfo::JsRpc(JsRpcEventInfo {
            method_name: b"run".to_vec(),
        }),
        EventInfo::Scheduled(ScheduledEventInfo {
            scheduled_time: 1.5,
            cron: b"* * * * *".to_vec(),
        }),
        EventInfo::Alarm(AlarmEventInfo {
            scheduled_time_ms: -1,
        }),
        EventInfo::Queue(QueueEventInfo {
            queue_name: b"jobs".to_vec(),
            batch_size: 7,
        }),
        EventInfo::Email(EmailEventInfo {
            mail_from: b"a@example.com".to_vec(),
            rcpt_to: b"b@example.com".to_vec(),
            raw_size: 1024,
        }),
        EventInfo::Trace(TraceEventInfo {
            traces: vec![TraceItem {
                script_name: Some(b"worker".to_vec()),
            }],
        }),
        EventInfo::HibernatableWebSocket(HibernatableWebSocketEventInfo::Close {
            code: 1000,
            was_clean: true,
        }),
        EventInfo::Connect,
        EventInfo::OnHibernate(OnHibernateEventInfo {
            reason: OnHibernateReason::CodeUpdated,
        }),
        EventInfo::Custom,
    ];
    for info in &infos {
        assert_eq!(&round_trip(info), info);
    }
}

#[test]
fn a_trace_item_without_a_script_name_reads_back_empty() {
    let info = EventInfo::Trace(TraceEventInfo {
        traces: vec![TraceItem { script_name: None }],
    });
    assert_eq!(
        round_trip(&info),
        EventInfo::Trace(TraceEventInfo {
            traces: vec![TraceItem {
                script_name: Some(Vec::new()),
            }],
        })
    );
}

#[test]
fn the_other_method_is_rejected() {
    let mut message = Builder::new(HeapAllocator::new());
    message
        .init_root::<trace::fetch_event_info::Builder<'_>>()
        .set_method(HttpMethod::Other);
    let reader = message
        .get_root_as_reader::<trace::fetch_event_info::Reader<'_>>()
        .unwrap();
    assert!(FetchEventInfo::from_capnp(reader).is_err());
}

#[test]
fn an_unknown_hibernation_reason_is_rejected() {
    // A `FetchResponseInfo` keeps its status code where an `OnHibernateEventInfo` keeps its
    // reason, which lets the test write a reason the schema doesn't know.
    let mut message = Builder::new(HeapAllocator::new());
    message
        .init_root::<trace::fetch_response_info::Builder<'_>>()
        .set_status_code(3);
    let reader = message
        .get_root_as_reader::<trace::on_hibernate_event_info::Reader<'_>>()
        .unwrap();
    assert!(OnHibernateEventInfo::from_capnp(reader).is_err());
}

#[test]
fn alarm_times_past_what_cpp_holds_round_trip() {
    for scheduled_time_ms in [i64::MAX, i64::MIN] {
        let info = EventInfo::Alarm(AlarmEventInfo { scheduled_time_ms });
        assert_eq!(round_trip(&info), info);
    }
}

#[test]
fn a_custom_info_is_not_read() {
    // A `QueueEventInfo` keeps its batch size where an onset info keeps its variant (8 is
    // `custom`), and its queue name where the info keeps the variant's struct, which lets the
    // test put text where the custom event's struct would be.
    let mut message = Builder::new(HeapAllocator::new());
    let mut queue = message.init_root::<trace::queue_event_info::Builder<'_>>();
    queue.set_batch_size(8);
    queue.set_queue_name("not a struct");
    let reader = message
        .get_root_as_reader::<onset::info::Reader<'_>>()
        .unwrap();
    assert_eq!(EventInfo::from_capnp(reader).unwrap(), EventInfo::Custom);
}
