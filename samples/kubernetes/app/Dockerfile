# The app's image: the workerd base image with the wrangler project copied to /app.
#
#     docker build -t workerd-sample samples/kubernetes/app

ARG WORKERD_IMAGE=workerd-base
FROM ${WORKERD_IMAGE}
COPY . /app/
