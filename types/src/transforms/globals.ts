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
    node: ts.InterfaceDeclaration | ts.ClassDeclaration
  ): ts.Node[] {
    const nodes: ts.Node[] = [];

    // Recursively extract from all superclasses
    if (node.heritageClauses !== undefined) {
      for (const clause of node.heritageClauses) {
        for (const superType of clause.types) {
          // TODO(soon): when overrides are implemented, superclasses may
          //  define type parameters (e.g. `EventTarget<WorkerGlobalScopeEventMap>`).
          //  In these cases, we'll need to inline these type params in
          //  extracted definitions. Type parameters are in `superType.typeArguments`.
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
          nodes.push(...extractGlobalNodes(superTypeDeclaration));
        }
      }
    }

    // Extract methods/properties
    for (const member of node.members) {
      const maybeNode = maybeExtractGlobalNode(member);
      if (maybeNode !== undefined) nodes.push(maybeNode);
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
