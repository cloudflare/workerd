@0xfe92df13ba26adb5;

using Modules = import "/workerd/jsg/modules.capnp";

const testBundle :Modules.Bundle = (
  modules = [
    (name = "testBundle:internal", src = "export const foo = 'foo';", type = internal, tsDeclaration="foo: string")
]);
