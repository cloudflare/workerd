# config.capnp
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "main", worker = .worker ),
    ( name = "disabled", worker = .disabledWorker ),
  ],
  sockets = .sockets,
);

const tsStreams :Workerd.Config = (
  services = [
    ( name = "main", worker = .tsWorker ),
    ( name = "disabled", worker = .disabledWorker ),
  ],
  sockets = .sockets,
);

const sockets :List(Workerd.Socket) = [
  ( name = "udp", address = "127.0.0.1:0", udp = (idleTimeoutMs = 200), service = "main" ),
  ( name = "udp-bounded", address = "127.0.0.1:0",
    udp = (idleTimeoutMs = 1000, maxPendingBytes = 95),
    service = (name = "main", entrypoint = "BoundedQueue") ),
  ( name = "udp-disabled", address = "127.0.0.1:0", udp = (idleTimeoutMs = 200), service = "disabled" ),
  ( name = "http-disabled", address = "127.0.0.1:0", http = (), service = "disabled" ),
];

const disabledWorker :Workerd.Worker = (
  modules = [
    ( name = "./disabled.mjs", esModule = embed "disabled.mjs" )
  ],
  compatibilityDate = "2024-01-01",
);

const modules :List(Workerd.Worker.Module) = [
  ( name = "./index.mjs", esModule = embed "index.mjs" )
];

const worker :Workerd.Worker = (
  modules = .modules,
  compatibilityDate = "2024-01-01",
  compatibilityFlags = [ "udp_ingress" ],
);

const tsWorker :Workerd.Worker = (
  modules = .modules,
  compatibilityDate = "2024-01-01",
  compatibilityFlags = [ "udp_ingress", "typescript_implemented_streams" ],
);
