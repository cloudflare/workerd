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
  autogates = [
    # Pyodide is included as a builtin wasm module so it requires the
    # corresponding autogate flag.
    "workerd-autogate-builtin-wasm-modules",
  ]
);

const mainWorker :Workerd.Worker = (
  modules = [
    (name = "worker", pythonModule = embed "./worker.py"),
    (name = "fastapi", pythonRequirement = "fastapi"),
  ],
  compatibilityDate = "2023-12-18",
  compatibilityFlags = ["experimental"],
  # Learn more about compatibility dates at:
  # https://developers.cloudflare.com/workers/platform/compatibility-dates/
);
