# OpenNext SSR Test

This directory contains tests for running actual OpenNext Cloudflare bundled output in workerd.
The test verifies that workerd can correctly execute Next.js SSR applications built with the
`@opennextjs/cloudflare` adapter.

## Files

- `opennext-ssr-test.js` - Test cases for API routes, SSR pages, streaming, RSC, etc.
- `opennext-ssr-test.wd-test` - workerd test configuration
- `src/` - Next.js application source files (JavaScript/JSX)

## Running the Test

The worker is automatically generated from source before the test runs:

```bash
bazel test //src/workerd/api/tests/opennextjs:opennext-ssr-test@
```

**Note:** This test is Linux-only (`target_compatible_with = ["@platforms//os:linux"]`) due to
the reliance on the Next.js/OpenNext build toolchain.

## How It Works

1. **opennextjs-build**: Runs `@opennextjs/cloudflare build` to compile the Next.js app and
   generate the OpenNext worker in `.open-next/`
2. **opennextjs-worker**: Runs `wrangler deploy --dry-run --outdir=dist` to bundle the worker
3. The output `dist/worker.js` is copied to `opennext-ssr-worker.js`
4. The test runner loads the worker and executes test cases against it

## Compatibility Flags

The test harness uses `nodejs_compat_v2` along with several additional Node.js module flags required (and defined in `src/wrangler.jsonc`)
by the OpenNext runtime. These flags are configured in:

- `src/wrangler.jsonc` - For the wrangler build
- `opennext-ssr-test.wd-test` - For the workerd test runtime

Key flags include:

- `nodejs_compat_v2` - Enables Node.js compatibility layer (required due to test harness using oldest & latest compatibility dates)
- `enable_nodejs_os_module` - Required for `node:os` module
- `enable_nodejs_fs_module` - Required for `node:fs` module
- `enable_nodejs_vm_module` - Required for `node:vm` module
- `enable_nodejs_http_modules` - Required for `node:http` modules

## Why JavaScript Instead of TypeScript?

The Next.js app uses JavaScript (`.js`/`.jsx`) instead of TypeScript because Next.js
automatically modifies `tsconfig.json` during builds. This conflicts with Bazel's sandbox,
which treats source files as read-only. Using JavaScript with `jsconfig.json` avoids this issue.

## Build Sandbox Note

The `opennextjs-build` target uses `execution_requirements = {"no-sandbox": "1"}` because
Next.js needs to write various files during the build process (caches, generated files, etc.)
which is incompatible with Bazel's default read-only sandbox.
