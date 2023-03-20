# Builtin Modules

This directory contains comprehensive sample of using builtin javascript modules.
Builtin modules extend workerd functionality for all workers hosted by a given server.

This example demonstrates following features:
- using builtins to provide new user-level api
- using internal builtin modules to hide implementation details from the user

The sample will be extended as more functionality is implemented.

## Demo bundle

The bundle is defined in [alibaba-bundle.capnp](alibaba-bundle.capnp) and specified modules
accessible to the user and internal modules, accessible only to builtins.

The bundle is loaded in server config and is available through normal ESM imports in
[worker.js](worker.js).
