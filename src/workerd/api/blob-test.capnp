using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "blob-test",
      worker = (
        modules = [
          (name = "worker", esModule = embed "blob-test.js")
        ],
        compatibilityDate = "2023-01-15",
      )
    ),
  ],
);
