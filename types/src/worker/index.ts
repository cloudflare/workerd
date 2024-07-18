// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import extraDefinitions from "raw:../../defines";
import comments from "virtual:comments.json";
import paramNames from "virtual:param-names.json";
import rtti from "workerd:rtti";
import { installParameterNames } from "../generator/parameter-names";
import { printDefinitions } from "../index";

installParameterNames(paramNames);
/**
 * Worker for generating TypeScript types based on an arbitrary compatibility date
 * and optional compatibility flags. Accepts paths of the form  `/<compat-date>(+<compat_flag>)*`
 * (e.g. `/2024-01-01+nodejs_compat+no_global_navigator`)
 */
export default {
  async fetch(request): Promise<Response> {
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
      let format: "ambient" | "importable" | "bundle" = "ambient";
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
      let rttiCapnpBuffer: ArrayBuffer;
      if (compatDate === "experimental") {
        rttiCapnpBuffer = rtti.exportExperimentalTypes();
      } else {
        rttiCapnpBuffer = rtti.exportTypes(compatDate, compatFlags);
      }

      const message = new Message(rttiCapnpBuffer, /* packed */ false);
      const root = message.getRoot(StructureGroups);

      // Generate and return types in correct format
      const { ambient, importable } = printDefinitions(
        root,
        comments,
        extraDefinitions
      );

      if (format === "ambient") return new Response(ambient);

      if (format === "importable") return new Response(importable);

      const bundle = new FormData();
      bundle.set("index.d.ts", ambient);
      bundle.set("index.ts", importable);
      return new Response(bundle);
    } catch (e) {
      return new Response(e.stack, { status: 500 });
    }
  },
} satisfies ExportedHandler;
