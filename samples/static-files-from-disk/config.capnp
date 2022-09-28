# This is a simple demo showing how to serve a static web site from disk.
#
# By default this will serve a subdirectory called `content-dir` from whatever directory `workerd`
# runs in, but you can override the directory on the command-line like so:
#
#     workerd serve config.capnp --directory-path site-files=/path/to/files

using Workerd = import "/workerd/workerd.capnp";

# A constant of type `Workerd.Config` will be recognized as the top-level configuration.
const config :Workerd.Config = (
  services = [
    # The site worker contains JavaScript logic to serve static files from a directory. The logic
    # includes things like setting the right content-type (based on file name), defaulting to
    # `index.html`, and so on.
    (name = "site-worker", worker = .siteWorker),
    
    # Underneath the site worker we have a second service which provides direct access to files on
    # disk. We only configure site-worker to be able to access this (via a binding, below), so it
    # won't be served publicly as-is. (Note that disk access is read-only by default, but there is
    # a `writable` option which enables PUT requests.)
    (name = "site-files", disk = "content-dir"),
  ],

  # We export it via HTTP on port 8080.
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "site-worker" ) ],
);

# For legibility we define the Worker's config as a separate constant.
const siteWorker :Workerd.Worker = (
  # All Workers must declare a compatibility date, which ensures that if `workerd` is updated to
  # a newer version with breaking changes, it will emulate the API as it existed on this date, so
  # the Worker won't break.
  compatibilityDate = "2022-09-16",

  # This worker is modules-based.
  modules = [
    (name = "static.js", esModule = embed "static.js"),
  ],

  bindings = [
    # Give this worker permission to request files on disk, via the "site-files" service we
    # defined earlier.
    (name = "files", service = "site-files"),

    # This worker supports some configuration options via a JSON binding. Here we set the option
    # so that we hide the `.html` extension from URLs. (See the code for all config options.)
    (name = "config", json = "{\"hideHtmlExtension\": true}")
  ],
);

