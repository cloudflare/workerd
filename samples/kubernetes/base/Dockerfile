# syntax=docker/dockerfile:1
#
# The base image: workerd and nothing else, on a distroless image, running as a non-root user. It
# serves the wrangler project in /app on port 8080. An app's image is built FROM it and copies its
# project to /app (see ../app/Dockerfile). Build it with
#
#     docker build -t workerd-base samples/kubernetes/base
#
# workerd comes from npm (`--build-arg WORKERD_VERSION=...` picks the version), or from a Linux
# binary built for the image's architecture: put it in a directory as `workerd` and add
#
#     --build-arg WORKERD_SOURCE=local --build-context workerd-binary=<directory>
#
# The build stops if that workerd cannot run here or cannot join a cluster with --cluster-addr.

ARG WORKERD_SOURCE=npm

FROM node:24-trixie-slim AS workerd-npm
ARG WORKERD_VERSION=latest
RUN npm install --prefix /opt/workerd "workerd@${WORKERD_VERSION}" \
  && cp "$(find /opt/workerd/node_modules/@cloudflare -path '*/bin/workerd' | head -n 1)" /workerd

FROM scratch AS workerd-local
COPY --from=workerd-binary workerd /workerd

FROM workerd-${WORKERD_SOURCE} AS workerd

# Runs the binary, and makes /data for Durable Objects, which the final image cannot: it has no
# shell. This stage is on Debian 13, as the final image is, so a binary that needs a newer C
# library, or is built for another architecture, fails here rather than in the cluster.
FROM node:24-trixie-slim AS check
COPY --from=workerd --chmod=755 /workerd /out/workerd
RUN /out/workerd --version \
  || { echo "This workerd does not run on Debian 13 on this architecture." >&2; exit 1; }
RUN /out/workerd serve --help | tr -s '[:space:]' ' ' \
    | grep -q 'such as a wrangler config, joins a cluster' \
  || { echo "This workerd runs, but is too old to join a cluster with --cluster-addr." >&2; \
       exit 1; }
RUN mkdir /out/data

FROM gcr.io/distroless/cc-debian13:nonroot
COPY --from=check /out/workerd /usr/local/bin/workerd
COPY --from=check --chown=nonroot:nonroot /out/data /data
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/workerd", "serve", "/app/wrangler.json", "--socket-addr=http=*:8080"]
