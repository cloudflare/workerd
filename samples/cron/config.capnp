using Workerd = import "/workerd/workerd.capnp";

# "main" is an ordinary Worker with a scheduled() handler. "cron" runs it on a
# schedule: the bundled module /workerd/cron.js keeps one Durable Object per
# schedule, whose alarm calls main's scheduled() at each occurrence. See the
# header of src/workerd/server/cron/cron.js for the config it takes.
const config :Workerd.Config = (
  services = [
    ( name = "main",
      worker = (
        modules = [ ( name = "worker.js", esModule = embed "worker.js" ) ],
        compatibilityDate = "2026-01-01",
      )
    ),
    ( name = "cron",
      worker = (
        modules = [ ( name = "cron.js", bundled = "/workerd/cron.js" ) ],
        compatibilityDate = "2026-01-01",
        compatibilityFlags = ["service_binding_extra_handlers"],
        bindings = [
          ( name = "SCHEDULE", durableObjectNamespace = "Schedule" ),
          # Each key is a service binding below, run by its expressions (UTC).
          ( name = "CRONS", json = "{\"main\": [\"* * * * *\", \"*/5 * * * *\"]}" ),
          ( name = "main", service = "main" ),
        ],
        durableObjectNamespaces = [
          ( className = "Schedule", uniqueKey = "Schedule", enableSql = true ),
        ],
        durableObjectStorage = ( localDisk = "cron-storage" ),
      )
    ),
    # Where each schedule keeps the retries it owes; it must exist.
    ( name = "cron-storage", disk = ( path = "cron-storage", writable = true ) ),
  ],
);
