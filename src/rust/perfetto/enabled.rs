//! Backend used when Perfetto is compiled in: forwards to the Perfetto Rust SDK, which writes
//! through the Perfetto C ABI into the process's (C++-owned) Perfetto instance.

use std::ffi::CStr;

use perfetto_sdk::protos::trace::track_event::track_descriptor::TrackDescriptorFieldNumber;
use perfetto_sdk::track_event::EventContext as SdkContext;
use perfetto_sdk::track_event::TrackEvent;
use perfetto_sdk::track_event::TrackEventCounter;
use perfetto_sdk::track_event::TrackEventDebugArg;
use perfetto_sdk::track_event::TrackEventFlow;
use perfetto_sdk::track_event::TrackEventProtoField;
use perfetto_sdk::track_event::TrackEventProtoTrack;
use perfetto_sdk::track_event::TrackEventTrack;
use perfetto_sdk::track_event::TrackEventType;

use crate::CounterValue;
use crate::DebugArg;
use crate::Flow;
use crate::Track;
use crate::sdk::categories as sdk_categories;
use crate::truncate_at_nul;

pub fn register_track_events() {
    // Registers the C ABI's track_event data source. A no-op when already done.
    TrackEvent::init();
    // The only error is "already registered", which is fine.
    let _ = sdk_categories::register();
}

pub fn process_track_uuid() -> u64 {
    TrackEventTrack::process_track_uuid()
}

#[inline]
pub fn is_enabled(category: usize) -> bool {
    sdk_categories::is_category_enabled(category)
}

pub struct EventContext<'a> {
    ctx: &'a mut SdkContext,
}

impl EventContext<'_> {
    pub fn add_arg(&mut self, name: &str, value: DebugArg<'_>) {
        let value = match value {
            DebugArg::Bool(v) => TrackEventDebugArg::Bool(v),
            DebugArg::Int(v) => TrackEventDebugArg::Int64(v),
            DebugArg::Uint(v) => TrackEventDebugArg::Uint64(v),
            DebugArg::Double(v) => TrackEventDebugArg::Double(v),
            // The SDK copies strings into a CString, which must not contain NUL.
            DebugArg::Str(v) => TrackEventDebugArg::String(truncate_at_nul(v)),
            DebugArg::Pointer(v) => TrackEventDebugArg::Pointer(v),
        };
        self.ctx.add_debug_arg(name, value);
    }

    pub fn set_track(&mut self, track: Track) {
        // An anonymous child of the process track, like C++ `perfetto::Track(id)`.
        let parent = process_track_uuid();
        self.ctx.set_proto_track(&TrackEventProtoTrack {
            uuid: track.uuid(),
            fields: &[TrackEventProtoField::VarInt(
                TrackDescriptorFieldNumber::ParentUuid as u32,
                parent,
            )],
        });
    }

    pub fn set_flow(&mut self, flow: Flow) {
        self.ctx.set_flow(&TrackEventFlow::global_flow(flow.id()));
    }

    pub fn set_terminating_flow(&mut self, flow: Flow) {
        self.ctx
            .set_terminating_flow(&TrackEventFlow::global_flow(flow.id()));
    }
}

fn emit(category: usize, variant: TrackEventType, f: impl FnOnce(EventContext<'_>)) {
    let mut ctx = SdkContext::default();
    f(EventContext { ctx: &mut ctx });
    sdk_categories::emit(category, variant, &mut ctx);
}

#[inline]
pub fn emit_instant(category: usize, name: &'static CStr, f: impl FnOnce(EventContext<'_>)) {
    if is_enabled(category) {
        emit(category, TrackEventType::Instant(name.as_ptr()), f);
    }
}

/// Returns whether the event was emitted.
#[inline]
pub fn emit_begin(category: usize, name: &'static CStr, f: impl FnOnce(EventContext<'_>)) -> bool {
    if is_enabled(category) {
        emit(category, TrackEventType::SliceBegin(name.as_ptr()), f);
        true
    } else {
        false
    }
}

#[inline]
pub fn emit_end(category: usize, f: impl FnOnce(EventContext<'_>)) {
    if is_enabled(category) {
        emit(category, TrackEventType::SliceEnd, f);
    }
}

#[inline]
pub fn emit_counter(category: usize, name: &'static str, value: impl FnOnce() -> CounterValue) {
    if !is_enabled(category) {
        return;
    }
    let name = truncate_at_nul(name);
    // Same UUID as C++ `perfetto::CounterTrack(name)`, so both languages share the track.
    let parent = process_track_uuid();
    let mut ctx = SdkContext::default();
    ctx.set_proto_track(&TrackEventProtoTrack {
        uuid: TrackEventTrack::counter_track_uuid(name, parent),
        fields: &[
            TrackEventProtoField::VarInt(TrackDescriptorFieldNumber::ParentUuid as u32, parent),
            TrackEventProtoField::Cstr(TrackDescriptorFieldNumber::Name as u32, name),
            TrackEventProtoField::Bytes(TrackDescriptorFieldNumber::Counter as u32, &[]),
        ],
    });
    ctx.set_counter(match value() {
        CounterValue::Int(v) => TrackEventCounter::Int64(v),
        CounterValue::Double(v) => TrackEventCounter::Double(v),
    });
    sdk_categories::emit(category, TrackEventType::Counter, &mut ctx);
}
