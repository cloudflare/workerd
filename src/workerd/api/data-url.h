#pragma once

#include <workerd/jsg/url.h>
#include <workerd/util/mimetype.h>

#include <kj/array.h>

namespace workerd::api {

class DataUrl final {
 public:
  static kj::Maybe<DataUrl> tryParse(kj::StringPtr url);
  static kj::Maybe<DataUrl> from(const jsg::Url& url);

  DataUrl(DataUrl&&) = default;
  DataUrl& operator=(DataUrl&&) = default;
  KJ_DISALLOW_COPY(DataUrl);

  const MimeType& getMimeType() const {
    return mimeType;
  }
  kj::ArrayPtr<const kj::byte> getData() const {
    return data.asPtr();
  }

  kj::Array<kj::byte> releaseData() {
    return data.releaseAsBytes();
  }

 private:
  DataUrl(MimeType mimeType, kj::Array<kj::byte> data)
      : mimeType(kj::mv(mimeType)),
        data(kj::mv(data)) {}

  MimeType mimeType;
  kj::Array<kj::byte> data;
};

}  // namespace workerd::api
