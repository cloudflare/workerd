//! Checks that Rust trace points (`//src/rust/perfetto`) and C++ trace points are recorded in the
//! same Perfetto session, with matching flow, track and counter track IDs.

#[cxx::bridge(namespace = "workerd::rust::perfetto_test")]
#[cfg_attr(not(test), expect(dead_code, reason = "only used by the tests"))]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/perfetto/test/test-helper.h");

        fn perfetto_in_build() -> bool;
        fn start_trace(categories: &str) -> Result<()>;
        fn emit_cpp_events(address: usize);
        fn stop_trace() -> Result<Vec<u8>>;
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use perfetto::Flow;
    use perfetto::Track;
    use perfetto::trace_counter;
    use perfetto::trace_event;
    use perfetto::trace_event_begin;
    use perfetto::trace_event_category_enabled;
    use perfetto::trace_event_end;
    use perfetto::trace_event_instant;

    use super::ffi;

    // TrackEvent.Type
    const TYPE_SLICE_BEGIN: u64 = 1;
    const TYPE_SLICE_END: u64 = 2;
    const TYPE_COUNTER: u64 = 4;

    enum Field<'a> {
        Varint(u64),
        Fixed64(u64),
        Bytes(&'a [u8]),
        Fixed32,
    }

    fn read_varint(data: &mut &[u8]) -> u64 {
        let mut result = 0u64;
        let mut shift = 0;
        loop {
            let (&byte, rest) = data.split_first().unwrap();
            *data = rest;
            result |= u64::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                return result;
            }
            shift += 7;
        }
    }

    // Minimal protobuf decoder: yields (field number, value) for each field of a message.
    fn fields(mut data: &[u8]) -> Vec<(u64, Field<'_>)> {
        let mut result = Vec::new();
        while !data.is_empty() {
            let key = read_varint(&mut data);
            let field = match key & 7 {
                0 => Field::Varint(read_varint(&mut data)),
                1 => {
                    let (bytes, rest) = data.split_at(8);
                    data = rest;
                    Field::Fixed64(u64::from_le_bytes(bytes.try_into().unwrap()))
                }
                2 => {
                    let len = usize::try_from(read_varint(&mut data)).unwrap();
                    let (bytes, rest) = data.split_at(len);
                    data = rest;
                    Field::Bytes(bytes)
                }
                5 => {
                    data = &data[4..];
                    Field::Fixed32
                }
                wire_type => panic!("unsupported wire type {wire_type}"),
            };
            result.push((key >> 3, field));
        }
        result
    }

    #[derive(Debug, Default)]
    struct Event {
        name: Option<String>,
        kind: u64,
        track_uuid: Option<u64>,
        flow_ids: Vec<u64>,
        terminating_flow_ids: Vec<u64>,
        counter_value: Option<f64>,
        debug_annotations: usize,
    }

    // Decodes the track events of a serialized perfetto.protos.Trace, resolving interned names.
    fn parse_trace(trace: &[u8]) -> Vec<Event> {
        let mut interned_names = HashMap::new();
        let mut events = Vec::new();
        for (id, packet) in fields(trace) {
            let Field::Bytes(packet) = packet else {
                continue;
            };
            if id != 1 {
                continue;
            }
            let packet = fields(packet);
            let sequence = packet
                .iter()
                .find_map(|(id, f)| match (id, f) {
                    // trusted_packet_sequence_id
                    (10, Field::Varint(v)) => Some(*v),
                    _ => None,
                })
                .unwrap_or(0);
            // Interned data comes before the events that use it within a packet sequence.
            for (id, field) in &packet {
                if let (12, Field::Bytes(interned)) = (id, field) {
                    for (id, entry) in fields(interned) {
                        // InternedData.event_names
                        let (2, Field::Bytes(entry)) = (id, entry) else {
                            continue;
                        };
                        let mut iid = None;
                        let mut name = None;
                        for (id, f) in fields(entry) {
                            match (id, f) {
                                (1, Field::Varint(v)) => iid = Some(v),
                                (2, Field::Bytes(b)) => {
                                    name = Some(String::from_utf8(b.to_vec()).unwrap());
                                }
                                _ => {}
                            }
                        }
                        interned_names.insert((sequence, iid.unwrap()), name.unwrap());
                    }
                }
            }
            for (id, field) in &packet {
                let (11, Field::Bytes(track_event)) = (id, field) else {
                    continue;
                };
                let mut event = Event::default();
                for (id, f) in fields(track_event) {
                    match (id, f) {
                        (9, Field::Varint(v)) => event.kind = v,
                        (10, Field::Varint(iid)) => {
                            event.name = interned_names.get(&(sequence, iid)).cloned();
                        }
                        (23, Field::Bytes(b)) => {
                            event.name = Some(String::from_utf8(b.to_vec()).unwrap());
                        }
                        (11, Field::Varint(v)) => event.track_uuid = Some(v),
                        (4, Field::Bytes(_)) => event.debug_annotations += 1,
                        (47 | 36, Field::Fixed64(v) | Field::Varint(v)) => event.flow_ids.push(v),
                        (48, Field::Fixed64(v) | Field::Varint(v)) => {
                            event.terminating_flow_ids.push(v);
                        }
                        #[expect(clippy::cast_precision_loss, reason = "small test values")]
                        (30, Field::Varint(v)) => {
                            event.counter_value = Some(v.cast_signed() as f64);
                        }
                        (44, Field::Fixed64(v)) => event.counter_value = Some(f64::from_bits(v)),
                        _ => {}
                    }
                }
                events.push(event);
            }
        }
        events
    }

    fn named<'a>(events: &'a [Event], name: &str) -> Option<&'a Event> {
        events.iter().find(|e| e.name.as_deref() == Some(name))
    }

    fn fnv1a(s: &str) -> u64 {
        let mut hash = 14_695_981_039_346_656_037u64;
        for b in s.bytes() {
            hash ^= u64::from(b);
            hash = hash.wrapping_mul(1_099_511_628_211);
        }
        hash
    }

    // Takes a pointer because the events are identified by the address, not the value.
    fn emit_rust_events(token: *const u64) {
        trace_event!("workerd", "rust-scoped", |ctx| {
            ctx.add_arg("answer", 42i64)
                .add_arg("text", "hello")
                .set_terminating_flow(Flow::from_ptr(token));
        });
        trace_event_instant!("workerd", "rust-instant");
        trace_event_begin!("workerd", "rust-begin", |ctx| {
            ctx.set_track(Track::from_ptr(token));
        });
        trace_event_end!("workerd", |ctx| {
            ctx.set_track(Track::from_ptr(token));
        });
        trace_counter!("workerd", "shared-counter", 7);
    }

    #[test]
    fn cpp_and_rust_events_share_a_session() {
        ffi::start_trace("workerd").unwrap();

        let token = Box::new(0u64);
        // The C++ events start a flow and use a track derived from `token`'s address; the Rust
        // events end the flow and use the same track.
        ffi::emit_cpp_events(std::ptr::from_ref::<u64>(&token).addr());
        let rust_enabled = trace_event_category_enabled!("workerd");
        emit_rust_events(std::ptr::from_ref::<u64>(&token));

        // Computed after start_trace(), which initializes Perfetto and the process track UUID.
        let expected_flow = Flow::from_ref::<u64>(&token).id();
        let expected_track = Track::from_ref::<u64>(&token).uuid();
        let process_uuid = Track::new(0).uuid();

        let events = parse_trace(&ffi::stop_trace().unwrap());

        if !ffi::perfetto_in_build() {
            assert!(events.is_empty());
            assert!(!rust_enabled);
            return;
        }

        let cpp_scoped = named(&events, "cpp-scoped").expect("C++ event missing");
        let cpp_begin = named(&events, "cpp-begin").expect("C++ event missing");

        if !cfg!(feature = "rust_perfetto") {
            // Perfetto without Rust Perfetto: C++ events only.
            assert!(!rust_enabled);
            assert!(events.iter().all(|e| {
                e.name
                    .as_deref()
                    .is_none_or(|name| !name.starts_with("rust-"))
            }));
            return;
        }
        assert!(rust_enabled);

        assert!(cpp_scoped.flow_ids.contains(&expected_flow), "{events:#?}");
        let rust_scoped = named(&events, "rust-scoped").expect("Rust event missing");
        assert_eq!(rust_scoped.kind, TYPE_SLICE_BEGIN);
        assert!(
            rust_scoped.terminating_flow_ids.contains(&expected_flow),
            "{events:#?}"
        );
        assert_eq!(rust_scoped.debug_annotations, 2);
        assert!(named(&events, "rust-instant").is_some());

        assert_eq!(cpp_begin.track_uuid, Some(expected_track));
        let rust_begin = named(&events, "rust-begin").expect("Rust event missing");
        assert_eq!(rust_begin.track_uuid, Some(expected_track));
        assert!(
            events
                .iter()
                .filter(|e| e.kind == TYPE_SLICE_END)
                .filter(|e| e.track_uuid == Some(expected_track))
                .count()
                == 2,
            "{events:#?}"
        );

        // C++ `perfetto::CounterTrack(name)` UUID.
        let counter_track = 0xb1a4_a67d_7970_839e ^ fnv1a("shared-counter") ^ process_uuid;
        let counters: Vec<_> = events
            .iter()
            .filter(|e| e.kind == TYPE_COUNTER)
            .map(|e| (e.track_uuid, e.counter_value))
            .collect();
        assert!(
            counters.contains(&(Some(counter_track), Some(42.0))),
            "{counters:?}"
        );
        assert!(
            counters.contains(&(Some(counter_track), Some(7.0))),
            "{counters:?}"
        );
    }

    #[test]
    fn disabled_category_emits_nothing() {
        // A session that doesn't enable `workerd`.
        ffi::start_trace("v8").unwrap();
        let token = 0u64;
        assert!(!trace_event_category_enabled!("workerd"));
        emit_rust_events(&raw const token);
        ffi::emit_cpp_events(std::ptr::from_ref(&token).addr());
        let events = parse_trace(&ffi::stop_trace().unwrap());
        assert!(events.iter().all(|e| e.name.is_none()), "{events:#?}");
    }
}
