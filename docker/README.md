# The workerd container image

`docker.io/cloudflare/workerd` is workerd on a distroless base image,
`gcr.io/distroless/cc-debian13:nonroot`. It holds:

- the workerd binary, at `/usr/local/bin/workerd`;
- an empty `/data` directory owned by the image's user;
- the C and C++ runtime libraries, CA certificates and time zone data of the
  base image.

It has no shell and no package manager. It runs as the non-root user `nonroot`
(65532:65532) and works on a read-only root filesystem. It is built for
`linux/amd64` and `linux/arm64`.

## Serving a wrangler project

By default the image runs

```sh
workerd serve /app/wrangler.json --socket-addr=http=*:8080
```

which serves the wrangler project in `/app` on port 8080. Build an image for an
app from it by copying the project to `/app`:

```dockerfile
FROM docker.io/cloudflare/workerd:<version>
COPY . /app/
```

The image has no esbuild, so the project needs `"no_bundle": true` (with `main`
an ES module), or `main` bundled before it is copied. Arguments to the container
are appended to that command line, so they add flags:

```sh
docker run -p 8080:8080 my-app --experimental
```

In Kubernetes, `args:` does the same. Leave `command:` unset to keep the
default command.

## Read-only and non-root

The image needs no writable path to serve a project without state:

```sh
docker run --read-only --cap-drop ALL --security-opt no-new-privileges -p 8080:8080 my-app
```

In Kubernetes, set `readOnlyRootFilesystem: true`, `runAsNonRoot: true` and
`runAsUser: 65532` in the security context. To keep Durable Objects on a
volume, mount it at `/data` and add `--directory-path=<name>-do-storage=/data`,
where `<name>` is the project's `name`:

```sh
docker run --read-only --tmpfs /data:uid=65532,gid=65532 \
  --tmpfs /app/.wrangler:uid=65532,gid=65532 \
  -p 8080:8080 my-app --directory-path=my-app-do-storage=/data
```

At startup, workerd creates directories under `.wrangler/state/workerd` next to
the project's config for each of these that the project has: Durable Objects
(even with `--directory-path`), `kv_namespaces`, `triggers.crons`, `queues`,
`workflows` and `d1_databases`. Such a project needs a writable
`/app/.wrangler`, such as the second tmpfs above or an `emptyDir` volume, and
keeps all but its Durable Objects there. In Kubernetes, give the pod
`fsGroup: 65532` so that it can write to its volumes.

## Running something else

To run workerd with another command line, replace the entrypoint: `--entrypoint`
with `docker run`, or `command:` in Kubernetes, which replaces the image's
entrypoint (`args:` then follows it). For example, to serve a Cap'n Proto
config:

```yaml
containers:
  - name: workerd
    image: docker.io/cloudflare/workerd:<version>
    command: [/usr/local/bin/workerd, serve, /etc/workerd/config.capnp]
```

or to run a config's tests:

```sh
docker run --rm -v "$PWD:/src:ro" --entrypoint /usr/local/bin/workerd \
  docker.io/cloudflare/workerd:<version> test /src/config.capnp
```

## Building the image

The build context is a directory with a Linux workerd binary for each platform,
at `<arch>/workerd`, where `<arch>` is `amd64` or `arm64`. Nothing runs during
the build, so one host builds both platforms. From the repository root:

```sh
docker buildx build --platform linux/amd64,linux/arm64 -f docker/Dockerfile <directory>
```

`docker/test.sh <image> <platform>` checks that an image runs `--version` and
serves a project as described above. `.github/workflows/deps-updater.yml` keeps
the digest of the base image current.
