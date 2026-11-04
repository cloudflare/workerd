# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# A `workerd run --config` template: one Worker with no modules, which the code becomes.

using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [(
    name = "app",
    worker = (
      compatibilityFlags = ["nodejs_compat"],
      bindings = [(name = "GREETING", text = "hi from the template")],
    ),
  )],
);
