# Copyright (c) 2017-2024 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

using Workerd = import "/workerd/workerd.capnp";

const eventSourceExample :Workerd.Config = (

  services = [
    (name = "main", worker = .eventsource),
    (name = "internet", network = (allow = ["private"]))
  ],

  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const eventsource :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2024-05-31",
);
