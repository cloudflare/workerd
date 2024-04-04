# Releasing `workerd` and `workers-types` to NPM

The primary distribution channel for `workerd` right now is through `npm`. We use a (mostly) automatic CI setup to release and publish both `workerd` and `workers-types`.

The release is a 3-step process:
1. Cut release, wait for all binaries to be published (5 platforms total, can take few hours)
2. Publish workerd
3. Publish workers-types

## Versioning

`workerd` and `workers-types` follow the same versioning system. The major version is currently `1` for `workerd` and `4` for `workers-types`. The minor version for both is the compatibility date specified in [supported-compatibility-date.txt](src/workerd/io/supported-compatibility-date.txt) (the latest compatibility date that the `workerd` release will support).

## Cutting Releases

This is pretty simple, and completely automatic—every time the compatibility date in [supported-compatibility-date.txt](src/workerd/io/supported-compatibility-date.txt) changes, a new release is generated, along with the built binaries for `linux-64`, `darwin-64` and `windows-64`. This is governed by the [release.yml](.github/workflows/release.yml) GitHub Action. Binaries for `darwin-arm64` and `linux-arm64` are automatically built with internal runners. It may take a few hours for all binaries to appear on the release.

When there is a build failure with the internal arm64 builders, those releases might not be automatically pushed to the release page. In this case, the binary can be built manually with the [build-releases.sh](build-releases.sh) script and uploaded to the release page. Note that this requires an Apple Silicon Mac. If only the Linux binary is missing, it can be built using [Dockerfile.release](Dockerfile.release) on arm64 Linux using the docker build command used in the build-releases script. Based on these limitations, building binaries manually is discouraged; fixing the CI configuration and restarting the release job should be preferred when possible.

## Publishing `workerd`

Once all binaries (5 platforms) have been automatically added to the release (few hours):

- navigate to "Actions" github tab
- pick "Publish to NPM" workflow
- select the tag created for the release. (Ex v1.20240327.0 where the middle version is the new compat date)
- *keep* patch 0 unless there was a problem with release and you need to publish a patch version (increment in that case).
- if this is a pre-release, check "Is Prerelease" box. If it _is_ a prerelease, the published NPM version will be tagged with `beta`, and have a version number starting with `0.`. Otherwise, the published NPM version will be tagged `latest`, and have a version number starting with `1.`.

## Publishing `workers-types`

Once all binaries (5 platforms) have been automatically added to the release (few hours):

- navigate to "Actions" github tab
- pick "Publish types to NPM" workflow
- select the tag created for the release. (Ex v1.20240327.0 where the middle version is the new compat date)
- *keep* patch 0 unless there was a problem with release and you need to publish a patch version (increment in that case).
- if this is a pre-release, check "Is Prerelease" box. If it _is_ a prerelease, the published NPM version will be tagged with `beta`, and have a version number starting with `0.`.  Otherwise, the published NPM version will be tagged `latest`, and have a version number starting with `4.`.
