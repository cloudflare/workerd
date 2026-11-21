// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "test-session.h"

#include <workerd/util/use-perfetto-categories.h>

#ifdef WORKERD_USE_PERFETTO
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/map.h>
#include <kj/string.h>
#endif

#include <kj/memory.h>

namespace workerd::perfetto_test {

#ifdef WORKERD_USE_PERFETTO

namespace {

kj::Maybe<kj::Own<PerfettoSession>> session;

kj::String toKj(::rust::Str str) {
  return kj::str(kj::ArrayPtr<const char>(str.data(), str.size()));
}

kj::Array<kj::byte> readFile(kj::StringPtr path) {
  auto fs = kj::newDiskFilesystem();
  auto file = fs->getRoot().openFile(fs->getCurrentPath().evalNative(path));
  return file->readAllBytes();
}

}  // namespace

bool compiled_in() {
  return true;
}

void flush_thread() {
  ::workerd::traces::TrackEvent::Flush();
}

void start_session(::rust::Str path) {
  KJ_REQUIRE(session == kj::none, "a session is already running");
  session = kj::heap<PerfettoSession>(toKj(path), "workerd");
}

::rust::Vec<::rust::String> stop_session(::rust::Str path) {
  // A flush from the service does not reach the events a thread has buffered.
  flush_thread();
  // The destructor flushes and stops the session.
  session = kj::none;

  namespace pbzero = ::perfetto::protos::pbzero;
  auto bytes = readFile(toKj(path));

  // Event names are interned per packet sequence, keyed here by the sequence in the high half and
  // the iid in the low half.
  kj::HashMap<uint64_t, kj::String> names;
  auto key = [](uint32_t sequence, uint64_t iid) {
    KJ_ASSERT(iid >> 32 == 0);
    return (static_cast<uint64_t>(sequence) << 32) | iid;
  };
  auto toKjString = [](::protozero::ConstChars chars) {
    return kj::str(kj::ArrayPtr<const char>(chars.data, chars.size));
  };
  kj::Vector<uint64_t> tracks;
  ::rust::Vec<::rust::String> events;

  pbzero::Trace::Decoder trace(bytes.begin(), bytes.size());
  for (auto it = trace.packet(); it; ++it) {
    pbzero::TracePacket::Decoder packet(*it);
    uint32_t sequence = packet.trusted_packet_sequence_id();
    if (packet.has_interned_data()) {
      pbzero::InternedData::Decoder interned(packet.interned_data());
      for (auto name = interned.event_names(); name; ++name) {
        pbzero::EventName::Decoder decoded(*name);
        names.upsert(key(sequence, decoded.iid()), toKjString(decoded.name()),
            [](kj::String& existing, kj::String&& replacement) { existing = kj::mv(replacement); });
      }
    }
    if (!packet.has_track_event()) continue;

    pbzero::TrackEvent::Decoder event(packet.track_event());
    kj::String track = kj::str("thread");
    if (event.has_track_uuid()) {
      uint64_t uuid = event.track_uuid();
      size_t index = 0;
      while (index < tracks.size() && tracks[index] != uuid) ++index;
      if (index == tracks.size()) tracks.add(uuid);
      track = kj::str("t", index + 1);
    }

    switch (event.type()) {
      case pbzero::TrackEvent::TYPE_SLICE_BEGIN: {
        kj::String name;
        if (event.has_name()) {
          name = toKjString(event.name());
        } else {
          KJ_IF_SOME(interned, names.find(key(sequence, event.name_iid()))) {
            name = kj::str(interned);
          } else {
            name = kj::str("<unknown iid>");
          }
        }
        events.push_back(::rust::String(kj::str("B ", track, " ", name).cStr()));
        break;
      }
      case pbzero::TrackEvent::TYPE_SLICE_END:
        events.push_back(::rust::String(kj::str("E ", track).cStr()));
        break;
      default:
        break;
    }
  }
  return events;
}

#else  // defined(WORKERD_USE_PERFETTO)

bool compiled_in() {
  return false;
}

void flush_thread() {}

void start_session(::rust::Str path) {}

::rust::Vec<::rust::String> stop_session(::rust::Str path) {
  return {};
}

#endif  // defined(WORKERD_USE_PERFETTO)

}  // namespace workerd::perfetto_test
