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

## Tags

Every release of workerd to npm also publishes the image:

- `<version>`, such as `1.20261116.1`: that release.
- `latest`: the newest release.
- `beta`: the newest prerelease (versions that start with `0.`).

These match the npm dist-tags of the `workerd` package. Each image comes with
an SBOM and a provenance attestation, and is signed with
[cosign](https://docs.sigstore.dev/) keyless signing by the release workflow.
Verify the signature with cosign 3 or later:

```sh
cosign verify docker.io/cloudflare/workerd:<version> \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity https://github.com/cloudflare/workerd/.github/workflows/release.yml@refs/heads/main
```

## Serving a wrangler project

By default the image runs

```sh
workerd serve /app --socket-addr=http=*:8080
```

which serves the wrangler project in `/app` on port 8080, with its
`wrangler.json`, else its `wrangler.jsonc`, as the config. Build an image for
an app from it by copying the project to `/app`:

```dockerfile
FROM docker.io/cloudflare/workerd:<version>
COPY . /app/
```

The image has no esbuild, and workerd runs esbuild for every project that does
not set `"no_bundle": true`, even one whose `main` is already bundled. So the
project must set `"no_bundle": true`, with `main` a `.js` or `.mjs` ES module
built beforehand. workerd adds the modules in `main`'s directory and below it,
which `main` can import.

Arguments to the container are appended to the command line, so they add flags:

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
`runAsUser: 65532` in the workerd container's security context.

A project with state, such as Durable Objects, `kv_namespaces`,
`triggers.crons`, `queues`, `workflows` or `d1_databases`, keeps it under
`.wrangler/state/workerd` next to its config unless `--data-dir` names another
directory. Mount a volume at `/data` and add `--data-dir=/data`:

```sh
docker run --read-only -v my-app-data:/data -p 8080:8080 my-app --data-dir=/data
```

A new named volume such as `my-app-data` starts as a copy of the image's
`/data`, owned by the image's user, and keeps the state across restarts. In
Kubernetes, give the pod `fsGroup: 65532` so that it can write to its volumes.

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

The image has no shell, so `kubectl exec -it <pod> -- sh` fails. To inspect a
running pod, attach a debug container that shares the workerd container's
process namespace:

```sh
kubectl debug -it <pod> --image=busybox --target=<container> --profile=general
```

It sees workerd's processes, with workerd as PID 1. busybox runs as root and
workerd as 65532, so reading workerd's files through `/proc/1/root`, such as
`/proc/1/root/app`, needs `CAP_SYS_PTRACE`, which `--profile=general` adds to
the debug container. A namespace that enforces the `baseline` or `restricted`
Pod Security Standard refuses that capability.

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

## Publishing

Three jobs in `.github/workflows/release.yml` publish the image once a release's
`workerd` npm packages are published. `publish-image` builds the image from the
release's binaries, pushes it by digest only and signs that digest.
`verify-image` pulls the digest on an amd64 and an arm64 runner and runs
`docker/test.sh` on it. `tag-image` then points the tags at the digest. A
digest that fails its tests stays untagged, and no tag moves.

The jobs run only on `main`, and only once the repository has:

- the Actions variable `DOCKER_HUB_USERNAME`, a Docker Hub user that can push
  to `cloudflare/workerd`;
- the Actions secret `DOCKER_HUB_ACCESS_TOKEN`, an access token of that user
  with read and write access.

Until the variable is set, the jobs are skipped. `verify-image` pulls without
logging in, so `cloudflare/workerd` must be public.
