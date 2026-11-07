# A cluster node for Kubernetes. Every pod runs this config, compiled into the image with
# `workerd compile`. Durable Objects run on one pod at a time and move between pods as pods come
# and go. See README.md.

using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .counterWorker),

    # Each pod's own copy of the Durable Objects it runs. The bucket holds the authoritative copy,
    # so this can be an emptyDir volume.
    (name = "do-storage", disk = (path = "/data", writable = true, allowDotfiles = true)),
  ],

  sockets = [ (name = "http", address = "*:8080", http = (), service = "main") ],

  cluster = (
    # Pods reach each other on this port. The pod advertises its own IP with --cluster-advertise.
    address = "0.0.0.0:7000",

    # `bucket` and `key` are left empty, so the image holds neither: each pod reads them from
    # WORKERD_CLUSTER_BUCKET and WORKERD_CLUSTER_KEY_FILE. The bucket's endpoint and credentials
    # come from AWS_ENDPOINT_URL, AWS_REGION, AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY.
  ),
);

const counterWorker :Workerd.Worker = (
  compatibilityDate = "2026-01-01",
  modules = [ (name = "counter.js", esModule = embed "counter.js") ],
  bindings = [
    (name = "COUNTER", durableObjectNamespace = "Counter"),
    (name = "POD_NAME", fromEnvironment = "POD_NAME"),
  ],
  # Keep `uniqueKey` the same in every version of the app: it names the objects' storage.
  durableObjectNamespaces = [ (className = "Counter", uniqueKey = "kubernetes-sample-counter") ],
  durableObjectStorage = (localDisk = "do-storage"),
);
