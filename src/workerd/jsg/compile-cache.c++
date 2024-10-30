// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "compile-cache.h"

namespace workerd::jsg {

// CompileCache::Data

kj::Own<v8::ScriptCompiler::CachedData> CompileCache::Data::AsCachedData() {
  return kj::heap<v8::ScriptCompiler::CachedData>(
      data.begin(), data.size(), v8::ScriptCompiler::CachedData::BufferNotOwned)
      .attach(addRefToThis());
}

// CompileCache

void CompileCache::add(kj::StringPtr key, v8::Local<v8::UnboundModuleScript> script) const {
  auto cached = v8::ScriptCompiler::CreateCodeCache(script);
  auto data = kj::heapArray<kj::byte>(cached->data, cached->length);
  cache.lockExclusive()->upsert(kj::str(key), kj::arc<Data>(kj::mv(data)), [](auto&, auto&&) {});
  delete cached;
}

kj::Maybe<kj::Arc<CompileCache::Data>> CompileCache::find(kj::StringPtr key) const {
  KJ_IF_SOME(value, cache.lockExclusive()->find(key)) {
    return value.addRef();
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
    entry.setData(current.value->data);
    i++;
  }
}

void CompileCache::deserialize(capnp::PackedFdMessageReader& message) const {
  auto input = message.getRoot<workerd::tools::CompileCache>();
  auto lock = cache.lockExclusive();
  for (auto entry: input.getEntries()) {
    auto path = entry.getPath();
    auto data = entry.getData();
    auto compiled_cache = kj::heapArray<kj::byte>(data.begin(), data.size());
    lock->insert(kj::heapString(path.cStr(), path.size()), kj::arc<Data>(kj::mv(compiled_cache)));
  }
}

}  // namespace workerd::jsg
