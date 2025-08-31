// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd {

template <typename T>
inline auto mapAddRef(kj::Maybe<kj::Own<T>>& maybe) -> kj::Maybe<kj::Own<T>> {
  return maybe.map([](kj::Own<T>& t) { return kj::addRef(*t); });
}

template <typename T>
inline auto mapAddRef(kj::Maybe<kj::Rc<T>>& maybe) -> kj::Maybe<kj::Rc<T>> {
  return maybe.map([](kj::Rc<T>& t) { return t.addRef(); });
}

template <typename T>
inline auto mapAddRef(const kj::Maybe<kj::Arc<T>>& maybe) -> kj::Maybe<kj::Arc<T>> {
  return maybe.map([](const kj::Arc<T>& t) { return t.addRef(); });
}

template <typename T>
inline auto mapAddRef(kj::Maybe<T&> maybe) -> kj::Maybe<kj::Own<T>> {
  return maybe.map([](T& t) { return kj::addRef(t); });
}

template <typename T>
inline auto mapAddRef(kj::ArrayPtr<kj::Own<T>>& array) -> kj::Array<kj::Own<T>> {
  return KJ_MAP(t, array) { return kj::addRef(*t); };
}

template <typename T>
inline auto mapAddRef(kj::Array<kj::Own<T>>& array) -> kj::Array<kj::Own<T>> {
  return KJ_MAP(t, array) { return kj::addRef(*t); };
}

inline auto mapCopyString(const kj::Maybe<kj::String>& string) -> kj::Maybe<kj::String> {
  return string.map([](const kj::String& s) { return kj::str(s); });
}

inline auto mapCopyString(const kj::Maybe<kj::StringPtr>& string) -> kj::Maybe<kj::String> {
  return string.map([](const kj::StringPtr& s) { return kj::str(s); });
}

}  // namespace workerd
