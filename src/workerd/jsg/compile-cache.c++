// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "compile-cache.h"

#include <workerd/tools/compile-cache.capnp.h>

namespace workerd::jsg {

// CompileCache::Data

std::unique_ptr<v8::ScriptCompiler::CachedData> CompileCache::Data::AsCachedData() {
  return std::make_unique<v8::ScriptCompiler::CachedData>(
      data, length, v8::ScriptCompiler::CachedData::BufferNotOwned);
}

// CompileCache

void CompileCache::add(kj::StringPtr key, v8::Local<v8::UnboundModuleScript> script) const {
  auto cached =
      std::shared_ptr<v8::ScriptCompiler::CachedData>(v8::ScriptCompiler::CreateCodeCache(script));
  cache.lockExclusive()->upsert(kj::str(key), Data(kj::mv(cached)), [](auto&, auto&&) {});
}

kj::Maybe<CompileCache::Data&> CompileCache::find(kj::StringPtr key) const {
  KJ_IF_SOME(value, cache.lockExclusive()->find(key)) {
    if (value.data != nullptr) {
      return value;
    }
  }
  return kj::none;
}

void CompileCache::serialize(capnp::MessageBuilder& message) const {
  auto builder = message.initRoot<workerd::tools::CompileCache>();
  auto lock = cache.lockShared();
  auto entries = builder.initEntries(lock->size());

  size_t i = 0;
  for (auto& current: *lock) {
    auto entry = entries[i];
    entry.setPath(current.key);
    entry.setData(kj::ArrayPtr(current.value.data, current.value.length));
    i++;
  }
}

}  // namespace workerd::jsg
