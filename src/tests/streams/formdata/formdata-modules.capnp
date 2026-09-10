using Workerd = import "/workerd/workerd.capnp";

# The single source of truth for the formdata suite's test modules.

const modules :List(Workerd.Worker.Module) = [
  (name = "main", esModule = embed "main.js"),
  (name = "multipart-streams", esModule = embed "multipart-streams.js"),
];
