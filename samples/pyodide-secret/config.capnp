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
    "workerd-autogate-pyodide-load-external",
  ]
);

const mainWorker :Workerd.Worker = (
  modules = [
    (name = "worker.py", pythonModule = embed "./worker.py"),
  ],
  compatibilityDate = "2023-12-18",
  compatibilityFlags = ["python_workers"],
  bindings = [
    (
      name = "secret",
      text = "thisisasecret"
    ),
  ],
  # Learn more about compatibility dates at:
  # https://developers.cloudflare.com/workers/platform/compatibility-dates/
);
