@0xc8cbb234694939d5;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::jsg");

struct Bundle {
  # Group of modules to be loaded together.
  # Bundles are currently generated during compilation process and linked with the workerd,
  # but loading bundles from somewhere else will also be possible.
  modules @0 :List(Module);
}

struct Module {
  # Javascript module with its source code.

  name @0 :Text;
  src @1 :Data;
  tsDeclaration @3 :Text;

  type @2 :ModuleType;
}


enum ModuleType {
  bundle @0;
  # Provided by the worker bundle.

  builtin @1;
  # Provided by the runtime and can be imported by the worker bundle.
  # Can be overridden by modules in the worker bundle.

  internal @2;
  # Provided by runtime but can only imported by builtin modules.
}
