using Workerd = import "/workerd/workerd.capnp";

# A Worker with a `bucket` service and a `cache` service for `workerd serve --threads`: `front`
# answers on whichever thread takes the request, and reaches the objects of `bucket` through a
# service binding and the entries of `cache` through `caches`. The directories of `storage` and
# `cache` are given on the command line.
const config :Workerd.Config = (
  services = [
    ( name = "front",
      worker = (
        modules = [(name = "worker", esModule = embed "services-worker.mjs")],
        compatibilityDate = "2025-06-01",
        bindings = [
          (name = "BUCKET", service = "bucket"),
        ],
        cacheApiOutbound = "cache",
      )
    ),
    (name = "bucket", bucket = (path = "objects", localDisk = "storage")),
    (name = "storage", disk = (writable = true)),
    (name = "cache", cache = ()),
  ],
  sockets = [
    (name = "http", address = "127.0.0.1:0", http = (), service = "front"),
  ],
);
