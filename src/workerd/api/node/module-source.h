#pragma once

#include <workerd/jsg/util.h>

namespace workerd::api::node {

class ModuleSource {
 public:
  virtual ~ModuleSource() noexcept(false) = default;

  virtual jsg::StaticExternalStringSource get(kj::ArrayPtr<const char> embeddedSource) const = 0;
};

}  // namespace workerd::api::node
