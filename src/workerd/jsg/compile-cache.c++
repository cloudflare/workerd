// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "compile-cache.h"

namespace workerd::jsg {

// CompileCache::Data

std::unique_ptr<v8::ScriptCompiler::CachedData> CompileCache::Data::AsCachedData() {
  return std::make_unique<v8::ScriptCompiler::CachedData>(
      data, length, v8::ScriptCompiler::CachedData::BufferNotOwned);
}

// CompileCache

void CompileCache::add(
    kj::StringPtr key, std::shared_ptr<v8::ScriptCompiler::CachedData> cached) const {
  cache.lockExclusive()->upsert(kj::str(key), Data(kj::mv(cached)), [](auto&, auto&&) {});
}

kj::Maybe<CompileCache::Data&> CompileCache::find(kj::StringPtr key) const {
  auto lock = cache.lockExclusive();
  KJ_IF_SOME(value, lock->find(key)) {
    if (value.data != nullptr) {
      return value;
    }
  }
  return kj::none;
}

}  // namespace workerd::jsg
