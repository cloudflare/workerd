using Workerd = import "/workerd/workerd.capnp";

const helloWorldExample :Workerd.Config = (
  services = [
    (name = "main", worker = .helloWorld),
    (name = "log", worker = .logWorker),
  ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const helloWorld :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.ts")
  ],
  compatibilityDate = "2025-08-01",
  compatibilityFlags = ["typescript_strip_types", "experimental", "nodejs_compat"],

  durableObjectNamespaces = [
    (className = "MyDurableObject", uniqueKey = "210bd0cbd803ef7883a1ee9d86cce06e"),
  ],

  durableObjectStorage = (inMemory = void),

  bindings = [
    (name = "ns", durableObjectNamespace = "MyDurableObject"),
  ],
  tails = ["log"],

);

const logWorker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "tail.js")
  ],
  compatibilityDate = "2024-10-14",
);
