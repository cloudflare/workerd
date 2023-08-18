using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [ (name = "chat", worker = .mainWorker) ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "chat" ) ],
);

const mainWorker :Workerd.Worker = (
  compatibilityDate = "2023-02-28",

  # This worker is modules-based.
  modules = [
    (name = "chat.js", esModule = embed "main.js"),
  ],

  durableObjectNamespaces = [
    (className = "TestDO", uniqueKey = "ee9d86cce06e210bd0cbd803ef7883a1"),
  ],

  durableObjectStorage = (inMemory = void),

  bindings = [
    (name = "testDO", durableObjectNamespace = "TestDO"),
  ],
);

