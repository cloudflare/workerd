#include "tracing-public.h"
#include <workerd/util/tracing.h>

namespace workerd {

void registerPerfettoTrackEvents() {
  if (::perfetto::Tracing::IsInitialized()) {
    workerd::TrackEvent::Register();
  }
}

}  // namespace workerd
