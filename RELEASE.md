# Releasing `workerd` and `workers-types` to NPM

The primary distribution channel for `workerd` right now is through `npm`. We use a (mostly) automatic CI setup to release and publish both `workerd` and `workers-types`.

## Versioning

`workerd` and `workers-types` follow the same versioning system. The major version is currently `1` for `workerd` and `4` for `workers-types`. The minor version for both is the compatibility date specified in [supported-compatibility-date.txt](src/workerd/io/supported-compatibility-date.txt) (the latest compatibility date that the `workerd` release will support).

## Releases

This is pretty simple, and completely automatic—every time the compatibility date in [supported-compatibility-date.txt](src/workerd/io/supported-compatibility-date.txt) changes, a new release is generated, along with the built binaries for `linux-64`, `darwin-64` and `windows-64`. This is governed by the [release.yml](.github/workflows/release.yml) GitHub Action. Binaries for `darwin-arm64` and `linux-arm64` are automatically built with internal runners. It may take a few hours for all binaries to appear on the release.

## Publishing `workerd`

Once all binaries have been automatically added to the release, the "Publish" stage requires a manual run of a GitHub Action—the [Publish to NPM](.github/workflows/npm.yml) action. This action has a `workflow_dispatch` trigger that can be activated from within the GitHub UI, and takes two parameters; the patch version, and whether this release is a prerelease. If it _is_ a prerelease, the published NPM version will be tagged with `beta`, and have a version number starting with `0.`. Otherwise, the published NPM version will be tagged `latest`, and have a version number starting with `1.`.

## Publishing `workers-types`

This also requires a manual run of a GitHub Action—the [Publish types to NPM](.github/workflows/npm-types.yml) action. This action has a `workflow_dispatch` trigger that can be activated from within the GitHub UI, and takes two parameters; the patch version, and whether this is a prerelease. If it _is_ a prerelease, the published NPM version will be tagged with `beta`, and have a version number starting with `0.`. Otherwise, the published NPM version will be tagged `latest`, and have a version number starting with `4.`.
