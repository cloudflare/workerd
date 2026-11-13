#pragma once

#include <workerd/jsg/util.h>

namespace workerd::jsg {

// Maps the source text of a compiled-in module or script to the storage V8's external string
// should reference. The default (no ModuleSource, a null pointer at every call site) is the
// compiled-in bytes themselves, as a one-byte string without transcoding. An embedder can
// instead keep the sources elsewhere, for example transcoded and in a guarded mapping, so that
// an external string header corrupted inside the V8 sandbox cannot read past them. The storage
// returned must outlive every isolate that compiles from it.
class ModuleSource {
 public:
  virtual ~ModuleSource() noexcept(false) = default;

  // `embeddedSource` is the module's bytes exactly as compiled in (`Module::getSrc()`), which
  // implementations may use as a lookup key.
  virtual StaticExternalStringSource get(kj::ArrayPtr<const char> embeddedSource) const = 0;
};

}  // namespace workerd::jsg
