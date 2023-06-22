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

  internal @2 :Bool;
  # internal modules can't be imported by user's code
}
