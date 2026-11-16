// The declarations the bootstrap contributes to the Workers types
// (@cloudflare/workers-types), in the order they appear there. The generator
// in types/ reads each one from the declaration file TypeScript emits for its
// module, at every compatibility date. See "Per-isolate declarations" in
// types/AGENTS.md for how a class declares its public surface.
//
// A declaration listed here may refer only to globals and to declarations
// listed here.

export type { URL, URLSearchParams } from './url/url';
export type {
  URLPattern,
  URLPatternInit,
  URLPatternComponentResult,
  URLPatternResult,
  URLPatternOptions,
} from './urlpattern/urlpattern';
