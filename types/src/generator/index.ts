import assert from "assert";
import {
  FunctionType,
  Member,
  Member_Which,
  Method,
  Structure,
  StructureGroups,
  Type,
  Type_Which,
} from "@workerd/jsg/rtti.capnp.js";
import ts from "typescript";
import { createStructureNode } from "./structure";

type StructureMap = Map<string, Structure>;
// Builds a lookup table mapping type names to structures
function collectStructureMap(root: StructureGroups): StructureMap {
  const map = new Map<string, Structure>();
  root.getGroups().forEach((group) => {
    group.getStructures().forEach((structure) => {
      map.set(structure.getFullyQualifiedName(), structure);
    });
  });
  return map;
}

// Types to visit in `collectIncluded` for finding types to include
// (global scope and bindings types)
// TODO(soon): replace this with a macro like JSG_TS_ROOT or JSG_TS_BINDING_TYPE
const TYPE_ROOTS = [
  "workerd::api::ServiceWorkerGlobalScope",
  "workerd::api::ExportedHandler",
  "workerd::api::DurableObjectNamespace",
  "workerd::api::AnalyticsEngine",
  "workerd::api::KvNamespace",
  "workerd::api::public_beta::R2Bucket",
];

// Builds a set containing the names of structures that should be included
// in the definitions, because they are referenced by root types or any of their
// children.
//
// We need to do this as some types should only be included in the definitions
// when certain compatibility flags are enabled (e.g. `Navigator`,
// standards-compliant `URL`). However, these types are always included in
// the *_TYPES macros.
function collectIncluded(map: StructureMap): Set<string> {
  const included = new Set<string>();

  function visitType(type: Type): void {
    switch (type.which()) {
      case Type_Which.PROMISE:
        return visitType(type.getPromise().getValue());
      case Type_Which.STRUCTURE:
        const name = type.getStructure().getFullyQualifiedName();
        const structure = map.get(name);
        assert(structure !== undefined, `Unknown structure type: ${name}`);
        return visitStructure(structure);
      case Type_Which.ARRAY:
        return visitType(type.getArray().getElement());
      case Type_Which.MAYBE:
        return visitType(type.getMaybe().getValue());
      case Type_Which.DICT:
        const dict = type.getDict();
        visitType(dict.getKey());
        return visitType(dict.getValue());
      case Type_Which.ONE_OF:
        return type.getOneOf().getVariants().forEach(visitType);
      case Type_Which.FUNCTION:
        return visitFunction(type.getFunction());
    }
  }

  function visitFunction(func: FunctionType | Method) {
    func.getArgs().forEach(visitType);
    return visitType(func.getReturnType());
  }

  function visitMember(member: Member) {
    switch (member.which()) {
      case Member_Which.METHOD:
        return visitFunction(member.getMethod());
      case Member_Which.PROPERTY:
        return visitType(member.getProperty().getType());
      case Member_Which.NESTED:
        return visitStructure(member.getNested().getStructure());
      case Member_Which.CONSTRUCTOR:
        return member.getConstructor().getArgs().forEach(visitType);
    }
  }

  function visitStructure(structure: Structure): void {
    const name = structure.getFullyQualifiedName();
    if (included.has(name)) return;
    included.add(name);
    structure.getMembers().forEach(visitMember);
    if (structure.hasExtends()) {
      visitType(structure.getExtends());
    }
    if (structure.hasIterator()) {
      visitFunction(structure.getIterator());
    }
    if (structure.hasAsyncIterator()) {
      visitFunction(structure.getAsyncIterator());
    }
  }

  for (const rootName of TYPE_ROOTS) {
    const root = map.get(rootName);
    assert(root !== undefined, `Unknown root type: ${rootName}`);
    visitStructure(root);
  }

  return included;
}

// Builds a set containing the names of structures that must be declared as
// `class`es rather than `interface`s because they either:
// 1) Get inherited by another class (`class` `extends` requires another `class`)
// 2) Are constructible (`constructor(...)`s can only appear in `class`es)
// 3) Have `static` methods (`static`s can only appear in `class`es)
// 4) Are a nested type (users could call `instanceof` with the type)
function collectClasses(map: StructureMap): Set<string> {
  const classes = new Set<string>();
  for (const structure of map.values()) {
    // 1) Add all classes inherited by this class
    if (structure.hasExtends()) {
      const extendsType = structure.getExtends();
      if (extendsType.isStructure()) {
        classes.add(extendsType.getStructure().getFullyQualifiedName());
      }
    }

    structure.getMembers().forEach((member) => {
      // 2) Add this class if it's constructible
      if (member.isConstructor()) {
        classes.add(structure.getFullyQualifiedName());
      }
      // 3) Add this class if it contains static methods
      if (member.isMethod() && member.getMethod().getStatic()) {
        classes.add(structure.getFullyQualifiedName());
      }
      // 4) Add all nested types defined by this class
      if (member.isNested()) {
        classes.add(member.getNested().getStructure().getFullyQualifiedName());
      }
    });
  }
  return classes;
}

export function generateDefinitions(root: StructureGroups): ts.Node[] {
  const map = collectStructureMap(root);
  const included = collectIncluded(map);
  const classes = collectClasses(map);

  // Can't use `flatMap()` here as `getGroups()` returns a `capnp.List`
  const nodes = root.getGroups().map((group) => {
    const structureNodes: ts.Node[] = [];
    group.getStructures().forEach((structure) => {
      const name = structure.getFullyQualifiedName();
      if (included.has(name)) {
        const asClass = classes.has(name);
        structureNodes.push(createStructureNode(structure, asClass));
      }
    });

    // Add group label to first in group
    if (structureNodes.length > 0) {
      ts.addSyntheticLeadingComment(
        structureNodes[0],
        ts.SyntaxKind.SingleLineCommentTrivia,
        ` ${group.getName()}`,
        /* hasTrailingNewLine */ true
      );
    }

    return structureNodes;
  });

  return nodes.flat();
}
