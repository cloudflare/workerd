# Releasing `workerd` and `workers-types` to NPM

The primary distribution channel for `workerd` right now is through `npm`. We use a (mostly) automatic CI setup to release and publish both `workerd` and `workers-types`.

## Versioning

`workerd` and `workers-types` follow the same versioning system. The major version is currently `1` for `workerd` and `4` for `workers-types`. The minor version for both is the compatibility date specified in [compatibility-date.capnp](src/workerd/io/compatibility-date.capnp) (the latest compatibility date that the `workerd` release will support).


## Releases

This is pretty simple, and completely automatic—every time the compatibility date in [compatibility-date.capnp](src/workerd/io/compatibility-date.capnp) changes, a new release is generated, along with the built binaries for `linux-64` and `darwin-64`. This is governed by the [release.yml](.github/workflows/release.yml) GitHub Action. Since this only generates binaries for `darwin-64` and `linux-64`, binaries for `darwin-arm64` and `linux-arm64` need to be built manually on local machines. These can be generated from a local checkout of `workerd` using the `build-release.sh` script. This must be run on an Apple Silicon machine, and will generate binaries for the latest release on GitHub. The generated GitHub release should be edited, with the built binaries uploaded as extra assets (named `workerd-darwin-arm64` and `workerd-linux-arm64` respectively).
## Publishing `workerd`

Since the "Release" stage requires manual upload of binaries, this "Publish" stage requires a manual run of a GitHub Action—the [Publish to NPM](.github/workflows/npm.yml) action. This action has a `workflow_dispatch` trigger that can be activated from within the GitHub UI, and takes two parameters; the patch version, and whether this release is a prerelease. If it _is_ a prerelease, the published NPM version will be tagged with `beta`, and have a version number starting with `0.`. Otherwise, the published NPM version will be tagged `latest`, and have a version number starting with `1.`.

## Publishing `workers-types`

This also requires a manual run of a GitHub Action—the [Publish types to NPM](.github/workflows/npm-types.yml) action. This action has a `workflow_dispatch` trigger that can be activated from within the GitHub UI, and takes two parameters; the patch version, and whether this is a prerelease. If it _is_ a prerelease, the published NPM version will be tagged with `beta`, and have a version number starting with `0.`. Otherwise, the published NPM version will be tagged `latest`, and have a version number starting with `4.`.
