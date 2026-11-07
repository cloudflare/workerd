# syntax=docker/dockerfile:1
#
# The sample's image: the cluster node and the demo bucket, each a binary that `workerd compile`
# built with its config inside. Build it from the repository root, since the demo bucket embeds
# the fake S3 from workerd's tests:
#
#     docker build -f samples/kubernetes/Dockerfile -t workerd-kubernetes-sample .
#
# workerd comes from npm (`--build-arg WORKERD_VERSION=...` picks the version), or from a Linux
# binary you built for the image's architecture: put it in a directory as `workerd` and add
#
#     --build-arg WORKERD_SOURCE=local --build-context workerd-binary=<directory>
#
# The build stops if that workerd cannot read the cluster key from the environment.

ARG WORKERD_SOURCE=npm

FROM node:24-slim AS workerd-npm
ARG WORKERD_VERSION=latest
RUN npm install --prefix /opt/workerd "workerd@${WORKERD_VERSION}" \
  && cp "$(find /opt/workerd/node_modules/@cloudflare -path '*/bin/workerd' | head -n 1)" \
    /usr/local/bin/workerd

FROM scratch AS workerd-local
COPY --from=workerd-binary workerd /usr/local/bin/workerd

FROM workerd-${WORKERD_SOURCE} AS workerd

FROM node:24-slim AS build
COPY --from=workerd /usr/local/bin/workerd /usr/local/bin/workerd
RUN workerd serve --help | grep -q WORKERD_CLUSTER_KEY_FILE \
  || { echo "This workerd cannot read the cluster key from WORKERD_CLUSTER_KEY_FILE." >&2; exit 1; }
WORKDIR /src
COPY samples/kubernetes/ samples/kubernetes/
COPY src/workerd/server/tests/cluster/fake-s3.js src/workerd/server/tests/cluster/
RUN mkdir /out \
  && workerd compile samples/kubernetes/config.capnp > /out/workerd-sample \
  && workerd compile samples/kubernetes/bucket.capnp > /out/fake-s3 \
  && chmod +x /out/workerd-sample /out/fake-s3

FROM gcr.io/distroless/cc-debian12:nonroot
COPY --from=build /out/ /usr/local/bin/
ENTRYPOINT ["/usr/local/bin/workerd-sample"]
