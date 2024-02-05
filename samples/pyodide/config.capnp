using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
  ],

  sockets = [
    # Serve HTTP on port 8080.
    ( name = "http",
      address = "*:8080",
      http = (),
      service = "main"
    ),
  ],
);

const mainWorker :Workerd.Worker = (
  modules = [
    (name = "worker.py", pythonModule = embed "./worker.py"),
  ],
  compatibilityDate = "2023-12-18",
  compatibilityFlags = ["experimental"],
  # Learn more about compatibility dates at:
  # https://developers.cloudflare.com/workers/platform/compatibility-dates/
);
