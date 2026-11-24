# The workerd base image, published as docker.io/cloudflare/workerd: the workerd binary on a
# distroless image, running as a non-root user. It serves the wrangler project in /app on port
# 8080. See README.md.
#
# The build context is a directory with a Linux workerd binary for each platform to build, at
# <arch>/workerd, where <arch> is Docker's name for it (amd64, arm64). Nothing runs at build time,
# so any platform builds on any host:
#
#     docker buildx build --platform linux/amd64,linux/arm64 -f docker/Dockerfile <directory>

# gcr.io/distroless/cc-debian13:nonroot. .github/workflows/deps-updater.yml updates the digest.
# cc rather than base: glibc's backtrace(), which workerd calls for stack traces, loads
# libgcc_s.so.1, which only cc has.
FROM gcr.io/distroless/cc-debian13:nonroot@sha256:cb87acc574abe719b6e5c547384defa2f7b1f24af9605d3f618cf852495d8be2 AS base

FROM base
ARG TARGETARCH
ARG VERSION=
ARG REVISION=
LABEL org.opencontainers.image.title="workerd" \
      org.opencontainers.image.description="A JavaScript / Wasm server runtime based on the same code that powers Cloudflare Workers" \
      org.opencontainers.image.source="https://github.com/cloudflare/workerd" \
      org.opencontainers.image.url="https://github.com/cloudflare/workerd" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.vendor="Cloudflare, Inc." \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}"
COPY --chmod=755 ${TARGETARCH}/workerd /usr/local/bin/workerd
# The image has no shell to make an empty directory with, so /data is a copy of the empty home
# directory of nonroot (65532).
COPY --from=base --chown=65532:65532 /home/nonroot /data
USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/workerd", "serve", "/app", "--socket-addr=http=*:8080"]
