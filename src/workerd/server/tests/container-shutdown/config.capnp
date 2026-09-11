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
  containerEngine = (
    localDocker = (
      socketPath = "unix:/var/run/docker.sock",
      containerEgressInterceptorImage = "cloudflare/proxy-everything:main",
    ),
  ),
  durableObjectNamespaces = [
    (
      className = "ContainerActor",
      uniqueKey = "container-shutdown-test",
      container = (imageName = "cloudflare/workerd/container-client-test"),
    ),
  ],
  durableObjectStorage = (inMemory = void),
  bindings = [
    (name = "CONTAINER", durableObjectNamespace = "ContainerActor"),
  ],
);
