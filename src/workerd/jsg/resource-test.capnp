@0x92df13ba26adb5fe;

using Modules = import "/workerd/jsg/modules.capnp";

const bootstrapBundle :Modules.Bundle = (
  modules = [
    (name = "bootstrap:main", src = embed "resource-test-bootstrap.js", internal = true)
]);
