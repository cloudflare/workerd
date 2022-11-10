import assert from "assert";
import ts from "typescript";
import { isUnsatisfiable } from "../../generator/type";
import { printNode } from "../../print";
import { maybeGetDefines, maybeGetOverride } from "./compiler";

export { compileOverridesDefines } from "./compiler";

// Applies handwritten partial TypeScript overrides to generate types to improve
// output fidelity. Also applies type renames and inserts additional handwritten
// definitions for non-generated types.
//
// See the `JSG_TS_OVERRIDE` macro's documentation in `src/workerd/jsg/jsg.h`
// for a full explanation of override rules and examples.
//
// `compileOverridesDefines()` must be used to compile overrides and defines
// into valid TypeScript source files. These should be included in the *same*
// TypeScript `Program` as the source file being transformed. This `Program`,
// along with the set of types that should be fully-replaced (also returned from
// `compileOverridesDefines()`), should be passed to this transformer factory.
//
// ```ts
// export declare class A {
//   thing: string;
// }
// export declare class B {
//   get(key: string, type: string): Promise<any>;
//   put(key: string, value: string): Promise<void>;
// }
// ```
//
// ...with the following overrides and defines...
//
// - `A`'s override: `RenamedA<Type extends string> { thing: Type; }`
// - `B`'s override: `{
//     get(key: string, type: "text"): Promise<string | null>;
//     get(key: string, type: "arrayBuffer"): Promise<ArrayBuffer | null>;
//   }`
// - `B`'s define: `interface C { foo: A; }`
//
// --- transforms to --->
//
// ```ts
// export declare class RenamedA<Type extends string> {
//   thing: Type;
// }
// export interface C {
//   foo: RenamedA;
// }
// export declare class B {
//   get(key: string, type: "text"): Promise<string | null>;
//   get(key: string, type: "arrayBuffer"): Promise<ArrayBuffer | null>;
//   put(key: string, value: string): Promise<void>;
// }
// ```
export function createOverrideDefineTransformer(
  program: ts.Program,
  replacements: Set<string>
): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const overrideCtx: OverrideTransformContext = {
        program,
        replacements,
        renames: new Map<string, string>(),
      };
      const v1 = createOverrideDefineVisitor(ctx, overrideCtx);
      const v2 = createRenameReferencesVisitor(ctx, overrideCtx.renames);
      node = ts.visitEachChild(node, v1, ctx);
      return ts.visitEachChild(node, v2, ctx);
    };
  };
}

interface OverrideTransformContext {
  program: ts.Program;
  replacements: Set<string>;
  renames: Map</* from */ string, /* to */ string>;
}

// Gets an identifying label for this member, shared between method overloads
function getMemberKey(member: ts.ClassElement | ts.TypeElement): string {
  if (ts.isConstructorDeclaration(member)) return "constructor$";

  const name = member.name;
  assert(
    name !== undefined,
    `Expected named member, got "${printNode(member)}"`
  );

  // Put static and instance members in different namespaces. For example, this
  // allows instance methods to be overridden without affecting static methods
  // of the same name.
  const isStatic =
    member.modifiers !== undefined &&
    hasModifier(member.modifiers, ts.SyntaxKind.StaticKeyword);
  const keyNamespace = isStatic ? "static$" : "instance$";

  if (
    ts.isIdentifier(name) ||
    ts.isStringLiteral(name) ||
    ts.isNumericLiteral(name)
  ) {
    return keyNamespace + name.text;
  }
  if (ts.isComputedPropertyName(name)) {
    const expression = name.expression;
    if (ts.isStringLiteral(expression) || ts.isNumericLiteral(expression)) {
      return keyNamespace + expression.text;
    }
  }
  return keyNamespace + printNode(name);
}

// Groups override members by their identifying labels from `getMemberKey()`
function groupMembersByKey<Member extends ts.ClassElement | ts.TypeElement>(
  members: ts.NodeArray<Member>
): Map<string, Member[]> {
  const result = new Map<string, Member[]>();
  members.forEach((member) => {
    const key = getMemberKey(member);
    let array = result.get(key);
    if (array === undefined) result.set(key, (array = []));
    array.push(member);
  });
  return result;
}

// Returns the index of a member in `members` with the specified `key`, or -1 if
// none exists
function findMemberIndex<Member extends ts.ClassElement | ts.TypeElement>(
  members: Member[],
  key: string,
  fromIndex = 0
): number {
  return members.findIndex(
    (member, index) => fromIndex <= index && getMemberKey(member) === key
  );
}

// Merges generated members with overrides according to the following rules:
// 1. Members in the override but not in the generated type are inserted
// 2. If an override has the same key as a member in the generated type, the
//   generated member is removed, and the override is inserted instead
// 3. If an override member property is declared type `never`, it is not
//   inserted, but its presence may remove the generated member (as per 2)
function mergeMembers<Member extends ts.ClassElement | ts.TypeElement>(
  generated: ts.NodeArray<Member>,
  overrides: ts.NodeArray<ts.ClassElement>,
  transformer: (member: ts.ClassElement) => Member
): Member[] {
  const result = [...generated];
  const grouped = groupMembersByKey(overrides);
  for (const [key, overrideMembers] of grouped) {
    const filteredOverrideMembers = overrideMembers.filter((member) => {
      // Filter out `never` typed properties
      if (ts.isPropertyDeclaration(member) && member.type !== undefined) {
        return !isUnsatisfiable(member.type);
      }
      // Include all other members
      return true;
    });
    // Transform all class elements into the correct member type. If `Member` is
    // `ts.ClassElement` already, `transformer` will be the identify function.
    const transformedOverrideMembers = filteredOverrideMembers.map(transformer);

    // Try to find index of existing generated member with same key
    const index = findMemberIndex(result, key);
    if (index === -1) {
      // If the member couldn't be found, insert overrides at the end
      result.push(...transformedOverrideMembers);
    } else {
      const member = result[index];
      const nextIndex = findMemberIndex(result, key, index + 1);
      if (
        ts.isGetAccessorDeclaration(member) ||
        ts.isSetAccessorDeclaration(member)
      ) {
        // If this is a getter/setter, it's possible there's one other
        // getter/setter with the same key.
        if (nextIndex !== -1) {
          // Make sure this other member was a getter/setter
          const nextMember = result[nextIndex];
          assert(
            ts.isGetAccessorDeclaration(nextMember) ||
              ts.isSetAccessorDeclaration(nextMember),
            `Expected getter/setter, got "${printNode(nextMember)}"`
          );

          // Remove the other getter/setter. Because `nextIndex > index`, we'll
          // still be able to `splice(index)` later on.
          assert(nextIndex > index);
          result.splice(nextIndex, /* deleteCount */ 1);

          // Make sure this was the only other member with this key
          const nextNextIndex = findMemberIndex(result, key, nextIndex + 1);
          assert(nextNextIndex === -1);
        }
      } else {
        // Otherwise, make sure this was the only generated member with this key
        assert(nextIndex === -1);
      }

      // Remove the member at that index and replace it with overrides
      result.splice(index, /* deleteCount */ 1, ...transformedOverrideMembers);
    }
  }
  return result;
}

// Converts class members to interface members where possible. Used as a
// transformer when merging override members (which will always be class
// members) into an interface.
function classToTypeElement(
  ctx: ts.TransformationContext,
  member: ts.ClassElement
): ts.TypeElement {
  if (ts.isMethodDeclaration(member)) {
    return ctx.factory.createMethodSignature(
      member.modifiers,
      member.name,
      member.questionToken,
      member.typeParameters,
      member.parameters,
      member.type
    );
  }
  if (ts.isPropertyDeclaration(member)) {
    return ctx.factory.createPropertySignature(
      member.modifiers,
      member.name,
      member.questionToken,
      member.type
    );
  }
  if (
    ts.isGetAccessorDeclaration(member) ||
    ts.isSetAccessorDeclaration(member) ||
    ts.isIndexSignatureDeclaration(member)
  ) {
    return member;
  }
  assert.fail(
    `Expected interface-compatible member, got "${printNode(member)}".
You'll need to define a full-replacement override to a "class" if you wish to insert this member (i.e. "JSG_TS_OVERRIDE(class MyClass { <all_members> })").`
  );
}

// Finds and applies the override (if any) for a node, returning the new
// potentially overridden node
function applyOverride<
  Node extends ts.ClassDeclaration | ts.InterfaceDeclaration
>(
  ctx: ts.TransformationContext,
  overrideCtx: OverrideTransformContext,
  node: Node,
  updateDeclaration: (node: Node, override: ts.ClassDeclaration) => Node
): ts.Node {
  assert(node.name !== undefined);
  const name = node.name.text;
  const override = maybeGetOverride(overrideCtx.program, name);
  const isReplacement = overrideCtx.replacements.has(name);

  // Full-type replacement may rename type too, so record renames now
  if (override !== undefined) {
    // If override's name is different to the node's name, rename it later
    const overrideIdentifier = maybeGetStatementName(override);
    if (overrideIdentifier !== undefined) {
      const overrideName = overrideIdentifier.text;
      if (name !== overrideName) overrideCtx.renames.set(name, overrideName);
    }
  }

  if (isReplacement) {
    assert(override !== undefined);
    return ensureStatementModifiers(ctx, override);
  } else if (override !== undefined) {
    // Merge override into declaration. Whilst we convert all non-replacement
    // overrides to classes, this type classification is ignored when merging.
    // Classes just support all possible forms of override. See `./compiler.ts`
    // `compileOverride()` for details.
    assert(ts.isClassDeclaration(override));
    return updateDeclaration(node, override);
  } else {
    // No override, so return the node as is
    return node;
  }
}

// Apply all overrides, insert defines, and record type renames
function createOverrideDefineVisitor(
  ctx: ts.TransformationContext,
  overrideCtx: OverrideTransformContext
): ts.Visitor {
  // Copies all string and numeric literals. Without this, garbage would be
  // inserted in locations of literals instead.
  // TODO(soon): work out why this happens, something to do with source ranges
  //  and invalid source files/programs maybe?
  const copyLiteralsVisitor: ts.Visitor = (node) => {
    node = ts.visitEachChild(node, copyLiteralsVisitor, ctx);
    if (ts.isStringLiteral(node)) {
      return ctx.factory.createStringLiteral(node.text);
    }
    if (ts.isNumericLiteral(node)) {
      return ctx.factory.createNumericLiteral(node.text);
    }
    return node;
  };

  return (node) => {
    let defines: ts.NodeArray<ts.Statement> | undefined;

    if (ts.isClassDeclaration(node) && node.name !== undefined) {
      defines = maybeGetDefines(overrideCtx.program, node.name.text);
      node = applyOverride(ctx, overrideCtx, node, (node, override) => {
        return ctx.factory.updateClassDeclaration(
          node,
          node.decorators,
          node.modifiers,
          override.name,
          override.typeParameters ?? node.typeParameters,
          override.heritageClauses ?? node.heritageClauses,
          mergeMembers(node.members, override.members, (member) => member)
        );
      });
    } else if (ts.isInterfaceDeclaration(node)) {
      defines = maybeGetDefines(overrideCtx.program, node.name.text);
      node = applyOverride(ctx, overrideCtx, node, (node, override) => {
        assert(override.name !== undefined);
        return ctx.factory.updateInterfaceDeclaration(
          node,
          node.decorators,
          node.modifiers,
          override.name,
          override.typeParameters ?? node.typeParameters,
          override.heritageClauses ?? node.heritageClauses,
          mergeMembers(node.members, override.members, (member) =>
            classToTypeElement(ctx, member)
          )
        );
      });
    }

    // Process node and defines if defined
    node = ts.visitNode(node, copyLiteralsVisitor);
    defines = ts.visitNodes(defines, copyLiteralsVisitor);
    defines = ts.visitNodes(defines, (node) =>
      ensureStatementModifiers(ctx, node)
    );

    if (ts.isTypeAliasDeclaration(node) && isUnsatisfiable(node.type)) {
      // If node was overridden to `type T = never`, delete it, and just insert
      // defines if any
      return defines === undefined ? undefined : [...defines];
    } else {
      // Otherwise, return potentially overridden node, inserting defines if any
      // before node
      return defines == undefined ? node : [...defines, node];
    }
  };
}

// Apply previously-recorded type renames to all type references
function createRenameReferencesVisitor(
  ctx: ts.TransformationContext,
  renames: Map</* from */ string, /* to */ string>
): ts.Visitor {
  const visitor: ts.Visitor = (node) => {
    // Recursively visit all nodes
    node = ts.visitEachChild(node, visitor, ctx);

    // Rename all type references
    if (ts.isTypeReferenceNode(node) && ts.isIdentifier(node.typeName)) {
      const rename = renames.get(node.typeName.text);
      if (rename !== undefined) {
        return ctx.factory.updateTypeReferenceNode(
          node,
          ctx.factory.createIdentifier(rename),
          node.typeArguments
        );
      }
    }

    // Rename all type queries (e.g. nested types)
    if (ts.isTypeQueryNode(node) && ts.isIdentifier(node.exprName)) {
      const rename = renames.get(node.exprName.text);
      if (rename !== undefined) {
        return ctx.factory.updateTypeQueryNode(
          node,
          ctx.factory.createIdentifier(rename),
          node.typeArguments
        );
      }
    }

    // Rename all expressions with type arguments (e.g. heritage clauses)
    if (
      ts.isExpressionWithTypeArguments(node) &&
      ts.isIdentifier(node.expression)
    ) {
      const rename = renames.get(node.expression.text);
      if (rename !== undefined) {
        return ctx.factory.updateExpressionWithTypeArguments(
          node,
          ctx.factory.createIdentifier(rename),
          node.typeArguments
        );
      }
    }

    return node;
  };
  return visitor;
}

// Returns a statement's identifier if it has one. This is used to get the
// name to rename too (if any), hence we ignore variables and functions, since
// replacing a reference to a type with a referencing to a variable/function is
// an error.
function maybeGetStatementName(node: ts.Statement): ts.Identifier | undefined {
  if (
    ts.isClassDeclaration(node) ||
    ts.isInterfaceDeclaration(node) ||
    ts.isEnumDeclaration(node) ||
    ts.isTypeAliasDeclaration(node)
  ) {
    return node.name;
  }
}

// Checks whether the modifiers array contains a modifier of the specified kind
function hasModifier(modifiers: ts.ModifiersArray, kind: ts.Modifier["kind"]) {
  let hasModifier = false;
  modifiers?.forEach((modifier) => {
    hasModifier ||= modifier.kind === kind;
  });
  return hasModifier;
}

// Ensure a modifiers array has the specified modifier, inserting it at the
// start if it doesn't.
function ensureModifier(
  ctx: ts.TransformationContext,
  modifiers: ts.ModifiersArray | undefined,
  ensure: ts.SyntaxKind.ExportKeyword | ts.SyntaxKind.DeclareKeyword
): ts.ModifiersArray {
  // If modifiers already contains the required modifier, return it as is...
  if (modifiers !== undefined && hasModifier(modifiers, ensure)) {
    return modifiers;
  }
  // ...otherwise, add the modifier to the start of the array
  return ctx.factory.createNodeArray(
    [ctx.factory.createToken(ensure), ...(modifiers ?? [])],
    modifiers?.hasTrailingComma
  );
}

// Ensure a modifiers array includes the `export` modifier
function ensureExportModifier(
  ctx: ts.TransformationContext,
  modifiers: ts.ModifiersArray | undefined
): ts.ModifiersArray {
  return ensureModifier(ctx, modifiers, ts.SyntaxKind.ExportKeyword);
}

// Ensures a modifiers array includes the `export declare` modifiers
function ensureExportDeclareModifiers(
  ctx: ts.TransformationContext,
  modifiers: ts.ModifiersArray | undefined
): ts.ModifiersArray {
  // Call in reverse, so we end up with `export declare` not `declare export`
  modifiers = ensureModifier(ctx, modifiers, ts.SyntaxKind.DeclareKeyword);
  return ensureModifier(ctx, modifiers, ts.SyntaxKind.ExportKeyword);
}

// Make sure replacement node is `export`ed, with the `declare` modifier if it's
// a class, variable or function declaration.
function ensureStatementModifiers(
  ctx: ts.TransformationContext,
  node: ts.Node
): ts.Node {
  if (ts.isClassDeclaration(node)) {
    return ctx.factory.updateClassDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers),
      node.name,
      node.typeParameters,
      node.heritageClauses,
      node.members
    );
  }
  if (ts.isInterfaceDeclaration(node)) {
    return ctx.factory.updateInterfaceDeclaration(
      node,
      node.decorators,
      ensureExportModifier(ctx, node.modifiers),
      node.name,
      node.typeParameters,
      node.heritageClauses,
      node.members
    );
  }
  if (ts.isEnumDeclaration(node)) {
    return ctx.factory.updateEnumDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers),
      node.name,
      node.members
    );
  }
  if (ts.isTypeAliasDeclaration(node)) {
    return ctx.factory.updateTypeAliasDeclaration(
      node,
      node.decorators,
      ensureExportModifier(ctx, node.modifiers),
      node.name,
      node.typeParameters,
      node.type
    );
  }
  if (ts.isVariableStatement(node)) {
    return ctx.factory.updateVariableStatement(
      node,
      ensureExportDeclareModifiers(ctx, node.modifiers),
      node.declarationList
    );
  }
  if (ts.isFunctionDeclaration(node)) {
    return ctx.factory.updateFunctionDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers),
      node.asteriskToken,
      node.name,
      node.typeParameters,
      node.parameters,
      node.type,
      node.body
    );
  }
  if (ts.isModuleDeclaration(node)) {
    return ctx.factory.updateModuleDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers),
      node.name,
      node.body
    );
  }
  assert.fail(`Expected statement, got "${printNode(node)}"`);
}
