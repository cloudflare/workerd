# JavaScript API Updates

When adding new JavaScript APIs to workerd, please remember that all compatibility flags MUST be
documented BEFORE their enable date. This is important as users need to understand what they are
opting into when they update the date. It is the responsibility of whoever introduces the flag to
update the documentation at the same time as the flag.

## Compatibility definitions for Workerd

Compatibility flags and dates for use within Workerd are defined in [compatibility-date.capnp](src/workerd/io/compatibility-date.capnp).

When you create a Workerd pull request that contains new compatibility definitions, please
create a pull request to update Cloudflare docs at the same time.

## Compatibility documentation in Cloudflare docs

Compatibility flags and dates should be added to the [Cloudflare Workers Compatibility Dates](https://developers.cloudflare.com/workers/platform/compatibility-dates) documentation.

The [cloudflare-docs](https://github.com/cloudflare/cloudflare-docs) repository is hosted on GitHub.
Compatibility flags and dates are documented in per-feature markdown files under [content/workers/_partials/_platform-compatibility-dates](https://github.com/cloudflare/cloudflare-docs/tree/production/content/workers/_partials/_platform-compatibility-dates).
