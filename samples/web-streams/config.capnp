# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

using Workerd = import "/workerd/workerd.capnp";

const webStreamsExample :Workerd.Config = (
  services = [ (name = "main", worker = .webStreams) ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const webStreams :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js"),
    (name = "streams-util", esModule = embed "streams-util.js")
  ],
  compatibilityDate = "2025-12-31",
);
