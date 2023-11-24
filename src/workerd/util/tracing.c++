#include "tracing.h"
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/memory.h>
#include <kj/vector.h>
#include <fcntl.h>
#include <perfetto/tracing/track_event_legacy.h>
#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/config/track_event/track_event_config.gen.h"

PERFETTO_TRACK_EVENT_STATIC_STORAGE_IN_NAMESPACE(workerd);

namespace workerd {
namespace {
kj::AutoCloseFd openTraceFile() {
  int fd = open("/tmp/PERFETTO_TRACE", O_RDWR | O_CREAT | O_TRUNC, 0600);
  KJ_REQUIRE(fd >= 0, "Unable to open tracing file");
  return kj::AutoCloseFd(fd);
}

bool perfettoInitialized = false;
void initializePerfettoOnce() {
  if (perfettoInitialized) return;
  perfettoInitialized = true;
  perfetto::TracingInitArgs args;
  args.backends |= perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(args);
  workerd::TrackEvent::Register();
}

kj::Array<kj::ArrayPtr<const char>> parseCategories(kj::StringPtr categories) {
  kj::Vector<kj::ArrayPtr<const char>> results;
  while (categories.size() > 0) {
    KJ_IF_SOME(pos, categories.findFirst(',')) {
      results.add(categories.slice(0, pos));
      categories = categories.slice(pos + 1);
    } else {
      results.add(categories);
      categories = nullptr;
    }
  }
  return results.releaseAsArray();
}

std::unique_ptr<perfetto::TracingSession> createTracingSession(int fd, kj::StringPtr categories) {
  initializePerfettoOnce();
  perfetto::protos::gen::TrackEventConfig track_event_cfg;
  track_event_cfg.add_disabled_categories("*");

  // The categories is a comma-separated list
  auto cats = parseCategories(categories);
  for (auto category : cats) {
    auto view = std::string(category.begin(), category.size());
    track_event_cfg.add_enabled_categories(view);
  }

  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(256);  // Record up to 1 MiB.
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_track_event_config_raw(track_event_cfg.SerializeAsString());
  std::unique_ptr<perfetto::TracingSession> tracing_session(
      perfetto::Tracing::NewTrace());
  tracing_session->Setup(cfg, fd);
  return kj::mv(tracing_session);
}
}  // namespace

struct PerfettoSession::Impl {
  kj::AutoCloseFd fd;
  std::unique_ptr<perfetto::TracingSession> session;

  Impl(kj::StringPtr path, kj::StringPtr categories)
      : fd(openTraceFile()),
        session(createTracingSession(fd.get(), categories)) {
    session->StartBlocking();
  }
};

PerfettoSession::PerfettoSession(kj::StringPtr path, kj::StringPtr categories)
    : impl(kj::heap<Impl>(path, categories)) {}

PerfettoSession::~PerfettoSession() noexcept(false) {
  if (impl) {
    impl->session->FlushBlocking();
    impl->session->StopBlocking();
  }
}

void PerfettoSession::flush() {
  if (impl) {
    impl->session->FlushBlocking();
  }
}

}  // namespace workerd
