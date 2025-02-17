# JavaScript API Updates

When adding new JavaScript APIs to workerd, please remember that all compatibility flags MUST be
documented BEFORE their enable date. This is important as users need to understand what they are
opting into when they update the date. It is the responsibility of whoever introduces the flag to
update the documentation at the same time as the flag.

## Compatibility definitions for Workerd

Compatibility flags and dates for use within Workerd are defined in [compatibility-date.capnp](../src/workerd/io/compatibility-date.capnp).

When you create a Workerd pull request that contains new compatibility definitions, please
create a pull request to update Cloudflare docs at the same time.

## Compatibility documentation in Cloudflare docs

Compatibility flags should be added to the [Cloudflare Workers Compatibility flags](https://developers.cloudflare.com/workers/configuration/compatibility-flags/) documentation.

The [cloudflare-docs](https://github.com/cloudflare/cloudflare-docs) repository is hosted on GitHub.
Compatibility flags are documented in per-feature markdown files under [src/content/compatibility-flags](https://github.com/cloudflare/cloudflare-docs/tree/production/src/content/compatibility-flags).
