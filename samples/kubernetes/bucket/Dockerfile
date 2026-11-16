# A stand-in for S3 to try the sample without a cloud bucket: the workerd base image serving
# fake-s3.js, which accepts any credentials. Not for production use.
#
#     docker build -t workerd-sample-bucket:v1 samples/kubernetes/bucket

ARG WORKERD_IMAGE=workerd-base
FROM ${WORKERD_IMAGE}
COPY wrangler.json fake-s3.js /app/
