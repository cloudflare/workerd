using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .worker),
  ],
  sockets = [
    (name = "http", address = "*:0", http = (), service = "main"),
  ],
);

const worker :Workerd.Worker = (
  modules = [
    (name = "worker.mjs", esModule = embed "worker.mjs"),
  ],
  compatibilityDate = "2025-01-01",
  compatibilityFlags = ["experimental"],
  containerEngine = (
    localDocker = (
      socketPath = "unix:/var/run/docker.sock",
      containerEgressInterceptorImage = "cloudflare/proxy-everything:main",
      sessionId = "container-session-test",
    ),
  ),
  durableObjectNamespaces = [
    (
      className = "ContainerActor",
      uniqueKey = "container-session-actor",
      container = (imageName = "cloudflare/workerd/container-client-test"),
    ),
  ],
  durableObjectStorage = (inMemory = void),
  bindings = [
    (name = "CONTAINER", durableObjectNamespace = "ContainerActor"),
  ],
);
