@0xd36f904ea8f67738;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::tools");

struct CompileCache {
  entries @0 :List(CompileCacheEntry);

  struct CompileCacheEntry {
    path @0 :Text;
    data @1 :Data;
  }
}
