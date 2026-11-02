// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/io.h>
#include <kj/string.h>

#include <atomic>

namespace workerd::jsg {

// Appends a line for each block of V8-generated machine code to a Linux perf map
// (`/tmp/perf-<pid>.map`; see `tools/perf/Documentation/jit-interface.txt` in the Linux tree), so
// that external profilers such as `perf` and the eBPF profiler can name frames in JIT code.
//
// Each line is `<start hex> <size hex> <name>`. The map is only ever appended to: V8 moves and
// frees code, so an address can appear more than once, and the latest line covering it wins.
//
// `add()` may be called from any thread. Each line goes out in a single write() to a descriptor
// opened with O_APPEND, so lines from concurrent callers never interleave.
class PerfMapWriter {
 public:
  explicit PerfMapWriter(kj::OwnFd fd);

  void add(const void* start, size_t size, kj::ArrayPtr<const char> name) const;

 private:
  kj::OwnFd fd;

  // Set after the first failed write (e.g. when the disk fills up), after which the map is left
  // as is rather than retrying on every code event.
  mutable std::atomic<bool> failed = false;
};

}  // namespace workerd::jsg
