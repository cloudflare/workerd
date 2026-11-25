@0xc8cbb234694939d5;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::jsg");

const bootstrapFunctionParams :List(Text) = [
  "require", "compatFlags", "autogates", "module", "exports", "primordials", "utils",
];
# The parameters, in order, of the function that each per-isolate bootstrap script
# (src/per_isolate/) is compiled as: the pseudo-globals the scripts reach as variables. The
# loaders (api/per-isolate-bootstrap.c++ and api/per-isolate-bootstrap/lib.rs) compile the
# scripts with them and pass each its value, and gen-compile-cache makes the scripts' compile
# caches with them, which must match for V8 to accept a cache.
#
# They are parameters rather than the properties of a context extension object because an
# extension object makes V8 resolve every free variable in the script (`undefined` included) with
# a dynamic lookup slot: a runtime call that walks the context chain on every access, even in
# optimized code.

struct Bundle {
  # Group of modules to be loaded together.
  # Bundles are currently generated during compilation process and linked with the workerd,
  # but loading bundles from somewhere else will also be possible.
  modules @0 :List(Module);
}

struct Module {
  # Javascript module with its source code.

  name @0 :Text;
  union {
    src @1 :Data; # JS / TS code
    wasm @4 :Data; # Wasm module
    data @5 :Data; # Binary data module
    json @6 :Text; # Json module
  }
  tsDeclaration @3 :Text;

  type @2 :ModuleType;

  # Optional compile cache to be used to speed up module loading
  compileCache @7 :Data;
}


enum ModuleType {
  bundle @0;
  # Provided by the worker bundle. TODO: rename this to e.g., user?

  builtin @1;
  # Provided by the runtime and can be imported by the worker bundle.
  # Can be overridden by modules in the worker bundle.

  internal @2;
  # Provided by runtime but can only imported by builtin modules.
}
