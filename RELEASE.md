# Releasing `workerd` and `workers-types` to NPM

The primary distribution channel for `workerd` right now is through `npm`. We use a (mostly) automatic CI setup to release and publish both `workerd` and `workers-types`.

## Versioning

`workerd` and `workers-types` follow the same versioning system. The major version is currently `1` for `workerd` and `4` for `workers-types`. The minor version for both is the compatibility date specified in [supported-compatibility-date.txt](src/workerd/io/supported-compatibility-date.txt) (the latest compatibility date that the `workerd` release will support).

## Cutting Releases

This is pretty simple, and completely automatic—every time the compatibility date in [supported-compatibility-date.txt](src/workerd/io/supported-compatibility-date.txt) changes, a new release is generated, along with the built binaries for `linux-64`, `linux-arm64`, `darwin-64`, `darwin-arm64` and `windows-64`. The types will also be generated and published automatically. This is governed by the [release.yml](.github/workflows/release.yml) GitHub Action.
