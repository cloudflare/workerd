import assert from "assert";
import ts from "typescript";
import { printNode } from "../print";

// Replaces custom Iterator-like interfaces with built-in `Iterator` types:
//
// ```ts
// export class Thing {
//   readonly things: ThingIterator;
//   asyncThings(): AsyncThingIterator;
// }
//
// export interface ThingIterator extends Iterator {
//   next(): ThingIteratorNext;
//   [Symbol.iterator](): any;
// }
// export interface ThingIteratorNext {
//   done: boolean;
//   value?: string;
// }
//
// export interface AsyncThingIterator extends AsyncIterator {
//   next(): Promise<AsyncThingIteratorNext>;
//   return(value?: any): Promise<AsyncThingIteratorNext>;
//   [Symbol.asyncIterator](): any;
// }
// export interface AsyncThingIteratorNext {
//   done: boolean;
//   value?: number;
// }
// ```
//
// --- transforms to --->
//
// ```ts
// export class Thing {
//   readonly things: IterableIterator<string>;
//   asyncThings(): AsyncIterableIterator<number>;
// }
// ```
export function createIteratorTransformer(
  checker: ts.TypeChecker
): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const iteratorCtx: IteratorTransformContext = {
        types: new Map(),
        nextInterfaces: new Set(),
      };
      const v1 = createIteratorDeclarationsVisitor(ctx, checker, iteratorCtx);
      const v2 = createIteratorUsagesVisitor(ctx, checker, iteratorCtx);
      node = ts.visitEachChild(node, v1, ctx);
      return ts.visitEachChild(node, v2, ctx);
    };
  };
}

interface IteratorTransformContext {
  // Maps iterator-like interfaces to built-in `Iterator` types
  types: Map<ts.Symbol, ts.TypeNode>;
  // Set of iterator-next interfaces to remove
  nextInterfaces: Set<ts.Symbol>;
}

// Find all interfaces extending `Iterator`, record their next value type,
// and remove them. Also record the names of next interfaces for removal.
function createIteratorDeclarationsVisitor(
  ctx: ts.TransformationContext,
  checker: ts.TypeChecker,
  iteratorCtx: IteratorTransformContext
): ts.Visitor {
  return (node) => {
    if (ts.isInterfaceDeclaration(node)) {
      // Check if interface extends `Iterator`
      const extendsNode = node.heritageClauses?.[0];
      if (
        extendsNode?.token === ts.SyntaxKind.ExtendsKeyword &&
        extendsNode.types.length === 1 &&
        ts.isIdentifier(extendsNode.types[0].expression) &&
        (extendsNode.types[0].expression.text === "Iterator" ||
          extendsNode.types[0].expression.text === "AsyncIterator")
      ) {
        const isAsync = extendsNode.types[0].expression.text !== "Iterator";
        // Check `node` has one of the following shapes:
        // ```ts
        // export interface ThingIterator extends Iterator {
        //   next(): ThingIteratorNext;
        //   [Symbol.iterator](): any;
        // }
        // export interface AsyncThingIterator extends AsyncIterator {
        //   next(): Promise<AsyncThingIteratorNext>;
        //   return(value?: any): Promise<AsyncThingIteratorNext>;
        //   [Symbol.asyncIterator](): any;
        // }
        // ```
        assert(
          node.members.length === (isAsync ? 3 : 2) &&
            node.members[0].name !== undefined &&
            ts.isMethodSignature(node.members[0]) &&
            ts.isIdentifier(node.members[0].name) &&
            node.members[0].name.text === "next" &&
            node.members[0].type !== undefined,
          `Expected iterator-like interface, got "${printNode(node)}"`
        );
        // Extract `IteratorBase_ThingIterator_...Next` type
        let nextTypeNode = node.members[0].type;
        if (isAsync) {
          // Unwrap Promise type
          assert(
            ts.isTypeReferenceNode(nextTypeNode) &&
              ts.isIdentifier(nextTypeNode.typeName) &&
              nextTypeNode.typeName.text === "Promise" &&
              nextTypeNode.typeArguments?.length === 1,
            `Expected Promise, got "${printNode(nextTypeNode)}"`
          );
          nextTypeNode = nextTypeNode.typeArguments[0];
        }

        // Check `IteratorBase_ThingIterator_...Next` has the following shape,
        // and extract the `value?: T` declaration
        // ```ts
        // export interface ThingIteratorNext {
        //   done: boolean;
        //   value?: string;
        // }
        // ```
        const nextType = checker.getTypeFromTypeNode(nextTypeNode);
        const nextTypeSymbol = nextType.getSymbol();
        assert(nextTypeSymbol?.members !== undefined);
        let nextValueSymbol: ts.Symbol | undefined;
        nextTypeSymbol.members.forEach((value, key) => {
          if (key === "value") nextValueSymbol = value;
        });
        assert(nextValueSymbol !== undefined);
        const nextValueDeclarations = nextValueSymbol.getDeclarations();
        assert.strictEqual(nextValueDeclarations?.length, 1);
        const nextValueDeclaration = nextValueDeclarations[0];
        assert(ts.isPropertySignature(nextValueDeclaration));
        // Mark this interface for removal
        iteratorCtx.nextInterfaces.add(nextTypeSymbol);

        // Extract `value`'s type
        const nextValueType = nextValueDeclaration.type;
        assert(nextValueType !== undefined);

        // Record this iterator type...
        const nodeType = checker.getTypeAtLocation(node);
        const nodeSymbol = nodeType.getSymbol();
        assert(nodeSymbol !== undefined);
        const iteratorType = ctx.factory.createTypeReferenceNode(
          isAsync ? "AsyncIterableIterator" : "IterableIterator",
          [nextValueType]
        );
        iteratorCtx.types.set(nodeSymbol, iteratorType);
        // ...and remove the node by returning `undefined`
        return;
      }
    }

    return node;
  };
}

// Replace uses of iterator interfaces with built-in iterator type.
// Also remove all previously recorded next interfaces.
function createIteratorUsagesVisitor(
  ctx: ts.TransformationContext,
  checker: ts.TypeChecker,
  iteratorCtx: IteratorTransformContext
): ts.Visitor {
  // Find the built-in iterator type associated with a method's return type
  // or property's type
  function findIteratorType(
    node:
      | ts.MethodSignature
      | ts.MethodDeclaration
      | ts.PropertySignature
      | ts.PropertyDeclaration
      | ts.GetAccessorDeclaration
  ): ts.TypeNode | undefined {
    if (node.type === undefined) return;
    const type = checker.getTypeFromTypeNode(node.type);
    const typeSymbol = type.getSymbol();
    if (typeSymbol !== undefined) return iteratorCtx.types.get(typeSymbol);
  }

  const visitor: ts.Visitor = (node) => {
    // Remove all next interfaces by returning `undefined`
    if (ts.isInterfaceDeclaration(node)) {
      const type = checker.getTypeAtLocation(node);
      const symbol = type.getSymbol();
      if (symbol !== undefined && iteratorCtx.nextInterfaces.has(symbol)) {
        return;
      }
    }

    // Visit all interface/class declaration children
    if (ts.isInterfaceDeclaration(node) || ts.isClassDeclaration(node)) {
      return ts.visitEachChild(node, visitor, ctx);
    }

    // Replace all method return types and property types referencing iterators
    // with the built-in type
    if (ts.isMethodSignature(node)) {
      const iteratorType = findIteratorType(node);
      if (iteratorType !== undefined) {
        return ctx.factory.updateMethodSignature(
          node,
          node.modifiers,
          node.name,
          node.questionToken,
          node.typeParameters,
          node.parameters,
          iteratorType
        );
      }
    }
    if (ts.isMethodDeclaration(node)) {
      const iteratorType = findIteratorType(node);
      if (iteratorType !== undefined) {
        return ctx.factory.updateMethodDeclaration(
          node,
          node.decorators,
          node.modifiers,
          node.asteriskToken,
          node.name,
          node.questionToken,
          node.typeParameters,
          node.parameters,
          iteratorType,
          node.body
        );
      }
    }
    if (ts.isPropertySignature(node)) {
      const iteratorType = findIteratorType(node);
      if (iteratorType !== undefined) {
        return ctx.factory.updatePropertySignature(
          node,
          node.modifiers,
          node.name,
          node.questionToken,
          iteratorType
        );
      }
    }
    if (ts.isPropertyDeclaration(node)) {
      const iteratorType = findIteratorType(node);
      if (iteratorType !== undefined) {
        return ctx.factory.updatePropertyDeclaration(
          node,
          node.decorators,
          node.modifiers,
          node.name,
          node.questionToken ?? node.exclamationToken,
          iteratorType,
          node.initializer
        );
      }
    }
    if (ts.isGetAccessorDeclaration(node)) {
      const iteratorType = findIteratorType(node);
      if (iteratorType !== undefined) {
        return ctx.factory.updateGetAccessorDeclaration(
          node,
          node.decorators,
          node.modifiers,
          node.name,
          node.parameters,
          iteratorType,
          node.body
        );
      }
    }

    return node;
  };
  return visitor;
}
