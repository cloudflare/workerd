@0x8c73baaf4250210f;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::util::autogate");

struct Autogate {
  enabled @0 :Bool;
  name @1 :Text;
}
