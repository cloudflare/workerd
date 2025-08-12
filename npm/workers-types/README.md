# Cloudflare Workers Types

> **Note**
>
> We now recommend using the [Wrangler CLI](https://www.npmjs.com/package/wrangler) and the `wrangler types` command to generate types based on your compatibility date _and_ compatibility flags. You can learn more about this, and how to migrate from @cloudflare/workers-types [here in our docs](https://developers.cloudflare.com/workers/languages/typescript/#generate-types).
>
> @cloudflare/workers-types will continue to be published on the same schedule.


## Install

```bash
npm install -D @cloudflare/workers-types
-- Or
yarn add -D @cloudflare/workers-types
```

## Usage

The following is a minimal `tsconfig.json` for use alongside this package:

**`tsconfig.json`**

```json
{
  "compilerOptions": {
    "target": "esnext",
    "module": "esnext",
    "lib": ["esnext"],
    "types": ["@cloudflare/workers-types"]
  }
}
```

### Compatibility dates

The Cloudflare Workers runtime manages backwards compatibility through the use of [Compatibility Dates](https://developers.cloudflare.com/workers/platform/compatibility-dates/). The `@cloudflare/workers-types` package provides a typing environment that corresponds to the latest version of the Cloudflare Workers runtime. Instead of using `@cloudflare/workers-types` directly, we recommend following the [Typescript language documentation](https://developers.cloudflare.com/workers/languages/typescript/) for Cloudflare Workers to generate a runtime typing environment that corresponds exactly to your compatibility date and flags.

### Importable Types

It's not always possible (or desirable) to modify the `tsconfig.json` settings for a project to include all the Cloudflare Workers types. For use cases like that, this package provides importable versions of its types, which are usable with no additional `tsconfig.json` setup. For example:

```ts
import type { Request as WorkerRequest, ExecutionContext } from "@cloudflare/workers-types"

export default {
  fetch(request: WorkerRequest, env: unknown, ctx: ExecutionContext) {
    return new Response("OK")
  }
}
```


### Using bindings

It's recommended that you create a type file for any bindings your Worker uses. Create a file named
`worker-configuration.d.ts` in your src directory.

If you're using Module Workers, it should look like this:
```typescript
// worker-configuration.d.ts
interface Env {
  MY_ENV_VAR: string;
  MY_SECRET: string;
  myKVNamespace: KVNamespace;
}
```
For Service Workers, it should augment the global scope:
```typescript
// worker-configuration.d.ts
declare global {
  const MY_ENV_VAR: string;
  const MY_SECRET: string;
  const myKVNamespace: KVNamespace;
}
export {}
```

Wrangler can also generate this for you automatically from your `wrangler.toml` configuration file, using the `wrangler types` command.

