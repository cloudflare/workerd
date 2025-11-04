// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import ts from "typescript";

// Adapted from https://github.com/cloudflare/workerd/blob/2182afdd8ca9ac35fb18b76205308fabd5000d01/src/node/tsconfig.json#L27-L32.
// Maps import specifier patterns to target path pattern. Sorted by target path
// specificity (highest to lowest). When un-resolving target paths to import
// specifiers, the most specific will be checked first. Note all target paths
// must be absolute (i.e. start with "/").
const TSCONFIG_PATHS: Record<string, string> = {
  "node-internal:*": "/internal/*",
  "node:*": "/*",
};

// Resolves all relative imports in `declare module` blocks constructed from
// `*.d.ts` file contents, using `TSCONFIG_PATHS` to convert between specifiers
// and on-disk paths:
//
// ```ts
// declare module "node:crypto" {
//   const _default: {
//     DiffieHellman: new (key: import("./internal/crypto").ArrayLike, ...) => ...;
//     Hmac: new (..., options?: import("./internal/streams_transform").TransformOptions) => ...;
//   };
//   ...
// }
// declare module "node-internal:events" {
//   export namespace EventEmitter {
//     var on: typeof import("./events").on;
//   }
// }
// ```
//
// --- transforms to --->
//
// ```ts
// declare module "node:crypto" {
//   const _default: {
//     DiffieHellman: new (key: import("node-internal:crypto").ArrayLike, ...) => ...;
//     Hmac: new (..., options?: import("node-internal:streams_transform").TransformOptions) => ...;
//   };
//   ...
// }
// declare module "node-internal:events" {
//   export namespace EventEmitter {
//     var on: typeof import("node-internal:events").on;
//   }
// }
// ```
export function createImportResolveTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createImportResolveVisitor(ctx);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

// `RegExp` in wildcard rule may optionally contain single capturing group
type WildcardRule = [/* from */ RegExp, /* to */ string];
function entryWildcardRule([from, to]: [string, string]): WildcardRule {
  return [new RegExp(`^${from.replace("*", "(.+)")}$`), to];
}
function maybeApplyWildcardRule(rules: WildcardRule[], text: string): string | undefined {
  for (const [from, to] of rules) {
    const match = from.exec(text);
    if (match === null) continue;
    return match.at(1) === undefined ? to : to.replaceAll("*", match[1]);
  }
}

const pathEntries = Object.entries(TSCONFIG_PATHS);
const pathResolveRules = pathEntries.map(entryWildcardRule);
const pathUnresolveRules = pathEntries.map(([from, to]) =>
  entryWildcardRule([to, from])
);

function createImportResolveVisitor(ctx: ts.TransformationContext): ts.Visitor {
  return (node) => {
    // Visit all `declare module "<specifier>"` nodes
    if (
      ts.isModuleDeclaration(node) &&
      (node.flags & ts.NodeFlags.Namespace) === 0 &&
      ts.isStringLiteral(node.name)
    ) {
      const specifier = node.name.text;
      const maybePath = maybeApplyWildcardRule(pathResolveRules, specifier);
      // If we don't know the path of this module, we won't be able to do any
      // resolving, so return it as is
      if (maybePath === undefined) return node;
      const moduleVisitor = createModuleImportResolveVisitor(ctx, maybePath);
      return ts.visitEachChild(node, moduleVisitor, ctx);
    }

    return node;
  };
}

function createModuleImportResolveVisitor(
  ctx: ts.TransformationContext,
  referencingPath: string
): ts.Visitor {
  assert(referencingPath.startsWith("/"), "Expected absolute referencing path");
  // `file:` protocol isn't important here, just need something for a valid URL
  const referencingURL = new URL(referencingPath, "file:");

  const visitor: ts.Visitor = (node) => {
    if (
      ts.isImportTypeNode(node) &&
      ts.isLiteralTypeNode(node.argument) &&
      ts.isStringLiteral(node.argument.literal)
    ) {
      const relativeSpecifier = node.argument.literal.text;
      // If import isn't relative, no need to resolve it, so leave it as is
      if (!relativeSpecifier.startsWith(".")) return node;
      // Resolve specifier relative to referencing module
      const resolvedURL = new URL(relativeSpecifier, referencingURL);
      const resolvedPath = resolvedURL.pathname;
      // Convert resolved path back to specifier
      const specifier = maybeApplyWildcardRule(
        pathUnresolveRules,
        resolvedPath
      );
      assert(
        specifier !== undefined,
        `Unable to find matching specifier rule for path: "${resolvedPath}"`
      );

      // Update import with new specifier
      const argument = ctx.factory.updateLiteralTypeNode(
        node.argument,
        ctx.factory.createStringLiteral(specifier)
      );
      return ctx.factory.updateImportTypeNode(
        node,
        argument,
        node.attributes,
        node.qualifier,
        node.typeArguments,
        node.isTypeOf
      );
    }

    // Recursively visit all nodes (don't need to do this first as visitor never
    // creates any new import type nodes)
    return ts.visitEachChild(node, visitor, ctx);
  };
  return visitor;
}
