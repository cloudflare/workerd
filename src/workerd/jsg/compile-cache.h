// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "jsg.h"
#include "setup.h"

#include <v8.h>

#include <kj/string.h>

namespace workerd::jsg {

// The CompileCache is used to hold cached compilation data for built-in JavaScript modules.
//
// Importantly, this is a process-lifetime in-memory cache that is only appropriate for
// built-in modules.
//
// The memory-safety of this cache depends on the assumption that entries are never removed
// or replaced. If things are ever changed such that entries are removed/replaced, then
// we'd likely need to have find return an atomic refcount or something similar.
class CompileCache {
public:
  class Data {
  public:
    Data(): data(nullptr), length(0), owningPtr(nullptr) {};
    explicit Data(std::shared_ptr<v8::ScriptCompiler::CachedData> cached_data)
        : data(cached_data->data),
          length(cached_data->length),
          owningPtr(cached_data) {};

    // Returns a v8::ScriptCompiler::CachedData corresponding to this
    // CompileCache::Data. The lifetime of the returned
    // v8::ScriptCompiler::CachedData must not outlive that of the data.
    std::unique_ptr<v8::ScriptCompiler::CachedData> AsCachedData();

    const uint8_t* data;
    size_t length;

  private:
    std::shared_ptr<void> owningPtr;
  };

  void add(kj::StringPtr key, std::shared_ptr<v8::ScriptCompiler::CachedData> cached) const;
  kj::Maybe<Data&> find(kj::StringPtr key) const;

  static const CompileCache& get() {
    static const CompileCache instance;
    return instance;
  }

private:
  // The key is the address of the static global that was compiled to produce the CachedData.
  kj::MutexGuarded<kj::HashMap<kj::String, Data>> cache;
};

}  // namespace workerd::jsg
