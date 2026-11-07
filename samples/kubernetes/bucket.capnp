# A stand-in for S3 to try the sample without a cloud bucket: the fake S3 that workerd's own cluster
# tests use, which accepts any credentials. Not for production use. See README.md.

using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "s3",
      worker = (
        compatibilityDate = "2026-01-01",
        modules = [
          ( name = "fake-s3.js",
            esModule = embed "../../src/workerd/server/tests/cluster/fake-s3.js" ),
        ],
        bindings = [ (name = "BUCKET", durableObjectNamespace = "Bucket") ],
        durableObjectNamespaces = [ (className = "Bucket", uniqueKey = "fake-s3-bucket") ],
        durableObjectStorage = (localDisk = "storage"),
      )
    ),
    (name = "storage", disk = (path = "/data", writable = true, allowDotfiles = true)),
  ],

  sockets = [ (name = "http", address = "*:9000", http = (), service = "s3") ],
);
