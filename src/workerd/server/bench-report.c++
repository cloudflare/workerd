// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench-report.h"

#include <v8-initialization.h>

#include <kj/debug.h>
#include <kj/io.h>
#include <kj/vector.h>

#include <cmath>
#include <cstdio>

#if __linux__
#include <fcntl.h>
#endif

namespace workerd::server {

namespace {

using Case = bench::BenchReport::Case;

#if __linux__
// The contents of a file such as one under /proc, which reports a size of 0, or none if it can't
// be read.
kj::Maybe<kj::String> readProcFile(kj::StringPtr path) {
  int fd = open(path.cStr(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return kj::none;
  }
  kj::AutoCloseFd ownFd(fd);
  kj::Maybe<kj::String> result;
  KJ_IF_SOME(exception,
      kj::runCatchingExceptions([&]() { result = kj::FdInputStream(ownFd.get()).readAllText(); })) {
    (void)exception;
    return kj::none;
  }
  return kj::mv(result);
}

kj::ArrayPtr<const char> trim(kj::ArrayPtr<const char> text) {
  size_t begin = 0, end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n')) {
    --end;
  }
  return text.slice(begin, end);
}

kj::Maybe<kj::String> cpuModel() {
  KJ_IF_SOME(cpuinfo, readProcFile("/proc/cpuinfo")) {
    kj::ArrayPtr<const char> rest = cpuinfo;
    while (rest.size() > 0) {
      auto line = rest;
      KJ_IF_SOME(newline, rest.findFirst('\n')) {
        line = rest.first(newline);
        rest = rest.slice(newline + 1);
      } else {
        rest = nullptr;
      }
      KJ_IF_SOME(colon, line.findFirst(':')) {
        if (trim(line.first(colon)) == "model name"_kjb.asChars()) {
          return kj::str(trim(line.slice(colon + 1)));
        }
      }
    }
  }
  return kj::none;
}

kj::Maybe<kj::String> governor() {
  KJ_IF_SOME(text, readProcFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")) {
    return kj::str(trim(text));
  }
  return kj::none;
}
#else
kj::Maybe<kj::String> cpuModel() {
  return kj::none;
}
kj::Maybe<kj::String> governor() {
  return kj::none;
}
#endif

#ifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define WORKERD_BENCH_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define WORKERD_BENCH_SANITIZER 1
#endif

kj::StringPtr buildMode() {
#if WORKERD_BENCH_SANITIZER
  return "sanitizer"_kj;
#elifdef NDEBUG
  return "release"_kj;
#else
  return "debug"_kj;
#endif
}

// The first line of `text`.
kj::ArrayPtr<const char> firstLine(kj::StringPtr text) {
  KJ_IF_SOME(newline, text.findFirst('\n')) {
    return text.first(newline);
  }
  return text.asArray();
}

void appendPadded(kj::Vector<char>& out, kj::StringPtr text, size_t width) {
  out.addAll(text);
  for (size_t i = text.size(); i < width; ++i) {
    out.add(' ');
  }
}

void appendLine(kj::Vector<char>& out, kj::StringPtr text) {
  out.addAll(text);
  out.add('\n');
}

}  // namespace

void fillBenchEnvironment(bench::BenchReport::Environment::Builder environment) {
  kj::Vector<kj::String> warnings;

  auto mode = buildMode();
  environment.setBuildMode(mode);
  if (mode != "release"_kj) {
    warnings.add(kj::str("This is a ", mode,
        " build; its performance differs from a release build's, so use results only to compare "
        "runs of this build."));
  }

  environment.setV8Version(v8::V8::GetVersion());
  KJ_IF_SOME(model, cpuModel()) {
    environment.setCpuModel(model);
  }
  KJ_IF_SOME(g, governor()) {
    environment.setGovernor(g);
    if (g != "performance"_kj) {
      warnings.add(kj::str("The CPU frequency governor is \"", g,
          "\"; set it to \"performance\" for more stable results."));
    }
  }

  auto list = environment.initWarnings(warnings.size());
  for (auto i: kj::indices(warnings)) {
    list.set(i, warnings[i]);
  }
}

kj::String formatBenchDuration(double ns) {
  if (!std::isfinite(ns)) {
    return kj::str(ns);
  }
  static constexpr struct {
    double scale;
    const char* unit;
  } UNITS[] = {{1, "ns"}, {1e3, "us"}, {1e6, "ms"}, {1e9, "s"}};
  size_t i = 0;
  while (i + 1 < kj::size(UNITS) && std::abs(ns) >= UNITS[i + 1].scale) {
    ++i;
  }
  double value = ns / UNITS[i].scale;
  // Three significant figures: 1.23, 12.3, 123.
  int decimals = std::abs(value) < 10 ? 2 : std::abs(value) < 100 ? 1 : 0;
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%.*f %s", decimals, value, UNITS[i].unit);
  return kj::str(buffer);
}

kj::String formatBenchReport(bench::BenchReport::Reader report) {
  kj::Vector<char> out;
  auto environment = report.getEnvironment();

  appendLine(out,
      "Results compare runs on this machine and build; they don't predict production "
      "latency."_kj);
  {
    kj::Vector<kj::String> parts;
    if (environment.hasWorkerdVersion()) {
      parts.add(kj::str("workerd ", environment.getWorkerdVersion()));
    }
    parts.add(kj::str(environment.getBuildMode(), " build"));
    parts.add(kj::str("V8 ", environment.getV8Version()));
    if (environment.hasCpuModel()) {
      parts.add(kj::str(environment.getCpuModel()));
    }
    if (environment.hasGovernor()) {
      parts.add(kj::str("governor ", environment.getGovernor()));
    }
    if (environment.hasCompatDate()) {
      parts.add(kj::str("compat date ", environment.getCompatDate()));
    }
    if (environment.getAllAutogates()) {
      parts.add(kj::str("all autogates"));
    }
    appendLine(out, kj::strArray(parts, ", "));
  }
  for (auto warning: environment.getWarnings()) {
    appendLine(out, kj::str("WARNING: ", warning));
  }

  static constexpr kj::StringPtr HEADERS[] = {
    "case"_kj, "median"_kj, "95% CI"_kj, "mean"_kj, "cpu median"_kj, "samples"_kj};
  constexpr size_t COLUMNS = kj::size(HEADERS);

  for (auto group: report.getGroups()) {
    out.add('\n');
    appendLine(out, group.getName());
    if (group.hasError()) {
      appendLine(out, kj::str("  FAILED: ", firstLine(group.getError())));
    }

    // Each row has either all columns, or a name and a message.
    struct Row {
      kj::Array<kj::String> cells;
      kj::Maybe<kj::String> message;
    };
    kj::Vector<Row> rows;
    for (auto c: group.getCases()) {
      auto name = kj::str(c.getName());
      switch (c.getStatus()) {
        case Case::Status::OK: {
          auto wall = c.getWallNs();
          auto cells = kj::heapArrayBuilder<kj::String>(COLUMNS);
          cells.add(kj::mv(name));
          cells.add(formatBenchDuration(wall.getMedian()));
          cells.add(kj::str(formatBenchDuration(wall.getMedianLow()), " - ",
              formatBenchDuration(wall.getMedianHigh())));
          cells.add(formatBenchDuration(wall.getMean()));
          cells.add(formatBenchDuration(c.getCpuNs().getMedian()));
          cells.add(kj::str(wall.getSamples().size(), " x ", c.getIterationsPerSample()));
          kj::Maybe<kj::String> message;
          for (auto flag: c.getFlags()) {
            switch (flag) {
              case Case::Flag::HIGH_VARIANCE:
                message = kj::str("(high variance: try more samples or a longer minTime)");
                break;
            }
          }
          rows.add(Row{.cells = cells.finish(), .message = kj::mv(message)});
          break;
        }
        case Case::Status::SKIPPED:
          rows.add(Row{.cells = kj::arr(kj::mv(name)), .message = kj::str("skipped")});
          break;
        case Case::Status::FAILED:
          rows.add(Row{.cells = kj::arr(kj::mv(name)),
            .message = kj::str("FAILED: ", firstLine(c.getError()))});
          break;
      }
    }
    if (rows.empty()) {
      continue;
    }

    size_t widths[COLUMNS];
    for (auto i: kj::zeroTo(COLUMNS)) {
      widths[i] = HEADERS[i].size();
    }
    for (auto& row: rows) {
      for (auto i: kj::indices(row.cells)) {
        widths[i] = kj::max(widths[i], row.cells[i].size());
      }
    }

    auto appendRow = [&](kj::ArrayPtr<const kj::StringPtr> cells,
                         kj::Maybe<kj::StringPtr> message) {
      out.addAll("  "_kj);
      for (auto i: kj::indices(cells)) {
        bool last = i + 1 == cells.size() && message == kj::none;
        appendPadded(out, cells[i], last ? 0 : widths[i] + 2);
      }
      KJ_IF_SOME(m, message) {
        out.addAll(m);
      }
      out.add('\n');
    };
    appendRow(HEADERS, kj::none);
    for (auto& row: rows) {
      auto cells = KJ_MAP(cell, row.cells) -> kj::StringPtr { return cell; };
      appendRow(cells, row.message.map([](kj::String& m) -> kj::StringPtr { return m; }));
    }
  }

  out.add('\0');
  return kj::String(out.releaseAsArray());
}

}  // namespace workerd::server
