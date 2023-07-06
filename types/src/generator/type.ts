// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import {
  ArrayType,
  BuiltinType_Type,
  JsgImplType_Type,
  MaybeType,
  NumberType,
  Structure,
  StructureType,
  Type,
  Type_Which,
} from "@workerd/jsg/rtti.capnp.js";
import ts, { factory as f } from "typescript";
import { printNode } from "../print";
import { getParameterName } from "./parameter-names";

// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Array/findLastIndex
export function findLastIndex<T>(
  array: T[],
  predicate: (value: T, index: number, array: T[]) => unknown
) {
  for (let i = array.length - 1; i >= 0; i--) {
    if (predicate(array[i], i, array)) return i;
  }
  return -1;
}

// If `typeNode` has the shape `T | undefined`, returns `T`, otherwise returns
// `undefined`.
export function maybeUnwrapOptional(
  typeNode: ts.TypeNode
): ts.TypeNode | undefined {
  if (
    ts.isUnionTypeNode(typeNode) &&
    typeNode.types.length === 2 &&
    ts.isTypeReferenceNode(typeNode.types[1]) &&
    ts.isIdentifier(typeNode.types[1].typeName) &&
    typeNode.types[1].typeName.escapedText === "undefined"
  ) {
    return typeNode.types[0];
  }
}

// Returns `true` iff this maybe type represents `T | null`, not `T | undefined`
function isNullMaybe(maybe: MaybeType) {
  // https://github.com/cloudflare/workerd/blob/33e692f2216704b7226c8c59b1455eefedf79068/src/workerd/jsg/jsg.h#L220-L221
  return maybe.getName() === "kj::Maybe";
}

// Returns `true` iff this number type represents a byte
function isByteNumber(number: NumberType) {
  // https://github.com/cloudflare/workerd/blob/33e692f2216704b7226c8c59b1455eefedf79068/src/workerd/jsg/rtti.h#L160
  const name = number.getName();
  return name === "char" || name === "unsigned char";
}

// Returns `true` iff this number type represents `number | bigint`
function isBigNumber(number: NumberType) {
  // https://github.com/cloudflare/workerd/blob/33e692f2216704b7226c8c59b1455eefedf79068/src/workerd/jsg/README.md?plain=1#L56-L82
  // https://github.com/cloudflare/workerd/blob/33e692f2216704b7226c8c59b1455eefedf79068/src/workerd/jsg/rtti.h#L157-L167
  const name = number.getName();
  return (
    name === "long" ||
    name === "unsigned long" ||
    name === "long long" ||
    name === "unsigned long long"
  );
}

// Returns `true` iff this array type represents a pointer to an array
function isArrayPointer(array: ArrayType) {
  return array.getName() === "kj::ArrayPtr";
}

// Returns `true` iff this array type represents an iterable
function isIterable(array: ArrayType) {
  // https://github.com/cloudflare/workerd/blob/33e692f2216704b7226c8c59b1455eefedf79068/src/workerd/jsg/README.md?plain=1#L185-L186
  return array.getName() === "jsg::Sequence";
}

// Returns `true` iff `typeNode` is `never`
export function isUnsatisfiable(typeNode: ts.TypeNode) {
  const isNeverTypeReference =
    ts.isTypeReferenceNode(typeNode) &&
    ts.isIdentifier(typeNode.typeName) &&
    typeNode.typeName.text === "never";
  const isNeverKeyword =
    ts.isToken(typeNode) && typeNode.kind == ts.SyntaxKind.NeverKeyword;
  return isNeverTypeReference || isNeverKeyword;
}

// Strings to replace in fully-qualified structure names with nothing
const replaceEmpty =
  /^workerd::api::public_beta::|^workerd::api::|^workerd::jsg::|::|[ >]/g;
// Strings to replace in fully-qualified structure names with an underscore
const replaceUnderscore = /[<,]/g;
export function getTypeName(structure: Structure | StructureType): string {
  let name = structure.getFullyQualifiedName();
  name = name.replace(replaceEmpty, "");
  name = name.replace(replaceUnderscore, "_");
  return name;
}

export function createParamDeclarationNodes(
  fullyQualifiedParentName: string,
  name: string,
  args: Type[],
  forMethod = false
): ts.ParameterDeclaration[] {
  // Find the index of the last required parameter, all optional before this
  // will use the `| undefined` syntax, as opposed to a `?` token.
  const lastRequiredParameter = findLastIndex(args, (type) => {
    // Could simplify this to a single return, but this reads clearer
    if (type.isMaybe() && !isNullMaybe(type.getMaybe())) {
      // `type` is `T | undefined` so optional
      return false;
    }
    // noinspection RedundantIfStatementJS
    if (type.isJsgImpl()) {
      // `type` is varargs or internal implementation type so optional
      return false;
    }
    return true;
  });

  // `args` may include internal implementation types that shouldn't appear
  // in parameters. Therefore, we may end up with fewer params than args.
  const params: ts.ParameterDeclaration[] = [];

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    let typeNode = createTypeNode(
      arg,
      true, // Always allow coercion in function params
      forMethod // Allow additional coercion in method params
    );

    let dotDotDotToken: ts.DotDotDotToken | undefined;
    let questionToken: ts.QuestionToken | undefined;

    const which = arg.which();
    if (which === Type_Which.MAYBE) {
      // If this is an optional type, and we don't have any required args
      // left, use an optional parameter with a `?`
      const unwrappedTypeNode = maybeUnwrapOptional(typeNode);
      if (unwrappedTypeNode !== undefined && i > lastRequiredParameter) {
        typeNode = unwrappedTypeNode;
        questionToken = f.createToken(ts.SyntaxKind.QuestionToken);
      }
    } else if (which === Type_Which.JSG_IMPL) {
      if (arg.getJsgImpl().getType() === JsgImplType_Type.JSG_VARARGS) {
        // If this is a varargs type, make sure we include `...`
        assert(
          ts.isArrayTypeNode(typeNode),
          `Expected "T[]", got "${printNode(typeNode)}"`
        );
        dotDotDotToken = f.createToken(ts.SyntaxKind.DotDotDotToken);
      } else {
        // If this is an internal implementation type, omit it, and skip to
        // the next arg
        assert(
          isUnsatisfiable(typeNode),
          `Expected "never", got "${printNode(typeNode)}"`
        );
        continue;
      }
    }

    const param = f.createParameterDeclaration(
      /* decorators */ undefined,
      /* modifiers */ undefined,
      dotDotDotToken,
      getParameterName(fullyQualifiedParentName, name, i),
      questionToken,
      typeNode
    );
    params.push(param);
  }

  return params;
}

export function createTypeNode(
  type: Type,
  allowCoercion = false,
  allowMethodParameterCoercion = false
): ts.TypeNode {
  // If `allowMethodParameterCoercion` is set, `allowCoercion` must be set too.
  // `allowMethodParameterCoercion` enables additional coercions for C++ method
  // parameters.
  assert(
    !allowMethodParameterCoercion || allowCoercion,
    `"allowMethodParameterCoercion" requires "allowCoercion"`
  );

  // noinspection FallThroughInSwitchStatementJS
  switch (type.which()) {
    case Type_Which.UNKNOWN:
      return f.createTypeReferenceNode("any");
    case Type_Which.VOIDT:
      return f.createTypeReferenceNode("void");
    case Type_Which.BOOLT:
      return f.createTypeReferenceNode("boolean");
    case Type_Which.NUMBER:
      const number = type.getNumber();
      if (isBigNumber(number)) {
        return f.createUnionTypeNode([
          f.createTypeReferenceNode("number"),
          f.createTypeReferenceNode("bigint"),
        ]);
      } else {
        return f.createTypeReferenceNode("number");
      }
    case Type_Which.PROMISE:
      const value = type.getPromise().getValue();

      if (allowMethodParameterCoercion && value.which() === Type_Which.VOIDT) {
        // For C++ method parameters, treat `Promise<void>` as `Promise<any>`.
        // We don't use `allowCoercion` here, as we want stream callback return
        // types to be `Promise<void>` so they match official TypeScript types:
        // https://github.com/microsoft/TypeScript/blob/f1288c33a1594046dcb4bad2ecdda80a1b035bb7/lib/lib.webworker.d.ts#L5987-L6025
        return f.createTypeReferenceNode("Promise", [
          f.createTypeReferenceNode("any"),
        ]);
      }

      const valueType = createTypeNode(value, allowCoercion);
      const promiseType = f.createTypeReferenceNode("Promise", [valueType]);
      if (allowCoercion) {
        return f.createUnionTypeNode([valueType, promiseType]);
      } else {
        return promiseType;
      }
    case Type_Which.STRUCTURE:
      return f.createTypeReferenceNode(getTypeName(type.getStructure()));
    case Type_Which.STRING:
      return f.createTypeReferenceNode("string");
    case Type_Which.OBJECT:
      return f.createTypeReferenceNode("any");
    case Type_Which.ARRAY:
      const array = type.getArray();
      const element = array.getElement();
      if (element.isNumber() && isByteNumber(element.getNumber())) {
        // If the array element is a `byte`...
        if (allowCoercion) {
          // When coercion is enabled (e.g. method param), `kj::Array<byte>` and
          // `kj::ArrayPtr<byte>` both mean `ArrayBuffer | ArrayBufferView`
          return f.createUnionTypeNode([
            f.createTypeReferenceNode("ArrayBuffer"),
            f.createTypeReferenceNode("ArrayBufferView"),
          ]);
        } else {
          // When coercion is disabled, `kj::ArrayPtr<byte>` corresponds to
          // `ArrayBufferView`, whereas `kj::Array<byte>` is `ArrayBuffer`
          return f.createTypeReferenceNode(
            isArrayPointer(array) ? "ArrayBufferView" : "ArrayBuffer"
          );
        }
      } else if (isIterable(array) && allowCoercion) {
        // If this is a `jsg::Sequence` parameter, it should accept any iterable
        return f.createTypeReferenceNode("Iterable", [
          createTypeNode(element, allowCoercion),
        ]);
      } else {
        // Otherwise, return a regular array
        return f.createArrayTypeNode(createTypeNode(element, allowCoercion));
      }
    case Type_Which.MAYBE:
      const maybe = type.getMaybe();
      const alternative = isNullMaybe(maybe) ? "null" : "undefined";
      return f.createUnionTypeNode([
        createTypeNode(maybe.getValue(), allowCoercion),
        f.createTypeReferenceNode(alternative),
      ]);
    case Type_Which.DICT:
      const dict = type.getDict();
      return f.createTypeReferenceNode("Record", [
        createTypeNode(dict.getKey(), allowCoercion),
        createTypeNode(dict.getValue(), allowCoercion),
      ]);
    case Type_Which.ONE_OF:
      const variants = type
        .getOneOf()
        .getVariants()
        .map((variant) => createTypeNode(variant, allowCoercion));
      return f.createUnionTypeNode(variants);
    case Type_Which.BUILTIN:
      const builtin = type.getBuiltin().getType();
      switch (builtin) {
        case BuiltinType_Type.V8UINT8ARRAY:
          return f.createTypeReferenceNode("Uint8Array");
        case BuiltinType_Type.V8ARRAY_BUFFER_VIEW:
          return f.createTypeReferenceNode("ArrayBufferView");
        case BuiltinType_Type.JSG_BUFFER_SOURCE:
          return f.createUnionTypeNode([
            f.createTypeReferenceNode("ArrayBuffer"),
            f.createTypeReferenceNode("ArrayBufferView"),
          ]);
        case BuiltinType_Type.KJ_DATE:
          if (allowCoercion) {
            return f.createUnionTypeNode([
              f.createTypeReferenceNode("number"),
              f.createTypeReferenceNode("Date"),
            ]);
          } else {
            return f.createTypeReferenceNode("Date");
          }
        case BuiltinType_Type.V8FUNCTION:
          return f.createTypeReferenceNode("Function");
        default:
          assert.fail(`Unknown builtin type: ${builtin}`);
      }
    case Type_Which.INTRINSIC:
      const intrinsic = type.getIntrinsic().getName();
      switch (intrinsic) {
        case "v8::kErrorPrototype":
          return f.createTypeReferenceNode("Error");
        case "v8::kIteratorPrototype":
          return f.createTypeReferenceNode("Iterator", [
            f.createTypeReferenceNode("unknown"),
          ]);
        case "v8::kAsyncIteratorPrototype":
          return f.createTypeReferenceNode("AsyncIterator", [
            f.createTypeReferenceNode("unknown"),
          ]);
        default:
          assert.fail(`Unknown intrinsic type: ${intrinsic}`);
      }
    case Type_Which.FUNCTION:
      const func = type.getFunction();
      const params = createParamDeclarationNodes(
        "FUNCTION_TODO",
        "FUNCTION_TODO",
        func.getArgs().toArray()
      );
      const result = createTypeNode(
        func.getReturnType(),
        true // Always allow coercion in callback functions
      );
      return f.createFunctionTypeNode(
        /* typeParams */ undefined,
        params,
        result
      );
    case Type_Which.JSG_IMPL:
      const impl = type.getJsgImpl().getType();
      switch (impl) {
        case JsgImplType_Type.CONFIGURATION:
        case JsgImplType_Type.V8ISOLATE:
        case JsgImplType_Type.JSG_LOCK:
        case JsgImplType_Type.JSG_TYPE_HANDLER:
        case JsgImplType_Type.JSG_UNIMPLEMENTED:
        case JsgImplType_Type.JSG_SELF_REF:
        case JsgImplType_Type.V8FUNCTION_CALLBACK_INFO:
        case JsgImplType_Type.V8PROPERTY_CALLBACK_INFO:
          // All these types should be omitted from function parameters
          return f.createTypeReferenceNode("never");
        case JsgImplType_Type.JSG_VARARGS:
          return f.createArrayTypeNode(f.createTypeReferenceNode("any"));
        case JsgImplType_Type.JSG_NAME:
          return f.createTypeReferenceNode("PropertyKey");
        default:
          assert.fail(`Unknown JSG implementation type: ${impl}`);
      }
    default:
      assert.fail(`Unknown type: ${type.which()}`);
  }
}
