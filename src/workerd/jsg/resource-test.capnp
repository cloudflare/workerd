@0x92df13ba26adb5fe;

using Modules = import "/workerd/jsg/modules.capnp";

const builtinBundle :Modules.Bundle = (
  modules = [
    (name = "test:resource-test-builtin", src = embed "resource-test-builtin.js", type = builtin)
]);


const bootstrapBundle :Modules.Bundle = (
  modules = [
    (name = "test:resource-test-bootstrap", src = embed "resource-test-bootstrap.js", type = internal)
]);
