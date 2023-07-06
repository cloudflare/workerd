// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import ts from "typescript";

// Copies all properties of `ServiceWorkerGlobalScope` and its superclasses into
// the global scope:
//
// ```ts
// export declare class EventTarget {
//   constructor();
//   addEventListener(...): ...;
// }
// export declare abstract WorkerGlobalScope extends EventTarget {
//   ...
// }
// export interface ServiceWorkerGlobalScope extends WorkerGlobalScope {
//   DOMException: typeof DOMException;
//   btoa(value: string): string;
//   crypto: Crypto;
//   ...
// }
// ```
//
// --- transforms to --->
//
// ```ts
// export declare class EventTarget { ... }
// export declare abstract WorkerGlobalScope extends EventTarget { ... }
// export interface ServiceWorkerGlobalScope extends WorkerGlobalScope { ... }
//
// export declare function addEventListener(...): ...;
// export declare function btoa(value: string): string;
// export declare const crypto: Crypto;
// ```
export function createGlobalScopeTransformer(
  checker: ts.TypeChecker
): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createGlobalScopeVisitor(ctx, checker);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

// Copy type nodes everywhere they are referenced
function createInlineVisitor(
  ctx: ts.TransformationContext,
  inlines: Map<string, ts.TypeNode>
): ts.Visitor {
  // If there's nothing to inline, just return identity visitor
  if (inlines.size === 0) return (node) => node;

  const visitor: ts.Visitor = (node) => {
    // Recursively visit all nodes
    node = ts.visitEachChild(node, visitor, ctx);

    // Inline all matching type references
    if (ts.isTypeReferenceNode(node) && ts.isIdentifier(node.typeName)) {
      const inline = inlines.get(node.typeName.text);
      if (inline !== undefined) return inline;
    }

    return node;
  };
  return visitor;
}

export function createGlobalScopeVisitor(
  ctx: ts.TransformationContext,
  checker: ts.TypeChecker
) {
  // Call with each potential method/property that could be extracted into a
  // global function/const.
  function maybeExtractGlobalNode(node: ts.Node): ts.Node | undefined {
    if (
      (ts.isMethodSignature(node) || ts.isMethodDeclaration(node)) &&
      ts.isIdentifier(node.name)
    ) {
      const modifiers: ts.Modifier[] = [
        ctx.factory.createToken(ts.SyntaxKind.ExportKeyword),
        ctx.factory.createToken(ts.SyntaxKind.DeclareKeyword),
      ];
      return ctx.factory.createFunctionDeclaration(
        /* decorators */ undefined,
        modifiers,
        /* asteriskToken */ undefined,
        node.name,
        node.typeParameters,
        node.parameters,
        node.type,
        /* body */ undefined
      );
    }
    if (
      (ts.isPropertySignature(node) ||
        ts.isPropertyDeclaration(node) ||
        ts.isGetAccessorDeclaration(node)) &&
      ts.isIdentifier(node.name)
    ) {
      assert(node.type !== undefined);
      // Don't create global nodes for nested types, they'll already be there
      if (!ts.isTypeQueryNode(node.type)) {
        const modifiers: ts.Modifier[] = [
          ctx.factory.createToken(ts.SyntaxKind.ExportKeyword),
          ctx.factory.createToken(ts.SyntaxKind.DeclareKeyword),
        ];
        const varDeclaration = ctx.factory.createVariableDeclaration(
          node.name,
          /* exclamationToken */ undefined,
          node.type
        );
        const varDeclarationList = ctx.factory.createVariableDeclarationList(
          [varDeclaration],
          ts.NodeFlags.Const // Use `const` instead of `var`
        );
        return ctx.factory.createVariableStatement(
          modifiers,
          varDeclarationList
        );
      }
    }
  }

  // Called with each class/interface that should have its methods/properties
  // extracted into global functions/consts. Recursively visits superclasses.
  function extractGlobalNodes(
    node: ts.InterfaceDeclaration | ts.ClassDeclaration,
    typeArgs?: ts.NodeArray<ts.TypeNode>
  ): ts.Node[] {
    const nodes: ts.Node[] = [];

    // If this declaration has type parameters, we'll need to inline them when
    // extracting members.
    const typeArgInlines = new Map<string, ts.TypeNode>();
    if (node.typeParameters) {
      assert(
        node.typeParameters.length === typeArgs?.length,
        `Expected ${node.typeParameters.length} type argument(s), got ${typeArgs?.length}`
      );
      node.typeParameters.forEach((typeParam, index) => {
        typeArgInlines.set(typeParam.name.text, typeArgs[index]);
      });
    }
    const inlineVisitor = createInlineVisitor(ctx, typeArgInlines);

    // Recursively extract from all superclasses
    if (node.heritageClauses !== undefined) {
      for (let clause of node.heritageClauses) {
        // Handle case where type param appears in heritage clause:
        // ```ts
        // class A<T> {}     // ↓
        // class B<T> extends A<T> {}
        // class C extends B<string> {}
        // ```
        clause = ts.visitNode(clause, inlineVisitor);

        for (const superType of clause.types) {
          const superTypeSymbol = checker.getSymbolAtLocation(
            superType.expression
          );
          assert(superTypeSymbol !== undefined);
          const superTypeDeclarations = superTypeSymbol.getDeclarations();
          assert.strictEqual(superTypeDeclarations?.length, 1);
          const superTypeDeclaration = superTypeDeclarations[0];
          assert(
            ts.isInterfaceDeclaration(superTypeDeclaration) ||
              ts.isClassDeclaration(superTypeDeclaration)
          );
          nodes.push(
            // Pass any defined type arguments for inlining in extracted nodes
            // (e.g. `...extends EventTarget<WorkerGlobalScopeEventMap>`).
            ...extractGlobalNodes(superTypeDeclaration, superType.typeArguments)
          );
        }
      }
    }

    // Extract methods/properties
    for (const member of node.members) {
      const maybeNode = maybeExtractGlobalNode(member);
      if (maybeNode !== undefined) {
        nodes.push(ts.visitNode(maybeNode, inlineVisitor));
      }
    }

    return nodes;
  }

  // Finds the `ServiceWorkerGlobalScope` declaration, calls
  // `extractGlobalNodes` with it, and inserts all extracted nodes.
  const serviceWorkerGlobalScopeVisitor: ts.Visitor = (node) => {
    if (
      (ts.isInterfaceDeclaration(node) || ts.isClassDeclaration(node)) &&
      node.name !== undefined &&
      node.name.text === "ServiceWorkerGlobalScope"
    ) {
      return [node, ...extractGlobalNodes(node)];
    }
    return node;
  };
  return serviceWorkerGlobalScopeVisitor;
}
