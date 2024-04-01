// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO(someday): make this a TypeScript file: it needs to be typed under its own output. :)
//  Would probably want to update `api-encoder` to generate full RTTI with bundles, then
//  pass this to the type generator CLI.

import extraDefinitions from "raw:../../defines";
import comments from "virtual:comments.json";
import paramNames from "virtual:param-names.json";
import rtti from "workerd:rtti";
import {
  buildTypes,
  installComments,
  installParameterNames,
  makeImportable,
} from "../";

installParameterNames(paramNames);
installComments(comments);

/**
 * Worker for generating TypeScript types based on an arbitrary compatibility date
 * and optional compatibility flags. Accepts paths of the form  `/<compat-date>(+<compat_flag>)*`
 * (e.g. `/2024-01-01+nodejs_compat+no_global_navigator`)
 */
export default {
  /** @param {Request} request */
  fetch(request) {
    try {
      // Ensure pathname matches expected pattern
      let { pathname } = new URL(request.url);
      if (
        !/^\/\d{4}-\d{2}-\d{2}\+?/.test(pathname) &&
        !pathname.startsWith("/experimental")
      ) {
        return new Response("Not Found", { status: 404 });
      }

      // Extract response format
      /** @type {"ambient" | "importable" | "bundle"} */
      let format = "ambient";
      if (pathname.endsWith(".d.ts")) {
        pathname = pathname.slice(0, -".d.ts".length);
      } else if (pathname.endsWith(".ts")) {
        pathname = pathname.slice(0, -".ts".length);
        format = "importable";
      } else if (pathname.endsWith(".bundle")) {
        pathname = pathname.slice(0, -".bundle".length);
        format = "bundle";
      }

      // Export RTTI for specified compatibility settings
      const [compatDate, ...compatFlags] = pathname.substring(1).split("+");
      let rttiCapnpBuffer;
      if (compatDate === "experimental") {
        rttiCapnpBuffer = rtti.exportExperimentalTypes();
      } else {
        rttiCapnpBuffer = rtti.exportTypes(compatDate, compatFlags);
      }

      // Generate and return types in correct format
      const ambientDefinitions = buildTypes({
        rttiCapnpBuffer,
        extraDefinitions,
      });
      if (format === "ambient") return new Response(ambientDefinitions);

      const importableDefinitions = makeImportable(ambientDefinitions);
      if (format === "importable") return new Response(importableDefinitions);

      const bundle = new FormData();
      bundle.set("index.d.ts", ambientDefinitions);
      bundle.set("index.ts", importableDefinitions);
      return new Response(bundle);
    } catch (e) {
      return new Response(e.stack, { status: 500 });
    }
  },
};
