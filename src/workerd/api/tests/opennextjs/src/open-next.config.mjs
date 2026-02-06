import { defineCloudflareConfig } from '@opennextjs/cloudflare';

const config = defineCloudflareConfig({});

// Add buildCommand at the root level - this is used by @opennextjs/aws
// to run the Next.js build. Using "node --run build" avoids needing pnpm.
// --webpack is required because turbopack does not work under bazel.
// See https://github.com/vercel/next.js/discussions/89549
config.buildCommand = 'node --run build -- --webpack';

export default config;
