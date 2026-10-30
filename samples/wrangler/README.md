# Wrangler Config Example

workerd can run a project configured with a `wrangler.jsonc` instead of a
`config.capnp`. This one is a TypeScript Worker with a SQLite-backed Durable
Object that counts visits per path.

As Wrangler does, workerd bundles `main` with esbuild, so esbuild must be
installed: in the project (`npm i -D esbuild`), on `PATH`, or named by
`WORKERD_ESBUILD`.

To run the example on http://localhost:8787

```sh
$ ./workerd serve wrangler.jsonc
```

To run using bazel

```sh
$ bazel run //src/workerd/server:workerd -- serve ~/cloudflare/workerd/samples/wrangler/wrangler.jsonc
```

To test:

```sh
$ curl http://localhost:8787/alice
Hello, alice! (visit 1)
$ curl http://localhost:8787/alice
Hello, alice! (visit 2)
```

Counts are stored under `.wrangler/state/workerd/` and survive a restart.
