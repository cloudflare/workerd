# The app's image: the workerd image with the wrangler project copied to /app, which the workerd
# image serves on port 8080 (see docker/README.md).
#
#     docker build -t workerd-sample:v1 samples/kubernetes/app
#
# `--build-arg WORKERD_IMAGE=<image>` builds on another workerd image, such as one built from
# source. To run anything but the sample, pin a release's version tag rather than `latest`.

ARG WORKERD_IMAGE=docker.io/cloudflare/workerd:latest
FROM ${WORKERD_IMAGE}
COPY . /app/
