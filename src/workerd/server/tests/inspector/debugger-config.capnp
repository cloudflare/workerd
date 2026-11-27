# debugger-config.capnp
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "main", worker = .mainWorker ),
    ( name = "backend", worker = .backendWorker ),
    ( name = "node-inspector", worker = .nodeInspectorWorker ),
  ],
  sockets = [
    ( name = "http", address = "*:0", http = (), service = "main" ),
    ( name = "node", address = "*:0", http = (), service = "node-inspector" ),
  ]
);

const mainWorker :Workerd.Worker = (
  modules = [
    ( name = "./debugger-worker.mjs", esModule = embed "debugger-worker.mjs" ),
    ( name = "./top-level-debugger.mjs", esModule = embed "top-level-debugger.mjs" ),
  ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["nodejs_compat"],
  bindings = [
    ( name = "BACKEND", service = "backend" ),
  ],
);

const backendWorker :Workerd.Worker = (
  modules = [
    ( name = "./backend-worker.mjs", esModule = embed "backend-worker.mjs" ),
  ],
  compatibilityDate = "2024-01-01",
);

const nodeInspectorWorker :Workerd.Worker = (
  modules = [
    ( name = "./node-inspector-worker.mjs", esModule = embed "node-inspector-worker.mjs" ),
  ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = [
    "nodejs_compat",
    "nodejs_compat_v2",
    "enable_nodejs_inspector_module",
    "enable_nodejs_inspector_local_dev",
    "experimental",
  ],
);
