#include <workerd/tests/bench-tools.h>
#include <workerd/util/mimetype.h>

namespace workerd {
namespace {

static void MimeType_ParseAndSerialize(benchmark::State& state) {
  for (auto _: state) {
    for (size_t i = 0; i < 10000; i++) {
      benchmark::DoNotOptimize(MimeType::parse("text/plain;charset=UTF-8"_kj).toString());
      benchmark::DoNotOptimize(
          MimeType::parse("multipart/byteranges; boundary=3d6b6a416f9b5"_kj).toString());
      benchmark::DoNotOptimize(
          MimeType::parse("video/webm;codecs=\"vp09.02.10.10.01.09.16.09.01,opus\""_kj).toString());

      // longest entry from https://www.iana.org/assignments/media-types/media-types.xhtml
      benchmark::DoNotOptimize(MimeType::parse(
          "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheDefinition+xml"_kj)
                                   .toString());

      benchmark::DoNotOptimize(i);
    }
  }
}

static void MimeType_Serialize(benchmark::State& state) {
  for (auto _: state) {
    for (size_t i = 0; i < 100000; ++i) {
      benchmark::DoNotOptimize(MimeType::PLAINTEXT.toString());
      benchmark::DoNotOptimize(MimeType::CSS.toString());
      benchmark::DoNotOptimize(MimeType::HTML.toString());
      benchmark::DoNotOptimize(MimeType::JSON.toString());

      benchmark::DoNotOptimize(i);
    }
  }
}

WD_BENCHMARK(MimeType_ParseAndSerialize)->Name("Mimetype::ParseAndSerialize");
WD_BENCHMARK(MimeType_Serialize)->Name("Mimetype::Serialize");

}  // namespace

}  // namespace workerd
