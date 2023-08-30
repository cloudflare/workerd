#include <workerd/tests/bench-tools.h>
#include <workerd/util/mimetype.h>

namespace workerd {
namespace {

WD_BENCH("Mimetype::ParseAndSerialize") {
  MimeType::parse("text/plain;charset=UTF-8"_kj).toString();
  MimeType::parse("multipart/byteranges; boundary=3d6b6a416f9b5"_kj).toString();
  MimeType::parse("video/webm;codecs=\"vp09.02.10.10.01.09.16.09.01,opus\""_kj).toString();

  // longest entry from https://www.iana.org/assignments/media-types/media-types.xhtml
  MimeType::parse("application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheDefinition+xml"_kj).toString();
}

WD_BENCH("Mimetype::Serialize") {
  MimeType::PLAINTEXT.toString();
  MimeType::CSS.toString();
  MimeType::HTML.toString();
  MimeType::JSON.toString();
}

} // namespace

}
