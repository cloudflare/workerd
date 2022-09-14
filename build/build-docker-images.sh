#!/bin/bash -eu
# Builds .docker-images from multiple .dockerimage fragments
# Expected working directory is repo root

echo "images:" > .docker-images

for f in $(find bazel-bin/ -name '*.dockerimage'); do
  cat $f >> .docker-images
done;
