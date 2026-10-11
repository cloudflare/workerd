# Multiple Codebases Example

This example runs two applications with separate Worker scripts in one workerd
process. Both applications share a listening socket. A router Worker selects an
application by the request URL's hostname and forwards the request through a
service binding. Unrecognized hostnames receive a 404 response.

Use this pattern for codebases you trust, such as multiple applications belonging
to the same organization. Host-based routing does not provide the security
isolation needed to run arbitrary, potentially malicious tenant code.

## Run

From this directory, using a workerd binary:

```sh
workerd serve config.capnp
```

Or from the repository root, using Bazel:

```sh
bazel run //src/workerd/server:workerd -- serve "$PWD/samples/multiple-codebases/config.capnp"
```

## Try Both Applications

The example listens on `127.0.0.1:8080`. Set the `Host` header to select an
application without configuring local DNS:

```sh
curl -H 'Host: one.example.test' 'http://127.0.0.1:8080/hello?name=one'
# App one: http://one.example.test/hello?name=one

curl -H 'Host: two.example.test' 'http://127.0.0.1:8080/hello?name=two'
# App two: http://two.example.test/hello?name=two

curl -i -H 'Host: unknown.example.test' http://127.0.0.1:8080/
# HTTP/1.1 404 Not Found
```

Each application's service can have its own modules, bindings, and compatibility
date. To add an application, configure its service, give the router a binding to
that service, and add its hostname to the router. The original request, including
its path, query string, method, and body, is passed to the selected application.
