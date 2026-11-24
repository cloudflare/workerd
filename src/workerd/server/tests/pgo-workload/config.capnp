# The Workers that pgo-workload-test.mjs serves. The test replaces COMPAT_DATE with the newest
# compatibility date that workerd supports.
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "hello", worker = .hello ),
    ( name = "hello-async", worker = .helloAsync ),
    ( name = "mixed", worker = .mixed ),
    ( name = "upstream", worker = .upstream ),
  ],
  sockets = [
    ( name = "hello", address = "127.0.0.1:0", http = (), service = "hello" ),
    ( name = "hello-async", address = "127.0.0.1:0", http = (), service = "hello-async" ),
    ( name = "mixed", address = "127.0.0.1:0", http = (), service = "mixed" ),
  ]
);

const hello :Workerd.Worker = (
  modules = [ ( name = "worker", esModule = embed "hello.js" ) ],
  compatibilityDate = "COMPAT_DATE",
);

const helloAsync :Workerd.Worker = (
  modules = [ ( name = "worker", esModule = embed "hello-async.js" ) ],
  compatibilityDate = "COMPAT_DATE",
);

const mixed :Workerd.Worker = (
  modules = [ ( name = "worker", esModule = embed "mixed.js" ) ],
  compatibilityDate = "COMPAT_DATE",
  bindings = [ ( name = "upstream", service = "upstream" ) ],
);

const upstream :Workerd.Worker = (
  modules = [ ( name = "worker", esModule = embed "upstream.js" ) ],
  compatibilityDate = "COMPAT_DATE",
);
