// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/compat/http.h>

namespace workerd {

// A header set as some edits would leave it, copying the original only if an edit changes it.
//
// Forwarding a request or response with a header or two set or removed is common, and a copy of a
// kj::HttpHeaders costs a slot per ID of its table, set or not. Often the edit changes nothing:
// the header to remove isn't there, or already has the value to set. A patch then forwards the
// original itself.
//
//     HttpHeadersPatch patch(headers);
//     patch.unset(internalHeaderId);
//     return inner.send(status, statusText, patch.get(), size);
//
// The original must outlive the patch, as must the values passed to setPtr(), exactly as for a
// kj::HttpHeaders::cloneShallow() edited with setPtr(). get() stays valid until the next edit or
// until the patch is destroyed.
class HttpHeadersPatch {
 public:
  explicit HttpHeadersPatch(const kj::HttpHeaders& original): original(original) {}
  KJ_DISALLOW_COPY_AND_MOVE(HttpHeadersPatch);

  // The headers with the edits so far applied.
  const kj::HttpHeaders& get() const {
    KJ_IF_SOME(c, copy) {
      return c;
    }
    return original;
  }

  // Whether an edit has changed the headers, so get() is a copy rather than the original.
  bool isCopy() const {
    return copy != kj::none;
  }

  void unset(kj::HttpHeaderId id) {
    if (get().get(id) != kj::none) {
      edit().unset(id);
    }
  }

  // Like kj::HttpHeaders::setPtr(), including that an empty value removes the header.
  void setPtr(kj::HttpHeaderId id, kj::StringPtr value) {
    KJ_IF_SOME(current, get().get(id)) {
      if (current == value) return;
    } else if (value == nullptr) {
      return;
    }
    edit().setPtr(id, value);
  }
  void setPtr(kj::HttpHeaderId id, kj::String&& value) = delete;

  // Like kj::HttpHeaders::set(id, kj::String&&): the copy owns `value`.
  void set(kj::HttpHeaderId id, kj::String&& value) {
    KJ_IF_SOME(current, get().get(id)) {
      if (current == value) return;
    } else if (value == nullptr) {
      return;
    }
    edit().set(id, kj::mv(value));
  }

  // The copy, made now if no edit has made it yet, for edits a patch doesn't offer.
  kj::HttpHeaders& edit() {
    KJ_IF_SOME(c, copy) {
      return c;
    }
    return copy.emplace(original.cloneShallow());
  }

 private:
  const kj::HttpHeaders& original;
  kj::Maybe<kj::HttpHeaders> copy;
};

}  // namespace workerd
