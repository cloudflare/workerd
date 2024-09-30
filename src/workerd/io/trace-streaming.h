#include "trace-common.h"

namespace workerd {

class StreamingTrace final: public kj::Refcounted {
public:
  explicit StreamingTrace(trace::OnsetInfo&& onset = {}) {}
  ~StreamingTrace() noexcept(false) {}
  KJ_DISALLOW_COPY_AND_MOVE(StreamingTrace);
};

}  // namespace workerd
