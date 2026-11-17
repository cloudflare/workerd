# Cron Triggers Example

A Worker's `scheduled()` handler run on a schedule, here every minute and every
five minutes. Cron triggers are experimental, so `--experimental` is required.

To run the example

```sh
$ mkdir -p cron-storage
$ ./workerd serve --experimental config.capnp
```

To run using bazel, which runs workerd in another directory, so the disk's
path is given in full:

```sh
$ mkdir -p cron-storage
$ bazel run //src/workerd/server:workerd -- serve --experimental \
    -d cron-storage=$PWD/cron-storage $PWD/config.capnp
```

At the top of each minute:

```
* * * * * fired for 2026-11-07T12:01:00.000Z
```

A `wrangler.jsonc` with `"triggers": { "crons": [...] }` gets the same, with
`workerd serve --experimental wrangler.jsonc`.
