import ts from "typescript";

/**
 * Transforms an array of classes to an interface/variable pair, preserving the ability to construct the class
 * and call static methods.
 *
 * @example
 *
 * class MyClass<T> {
 *   constructor(str: string): MyClass<T>;
 *   prop: T;
 *   method(): void {}
 *   static staticMethod(str?: string): void {}
 * }
 *
 * // Becomes this:
 *
 * declare var MyClass: {
 *   prototype: MyClass;
 *   new <T>(str: string): MyClass<T>;
 *   staticMethod(str?: string): void;
 * }
 * interface MyClass<T = void, U = void> {
 *   prop: T;
 *   method(): U;
 * }
 *
 * NB:
 *   1. Generics are preserved and provided to the `new` method on the var declaration.
 *   2. Static methods are added to the var declaration instead of the interface.
 */
export function createClassToInterfaceTransformer(
  classNames: string[]
): ts.TransformerFactory<ts.SourceFile> {
  return (context) => {
    const visitor: ts.Visitor = (node) => {
      if (
        ts.isClassDeclaration(node) &&
        node.name &&
        classNames.includes(node.name.text)
      ) {
        return transformClassToInterface(node, context);
      }
      return ts.visitEachChild(node, visitor, context);
    };

    return (sourceFile) => {
      const transformedNodes = ts.visitNodes(sourceFile.statements, visitor);
      const filteredNodes = transformedNodes.filter(ts.isStatement);
      return context.factory.updateSourceFile(sourceFile, filteredNodes);
    };
  };
}

/**
 * Transforms a TypeScript class declaration into an interface and a variable declaration.
 * Used where you want to separate the type definition (interface)
 * from the runtime representation (variable declaration) of a class.
 */
function transformClassToInterface(
  node: ts.ClassDeclaration,
  context: ts.TransformationContext
): ts.Statement[] {
  const interfaceDeclaration = createInterfaceDeclaration(node, context);
  const varDeclaration = createVariableDeclaration(node, context);
  return [varDeclaration, interfaceDeclaration];
}

/**
 * Creates an interface declaration from a class declaration.
 * Extracts class members and converts them into interface members,
 * preserving access modifiers and type parameters.
 */
function createInterfaceDeclaration(
  node: ts.ClassDeclaration,
  context: ts.TransformationContext
): ts.InterfaceDeclaration {
  const interfaceMembers = transformClassMembers(node.members, context, false);
  return context.factory.createInterfaceDeclaration(
    getAccessModifiers(ts.getModifiers(node)),
    node.name!,
    node.typeParameters,
    node.heritageClauses,
    interfaceMembers
  );
}

/**
 * Transforms class members into interface type elements.
 * Filters and converts class elements into a format suitable for interfaces,
 * optionally including static members.
 */
function transformClassMembers(
  members: ts.NodeArray<ts.ClassElement>,
  context: ts.TransformationContext,
  includeStatic: boolean
): ts.TypeElement[] {
  return members
    .map((member) =>
      transformClassMemberToInterface(member, context, includeStatic)
    )
    .filter((member): member is ts.TypeElement => member !== undefined);
}

/**
 * Transforms a single class member into an interface element.
 * Handles different types of class elements, such as properties and methods,
 * and applies access modifiers appropriately.
 *
 * @example
 * // Given the following class declarations:
 *
 * myMethod(): void {}
 * private myPrivateProperty: string;
 *
 * // The function will produce an interface declarations similar to:
 *
 * myMethod(): void;
 *
 * Note: Private members like `myPrivateProperty` are not included in the interface.
 */
function transformClassMemberToInterface(
  member: ts.ClassElement,
  context: ts.TransformationContext,
  includeStatic: boolean
): ts.TypeElement | undefined {
  const modifiers = ts.canHaveModifiers(member)
    ? ts.getModifiers(member)
    : undefined;
  const isStatic =
    modifiers?.some((mod) => mod.kind === ts.SyntaxKind.StaticKeyword) ?? false;

  if (isStatic !== includeStatic) {
    return undefined;
  }

  const isPrivate =
    modifiers?.some((mod) => mod.kind === ts.SyntaxKind.PrivateKeyword) ??
    false;

  if (isPrivate) {
    return undefined;
  }

  const accessModifiers = getAccessModifiers(modifiers);

  if (ts.isPropertyDeclaration(member)) {
    return createPropertySignature(member, accessModifiers, context);
  } else if (ts.isMethodDeclaration(member)) {
    return createMethodSignature(member, accessModifiers, context);
  } else if (ts.isGetAccessor(member)) {
    return createGetAccessorSignature(member, accessModifiers, context);
  } else if (ts.isSetAccessor(member) || ts.isConstructorDeclaration(member)) {
    return undefined;
  }

  console.warn(`Unhandled member type: ${ts.SyntaxKind[member.kind]}`);
  return undefined;
}

/**
 * Creates a property signature for an interface from a class property declaration.
 * Preserves access modifiers and optionality.
 *
 * @example
 * // Given a TypeScript class property declaration:
 *
 * public optionalProp?: string;
 *
 * // The `createPropertySignature` function will produce an interface property signature:
 *
 * optionalProp?: string;
 */
function createPropertySignature(
  member: ts.PropertyDeclaration,
  modifiers: ts.Modifier[] | undefined,
  context: ts.TransformationContext
): ts.PropertySignature {
  return context.factory.createPropertySignature(
    modifiers,
    member.name,
    member.questionToken,
    member.type
  );
}

/**
 * Creates a method signature for an interface from a class method declaration.
 * Handles method parameters and return types.
 *
 * @example
 * // Given a TypeScript class method declaration:
 *
 * public doSomething(param: number): string {
 *   return param.toString();
 * }
 *
 * // The `createMethodSignature` function will produce an interface method signature:
 *
 * doSomething(param: number): string;
 */
function createMethodSignature(
  member: ts.MethodDeclaration,
  modifiers: ts.Modifier[] | undefined,
  context: ts.TransformationContext
): ts.MethodSignature {
  return context.factory.createMethodSignature(
    modifiers,
    member.name,
    member.questionToken,
    member.typeParameters,
    member.parameters,
    member.type
  );
}

/**
 * Creates a property signature for an interface from a class `get` accessor declaration.
 * Used to represent getter methods as properties in interfaces.
 *
 * @example
 * // Given a TypeScript class with a getter:
 *
 * get value(): number {
 *   return 42;
 * }
 *
 * // The `createGetAccessorSignature` function will produce an interface property signature:
 *
 * value: number;
 */
function createGetAccessorSignature(
  member: ts.GetAccessorDeclaration,
  modifiers: ts.Modifier[] | undefined,
  context: ts.TransformationContext
): ts.PropertySignature {
  return context.factory.createPropertySignature(
    modifiers,
    member.name,
    undefined,
    member.type
  );
}

/**
 * Creates a variable declaration for a class, representing its runtime type.
 * Declares a variable with the class name and its associated type.
 *
 * @example
 * // Given a TypeScript class declaration:
 *
 * class Example {
 *   static staticMethod(): void {}
 *   constructor(public value: number) {}
 * }
 *
 * // The `createVariableDeclaration` function will produce a variable declaration:
 *
 * declare var Example: {
 *   prototype: Example;
 *   new (value: number): Example;
 *   staticMethod(): void;
 * };
 */
function createVariableDeclaration(
  node: ts.ClassDeclaration,
  context: ts.TransformationContext
): ts.VariableStatement {
  return context.factory.createVariableStatement(
    [context.factory.createModifier(ts.SyntaxKind.DeclareKeyword)],
    context.factory.createVariableDeclarationList(
      [
        context.factory.createVariableDeclaration(
          node.name!,
          undefined,
          createClassType(node, context)
        ),
      ],
      ts.NodeFlags.None
    )
  );
}

/**
 * Creates a type literal node representing the static members and prototype of a class.
 * Used to define the type structure of a class's static side.
 *
 * @example
 * // Given a TypeScript class with static members:
 *
 * class Example {
 *   constructor(public value: number) {}
 *   static staticMethod(): void {}
 * }
 *
 * // The `createClassType` function will produce a type literal node:
 *
 * {
 *   prototype: Example;
 *   new (value: number): Example;
 *   staticMethod(): void;
 * }
 */
function createClassType(
  node: ts.ClassDeclaration,
  context: ts.TransformationContext
): ts.TypeLiteralNode {
  const staticMembers = transformClassMembers(node.members, context, true);
  return context.factory.createTypeLiteralNode([
    createPrototypeProperty(node, context),
    createConstructSignature(node, context),
    ...staticMembers,
  ]);
}

/**
 * Creates a construct signature for a class, representing its constructor.
 * Includes type parameters and parameter types in the signature.
 *
 * @example
 * // Given a TypeScript class constructor:
 *
 * class Example<T> {
 *   constructor(public value: T) {}
 * }
 *
 * // The `createConstructSignature` function will produce a construct signature:
 *
 * new <T>(value: T): Example<T>;
 */
function createConstructSignature(
  node: ts.ClassDeclaration,
  context: ts.TransformationContext
): ts.ConstructSignatureDeclaration {
  const constructorDeclaration = node.members.find(ts.isConstructorDeclaration);
  const typeParameters = node.typeParameters;

  const returnType = context.factory.createTypeReferenceNode(
    node.name!,
    typeParameters?.map((param) =>
      context.factory.createTypeReferenceNode(param.name, undefined)
    )
  );

  return context.factory.createConstructSignature(
    typeParameters,
    constructorDeclaration?.parameters ?? [],
    returnType
  );
}

/**
 * Creates a property signature for the prototype property of a class.
 * Used to represent the prototype chain in the class type.
 *
 * @example
 * // Given a TypeScript class:
 *
 * class Example {}
 *
 * // The `createPrototypeProperty` function will produce a property signature:
 *
 * prototype: Example;
 */
function createPrototypeProperty(
  node: ts.ClassDeclaration,
  context: ts.TransformationContext
): ts.PropertySignature {
  return context.factory.createPropertySignature(
    undefined,
    "prototype",
    undefined,
    context.factory.createTypeReferenceNode(node.name!, undefined)
  );
}

/**
 * Filters and returns the access modifiers applicable to a class member.
 * Extracts modifiers such as readonly, public, protected, and private.
 */
function getAccessModifiers(
  modifiers: readonly ts.Modifier[] | undefined
): ts.Modifier[] | undefined {
  return modifiers?.filter(
    (mod) =>
      mod.kind === ts.SyntaxKind.ReadonlyKeyword ||
      mod.kind === ts.SyntaxKind.PublicKeyword ||
      mod.kind === ts.SyntaxKind.ProtectedKeyword ||
      mod.kind === ts.SyntaxKind.PrivateKeyword
  );
}
