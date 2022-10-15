import assert from "assert";
import {
  Constant,
  Member,
  Member_Nested,
  Member_Which,
  Method,
  Property,
  Structure,
} from "@workerd/jsg/rtti.capnp.js";
import ts, { factory as f } from "typescript";
import { printNode } from "../print";
import {
  createParamDeclarationNodes,
  createTypeNode,
  getTypeName,
  isUnsatisfiable,
  maybeUnwrapOptional,
} from "./type";

function createMethodPartial(
  method: Method
): [ts.Modifier[], string, ts.ParameterDeclaration[], ts.TypeNode] {
  const modifiers: ts.Modifier[] = [];
  if (method.getStatic()) {
    modifiers.push(f.createToken(ts.SyntaxKind.StaticKeyword));
  }
  const name = method.getName();
  const params = createParamDeclarationNodes(method.getArgs().toArray());
  const result = createTypeNode(method.getReturnType());
  return [modifiers, name, params, result];
}

function createIteratorMethodPartial(
  method: Method,
  isAsync: boolean
): [ts.Modifier[], ts.PropertyName, ts.ParameterDeclaration[], ts.TypeNode] {
  const [modifiers, , params, result] = createMethodPartial(method);
  const symbolIteratorExpression = f.createPropertyAccessExpression(
    f.createIdentifier("Symbol"),
    isAsync ? "asyncIterator" : "iterator"
  );
  const name = f.createComputedPropertyName(symbolIteratorExpression);
  return [modifiers, name, params, result];
}

function createInstancePropertyPartial(
  prop: Property
): [ts.Modifier[], string, ts.QuestionToken | undefined, ts.TypeNode] {
  assert(!prop.getPrototype());
  const modifiers: ts.Modifier[] = [];
  if (prop.getReadonly()) {
    modifiers.push(f.createToken(ts.SyntaxKind.ReadonlyKeyword));
  }
  const name = prop.getName();
  let value = createTypeNode(prop.getType());

  // If this is an optional type, use an optional property with a `?`
  let questionToken: ts.QuestionToken | undefined;
  const unwrappedValue = maybeUnwrapOptional(value);
  if (unwrappedValue !== undefined) {
    value = unwrappedValue;
    questionToken = f.createToken(ts.SyntaxKind.QuestionToken);
  }

  return [modifiers, name, questionToken, value];
}

function createPrototypeProperty(
  prop: Property
):
  | ts.GetAccessorDeclaration
  | [ts.GetAccessorDeclaration, ts.SetAccessorDeclaration] {
  assert(prop.getPrototype());
  const name = prop.getName();
  const value = createTypeNode(prop.getType());

  const getter = f.createGetAccessorDeclaration(
    /* decorators */ undefined,
    /* modifiers */ undefined,
    name,
    /* params */ [],
    value,
    /* body */ undefined
  );

  if (prop.getReadonly()) {
    return getter;
  } else {
    const param = f.createParameterDeclaration(
      /* decorators */ undefined,
      /* modifiers */ undefined,
      /* dotDotToken */ undefined,
      "value",
      /* questionToken */ undefined,
      value
    );
    const setter = f.createSetAccessorDeclaration(
      undefined,
      undefined,
      name,
      [param],
      undefined
    );
    return [getter, setter];
  }
}

function createNestedPartial(nested: Member_Nested): [string, ts.TypeNode] {
  const name = nested.getName();
  const targetName = getTypeName(nested.getStructure());
  const value = f.createTypeQueryNode(f.createIdentifier(targetName));
  return [name, value];
}

function createConstantPartial(
  constant: Constant
): [ts.Modifier[], string, ts.TypeNode] {
  const modifiers: ts.Modifier[] = [
    f.createToken(ts.SyntaxKind.StaticKeyword),
    f.createToken(ts.SyntaxKind.ReadonlyKeyword),
  ];
  const name = constant.getName();
  const value = constant.getValue().valueOf();
  const valueNode = f.createLiteralTypeNode(f.createNumericLiteral(value));
  return [modifiers, name, valueNode];
}

function createInterfaceMemberNode(
  member: Member
): ts.TypeElement | ts.TypeElement[] {
  let modifiers: ts.Modifier[];
  let name: string;
  let params: ts.ParameterDeclaration[];
  let result: ts.TypeNode;
  let questionToken: ts.QuestionToken | undefined;

  // noinspection FallThroughInSwitchStatementJS
  switch (member.which()) {
    case Member_Which.METHOD:
      const method = member.getMethod();
      [modifiers, name, params, result] = createMethodPartial(method);
      return f.createMethodSignature(
        modifiers,
        name,
        /* questionToken */ undefined,
        /* typeParams */ undefined,
        params,
        result
      );
    case Member_Which.PROPERTY:
      const prop = member.getProperty();
      if (prop.getPrototype()) {
        return createPrototypeProperty(prop);
      } else {
        [modifiers, name, questionToken, result] =
          createInstancePropertyPartial(prop);
        return f.createPropertySignature(
          modifiers,
          name,
          questionToken,
          result
        );
      }
    case Member_Which.NESTED:
      const nested = member.getNested();
      [name, result] = createNestedPartial(nested);
      return f.createPropertySignature(
        /* modifiers */ undefined,
        name,
        /* questionToken */ undefined,
        result
      );
    case Member_Which.CONSTANT:
      const constant = member.getConstant();
      [modifiers, name, result] = createConstantPartial(constant);
      return f.createPropertySignature(
        modifiers,
        name,
        /* questionToken */ undefined,
        result
      );
    case Member_Which.CONSTRUCTOR:
      assert.fail("Unexpected constructor member inside interface");
    default:
      assert.fail(`Unknown member: ${member.which()}`);
  }
}

function createIteratorInterfaceMemberNode(
  method: Method,
  isAsync: boolean
): ts.TypeElement {
  const [modifiers, name, params, result] = createIteratorMethodPartial(
    method,
    isAsync
  );
  return f.createMethodSignature(
    modifiers,
    name,
    /* questionToken */ undefined,
    /* typeParams */ undefined,
    params,
    result
  );
}

function createClassMemberNode(
  member: Member
): ts.ClassElement | ts.ClassElement[] {
  let modifiers: ts.Modifier[];
  let name: string;
  let params: ts.ParameterDeclaration[];
  let result: ts.TypeNode;
  let questionToken: ts.QuestionToken | undefined;

  switch (member.which()) {
    case Member_Which.METHOD:
      const method = member.getMethod();
      [modifiers, name, params, result] = createMethodPartial(method);
      return f.createMethodDeclaration(
        /* decorators */ undefined,
        modifiers,
        /* asteriskToken */ undefined,
        name,
        /* questionToken */ undefined,
        /* typeParameters */ undefined,
        params,
        result,
        /* body */ undefined
      );
    case Member_Which.PROPERTY:
      const prop = member.getProperty();
      if (prop.getPrototype()) {
        return createPrototypeProperty(prop);
      } else {
        [modifiers, name, questionToken, result] =
          createInstancePropertyPartial(prop);
        return f.createPropertyDeclaration(
          /* decorators */ undefined,
          modifiers,
          name,
          questionToken,
          result,
          /* initialiser */ undefined
        );
      }
    case Member_Which.NESTED:
      const nested = member.getNested();
      [name, result] = createNestedPartial(nested);
      return f.createPropertyDeclaration(
        /* decorators */ undefined,
        /* modifiers */ undefined,
        name,
        /* questionToken */ undefined,
        result,
        /* initialiser */ undefined
      );
    case Member_Which.CONSTANT:
      const constant = member.getConstant();
      [modifiers, name, result] = createConstantPartial(constant);
      return f.createPropertyDeclaration(
        /* decorators */ undefined,
        modifiers,
        name,
        /* questionToken */ undefined,
        result,
        /* initialiser */ undefined
      );
    case Member_Which.CONSTRUCTOR:
      const constructor = member.getConstructor();
      params = createParamDeclarationNodes(constructor.getArgs().toArray());
      return f.createConstructorDeclaration(
        /* decorators */ undefined,
        /* modifiers */ undefined,
        params,
        /* body */ undefined
      );
    default:
      assert.fail(`Unknown member: ${member.which()}`);
  }
}

function createIteratorClassMemberNode(
  method: Method,
  isAsync: boolean
): ts.ClassElement {
  const [modifiers, name, params, result] = createIteratorMethodPartial(
    method,
    isAsync
  );
  return f.createMethodDeclaration(
    /* decorators */ undefined,
    modifiers,
    /* asteriskToken */ undefined,
    name,
    /* questionToken */ undefined,
    /* typeParams */ undefined,
    params,
    result,
    /* body */ undefined
  );
}

// Remove all properties with type `never` and methods with return type `never`
function filterUnimplementedProperties<
  T extends ts.TypeElement | ts.ClassElement
>(members: T[]): T[] {
  return members.filter((member) => {
    // Could collapse these `if` statements, but this is much clearer
    if (
      ts.isPropertySignature(member) ||
      ts.isPropertyDeclaration(member) ||
      ts.isGetAccessorDeclaration(member) ||
      ts.isSetAccessorDeclaration(member) ||
      ts.isMethodSignature(member) ||
      ts.isMethodDeclaration(member)
    ) {
      if (member.type !== undefined && isUnsatisfiable(member.type)) {
        return false;
      }
    }
    return true;
  });
}

export function createStructureNode(structure: Structure, asClass: boolean) {
  const modifiers: ts.Modifier[] = [f.createToken(ts.SyntaxKind.ExportKeyword)];
  const name = getTypeName(structure);

  const heritage: ts.HeritageClause[] = [];
  if (structure.hasExtends()) {
    const typeNode = createTypeNode(structure.getExtends());
    assert(
      ts.isTypeReferenceNode(typeNode) && ts.isIdentifier(typeNode.typeName),
      `Expected type reference, got "${printNode(typeNode)}"`
    );
    const expr = f.createExpressionWithTypeArguments(
      typeNode.typeName,
      typeNode.typeArguments
    );
    heritage.push(f.createHeritageClause(ts.SyntaxKind.ExtendsKeyword, [expr]));
  }

  const members = structure.getMembers();
  if (asClass) {
    modifiers.push(f.createToken(ts.SyntaxKind.DeclareKeyword));
    // Can't use `flatMap()` here as `members` is a `capnp.List`
    const classMembers = members.map(createClassMemberNode).flat();

    const constructorIndex = classMembers.findIndex((member) =>
      ts.isConstructorDeclaration(member)
    );
    if (constructorIndex === -1) {
      // If this class doesn't have a constructor, it must be `abstract`, as we
      // never rely on the implicit default constructor. If a class can be
      // constructed using the empty constructor, it always defines it.
      modifiers.push(f.createToken(ts.SyntaxKind.AbstractKeyword));
    } else {
      // Otherwise, ensure that the constructor always comes first
      classMembers.unshift(...classMembers.splice(constructorIndex, 1));
    }

    // Add iterator members
    if (structure.hasIterator()) {
      const iterator = structure.getIterator();
      classMembers.push(createIteratorClassMemberNode(iterator, false));
    }
    if (structure.hasAsyncIterator()) {
      const iterator = structure.getAsyncIterator();
      classMembers.push(createIteratorClassMemberNode(iterator, true));
    }

    return f.createClassDeclaration(
      /* decorators */ undefined,
      modifiers,
      name,
      /* typeParams */ undefined,
      heritage,
      filterUnimplementedProperties(classMembers)
    );
  } else {
    // Can't use `flatMap()` here as `members` is a `capnp.List`
    const interfaceMembers = members.map(createInterfaceMemberNode).flat();

    // Add iterator members
    if (structure.hasIterator()) {
      const iterator = structure.getIterator();
      interfaceMembers.push(createIteratorInterfaceMemberNode(iterator, false));
    }
    if (structure.hasAsyncIterator()) {
      const iterator = structure.getAsyncIterator();
      interfaceMembers.push(createIteratorInterfaceMemberNode(iterator, true));
    }

    return f.createInterfaceDeclaration(
      /* decorators */ undefined,
      modifiers,
      name,
      /* typeParams */ undefined,
      heritage,
      filterUnimplementedProperties(interfaceMembers)
    );
  }
}
