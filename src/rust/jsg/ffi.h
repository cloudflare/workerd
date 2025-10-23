#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>

namespace workerd::rust::jsg {
  struct ModuleRegistry {
    virtual ~ModuleRegistry() = default;
    virtual void addBuiltinModule() = 0;
  };

  template <typename Registry>
  struct RustModuleRegistry : public ::workerd::rust::jsg::ModuleRegistry {
    virtual ~RustModuleRegistry() = default;
    RustModuleRegistry(Registry& registry) : registry(registry) {}
    void addBuiltinModule() override {
      KJ_UNREACHABLE;
    }
    Registry& registry;
  };

  inline void register_add_builtin_module(ModuleRegistry& registry) {
    registry.addBuiltinModule();
  }
}
