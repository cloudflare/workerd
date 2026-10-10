# Serving a Wrangler project directly

This is an ordinary Wrangler project: a `wrangler.jsonc` with a Durable Object class (SQLite
backed), plain `vars`, a service binding to itself, and a KV namespace. It needs no `workerd`
config file:

```
workerd serve samples/wrangler-project/wrangler.jsonc
curl http://localhost:8787/room-a
curl http://localhost:8787/room-a
curl http://localhost:8787/self
```

The counter survives restarts: Durable Object storage is persisted under
`samples/wrangler-project/.workerd-state`. The KV namespace has no standalone implementation, so
`workerd` reports it and leaves that binding out.

To see (and keep) the config `workerd` synthesized:

```
workerd serve samples/wrangler-project/wrangler.jsonc --print-config > config.capnp
```

To validate the project without serving it, which reports every error at once:

```
workerd check samples/wrangler-project/wrangler.jsonc
```
