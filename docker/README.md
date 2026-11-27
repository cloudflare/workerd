# workerd container image

`docker.io/cloudflare/workerd` is a container image of workerd on the distroless
base image `gcr.io/distroless/cc-debian13:nonroot`. The image contains the
following:

- The workerd binary, at `/usr/local/bin/workerd`
- An empty `/data` directory that the image's user owns
- The C and C++ runtime libraries, CA certificates, and time zone data of the
  base image

The image has no shell and no package manager. It runs as the non-root user
`nonroot`, with UID and GID 65532, and it works on a read-only root file system.
The image is available for `linux/amd64` and `linux/arm64`.

## Tags

Each release of workerd to npm also publishes the image. The image has the
following tags, which match the npm dist-tags of the `workerd` package:

- <code><var>VERSION</var></code>: a specific release, such as `1.20261124.2`
- `latest`: the newest release
- `beta`: the newest prerelease

Prerelease versions start with `0.`.

## Verify an image

Each image has a software bill of materials (SBOM) and a provenance attestation.
The release workflow signs each image with [cosign](https://docs.sigstore.dev/)
keyless signing. To verify an image's signature, use cosign 3 or later to run
the following command:

```sh
cosign verify docker.io/cloudflare/workerd:VERSION \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity https://github.com/cloudflare/workerd/.github/workflows/release.yml@refs/heads/main
```

Replace <code><var>VERSION</var></code> with the image's version tag.

## Serve a Wrangler project

By default, the image runs the following command, which serves the Wrangler
project in `/app` on port 8080:

```sh
workerd serve /app --socket-addr=http=*:8080
```

The command reads the project's `wrangler.json` file, or its `wrangler.jsonc`
file if the project has no `wrangler.json` file.

To build an image for an app, copy the project to `/app` in a Dockerfile:

```dockerfile
FROM docker.io/cloudflare/workerd:VERSION
COPY . /app/
```

Replace <code><var>VERSION</var></code> with a version tag of the image.

The project must set `"no_bundle": true`, because the image doesn't include
esbuild. Without that setting, workerd runs esbuild for every project, even a
project whose `main` module is already bundled. Build `main` beforehand as a
`.js` or `.mjs` ES module. The `serve` command adds the modules in the directory
of `main` and its subdirectories to the project, so `main` can import them.

The image appends the container's arguments to its command line, so you can use
arguments to add flags. For example, the following command runs an app image
named `my-app` with experimental features enabled:

```sh
docker run -p 8080:8080 my-app --experimental
```

In Kubernetes, `args:` adds flags in the same way. To keep the image's command,
leave `command:` unset.

## Run with a read-only root file system

A project without state doesn't need a writable path. For example, the following
command runs the app with a read-only root file system and no capabilities:

```sh
docker run --read-only --cap-drop ALL --security-opt no-new-privileges -p 8080:8080 my-app
```

In Kubernetes, set `readOnlyRootFilesystem: true`, `runAsNonRoot: true`, and
`runAsUser: 65532` in the security context of the workerd container.

### Keep state on a volume

A project with state keeps that state in `.wrangler/state/workerd` next to its
config file, unless `--data-dir` names a different directory. Durable Objects,
`kv_namespaces`, `triggers.crons`, `queues`, `workflows`, and `d1_databases` all
keep state. To keep the state on a volume, mount the volume at `/data` and add
`--data-dir=/data`:

```sh
docker run --read-only -v my-app-data:/data -p 8080:8080 my-app --data-dir=/data
```

A new named volume, such as `my-app-data`, starts as a copy of the image's
`/data` directory, which the image's user owns. The volume keeps the state
across restarts. In Kubernetes, set `fsGroup: 65532` in the pod's security
context so that workerd can write to the pod's volumes.

## Run a different command

To run workerd with a different command line, replace the image's entrypoint.
With `docker run`, use `--entrypoint`. In Kubernetes, set `command:`, which
replaces the image's entrypoint, and `args:` follows it. For example, the
following container serves a Cap'n Proto config:

```yaml
containers:
  - name: workerd
    image: docker.io/cloudflare/workerd:VERSION
    command: [/usr/local/bin/workerd, serve, /etc/workerd/config.capnp]
```

The following command runs the tests of a config:

```sh
docker run --rm -v "$PWD:/src:ro" --entrypoint /usr/local/bin/workerd \
  docker.io/cloudflare/workerd:VERSION test /src/config.capnp
```

In both examples, replace <code><var>VERSION</var></code> with a version tag of
the image.

### Debug a running pod

The image has no shell, so the command
<code>kubectl exec -it <var>POD_NAME</var> -- sh</code> fails. To inspect a
running pod, attach a debug container that shares the process namespace of the
workerd container:

```sh
kubectl debug -it POD_NAME --image=busybox --target=CONTAINER_NAME --profile=general
```

Replace the following:

- <code><var>POD_NAME</var></code>: the name of the pod
- <code><var>CONTAINER_NAME</var></code>: the name of the workerd container in
  the pod

The debug container sees the processes of the workerd container, with workerd as
PID 1. The debug container runs as root, and workerd runs as UID 65532, so
reading the files of the workerd container through `/proc/1/root`, such as
`/proc/1/root/app`, requires `CAP_SYS_PTRACE`. The `--profile=general` flag adds
this capability to the debug container. A namespace that enforces the `baseline`
or `restricted` Pod Security Standard rejects the capability.

## Build the image

The build context is a directory that contains a Linux workerd binary for each
platform, at <code><var>ARCH</var>/workerd</code>, where <var>ARCH</var> is
`amd64` or `arm64`. Nothing runs during the build, so one host can build both
platforms. To build the image, run the following command from the repository
root:

```sh
docker buildx build --platform linux/amd64,linux/arm64 -f docker/Dockerfile BUILD_CONTEXT
```

Replace <code><var>BUILD_CONTEXT</var></code> with the path of the build context
directory.

To test an image, run the following command:

```sh
docker/test.sh IMAGE PLATFORM
```

Replace the following:

- <code><var>IMAGE</var></code>: the image to test, either a local image or an
  image reference to pull
- <code><var>PLATFORM</var></code>: the platform to test, such as `linux/amd64`

The script checks that the image runs as UID and GID 65532 and runs `workerd
--version`. It checks that the image serves Wrangler projects from `/app` on a
read-only root file system, and that it keeps a Durable Object's state on a
volume with `--data-dir=/data`.

The `.github/workflows/deps-updater.yml` workflow keeps the digest of the base
image up to date.

## How releases publish the image

After the `workerd` npm packages of a release are published, three jobs in
`.github/workflows/release.yml` publish the image:

1. `publish-image` builds the image from the release's binaries, pushes it by
   digest only, and signs the digest.
1. `verify-image` pulls the digest on an amd64 runner and on an arm64 runner,
   and runs `docker/test.sh` on it.
1. `tag-image` points the tags at the digest.

If a digest fails its tests, it stays untagged, and no tag moves.

The jobs run only on `main`, and only if the repository has the following
settings:

- The `DOCKER_HUB_USERNAME` Actions variable: the name of a Docker Hub user that
  can push to `cloudflare/workerd`
- The `DOCKER_HUB_ACCESS_TOKEN` Actions secret: an access token of that user
  with read and write access

Until the variable is set, the release workflow skips the jobs. The
`verify-image` job pulls the image without logging in, so the
`cloudflare/workerd` repository on Docker Hub must be public.
