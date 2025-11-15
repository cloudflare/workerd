
using Workerd = import "/workerd/workerd.capnp";

const reprl :Workerd.Config = (
  services = [ (name = "main", worker = .replServer) ],

  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const replServer :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  bindings = [
    (
      name = "secret",
      text = "thisisasecret"
    ),
    ( name = "CACHE", memoryCache = (
      id = "abc123",
      limits = (
        maxKeys = 10,
        maxValueSize = 1024,
        maxTotalValueSize = 1024,
        ),
      )
    )
  ],
  compatibilityDate = "2023-02-28",
  compatibilityFlags = ["nodejs_compat", "experimental", "unsafe_module"]
);
