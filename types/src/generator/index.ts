// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert'
import {
  type FunctionType,
  type Member,
  Member_Which,
  type Method,
  type Structure,
  type StructureGroups,
  type Type,
  Type_Which,
} from '@workerd/jsg/rtti'
import type ts from 'typescript'
import { createStructureNode } from './structure'

export { getTypeName } from './type'

export type StructureMap = Map<string, Structure>
// Builds a lookup table mapping type names to structures
function collectStructureMap(root: StructureGroups): StructureMap {
  const map = new Map<string, Structure>()
  root.groups.forEach((group) => {
    group.structures.forEach((structure) => {
      map.set(structure.fullyQualifiedName, structure)
    })
  })
  return map
}

// Builds a set containing the names of structures that should be included
// in the definitions, because they are referenced by root types or any of their
// children. A struct/resource type is marked as a root type using a
// `JSG_(STRUCT_)TS_ROOT` macro.
//
// We need to do this as some types should only be included in the definitions
// when certain compatibility flags are enabled (e.g. `Navigator`,
// standards-compliant `URL`). However, these types are always included in
// the `*_TYPES` macros.
function collectIncluded(map: StructureMap, root?: string): Set<string> {
  const included = new Set<string>()

  function visitType(type: Type): void {
    switch (type.which()) {
      case Type_Which.PROMISE: {
        visitType(type.promise.value)
        return
      }
      case Type_Which.STRUCTURE: {
        const name = type.structure.fullyQualifiedName
        const structure = map.get(name)
        assert(structure !== undefined, `Unknown structure type: ${name}`)
        visitStructure(structure)
        return
      }
      case Type_Which.ARRAY: {
        visitType(type.array.element)
        return
      }
      case Type_Which.MAYBE: {
        visitType(type.maybe.value)
        return
      }
      case Type_Which.DICT: {
        const dict = type.dict
        visitType(dict.key)
        visitType(dict.value)
        return
      }
      case Type_Which.ONE_OF: {
        type.oneOf.variants.forEach(visitType)
        return
      }
      case Type_Which.FUNCTION: {
        visitFunction(type.function)
        return
      }
    }
  }

  function visitFunction(func: FunctionType | Method): void {
    func.args.forEach(visitType)
    visitType(func.returnType)
  }

  function visitMember(member: Member): void {
    switch (member.which()) {
      case Member_Which.METHOD: {
        visitFunction(member.method)
        return
      }
      case Member_Which.PROPERTY: {
        visitType(member.property.type)
        return
      }
      case Member_Which.NESTED: {
        visitStructure(member.nested.structure)
        return
      }
      case Member_Which.CONSTRUCTOR: {
        member.$constructor.args.forEach(visitType)
        return
      }
    }
  }

  function visitStructure(structure: Structure): void {
    const name = structure.fullyQualifiedName
    if (included.has(name)) return
    included.add(name)
    structure.members.forEach(visitMember)
    if (structure._hasExtends()) {
      visitType(structure.extends)
    }
    if (structure._hasIterator()) {
      visitFunction(structure.iterator)
    }
    if (structure._hasAsyncIterator()) {
      visitFunction(structure.asyncIterator)
    }
  }

  if (root === undefined) {
    // If no root was specified, visit all structures with
    // `JSG_(STRUCT_)TS_ROOT` macros
    for (const structure of map.values()) {
      if (structure.tsRoot) visitStructure(structure)
    }
  } else {
    // Otherwise, visit just that root
    const structure = map.get(root)
    assert(structure !== undefined, `Unknown root: ${root}`)
    visitStructure(structure)
  }

  return included
}

// Builds a set containing the names of structures that must be declared as
// `class`es rather than `interface`s because they either:
// 1) Get inherited by another class (`class` `extends` requires another `class`)
// 2) Are constructible (`constructor(...)`s can only appear in `class`es)
// 3) Have `static` methods (`static`s can only appear in `class`es)
// 4) Are a nested type (users could call `instanceof` with the type)
function collectClasses(map: StructureMap): Set<string> {
  const classes = new Set<string>()
  for (const structure of map.values()) {
    // 1) Add all classes inherited by this class
    if (structure._hasExtends()) {
      const extendsType = structure.extends
      if (extendsType._isStructure) {
        classes.add(extendsType.structure.fullyQualifiedName)
      }
    }

    structure.members.forEach((member) => {
      // 2) Add this class if it's constructible
      if (member._isConstructor) {
        classes.add(structure.fullyQualifiedName)
      }
      // 3) Add this class if it contains static methods
      if (member._isMethod && member.method.static) {
        classes.add(structure.fullyQualifiedName)
      }
      // 4) Add all nested types defined by this class
      if (member._isNested) {
        classes.add(member.nested.structure.fullyQualifiedName)
      }
    })
  }
  return classes
}

export function generateDefinitions(root: StructureGroups): {
  nodes: ts.Statement[]
  structureMap: StructureMap
} {
  const structureMap = collectStructureMap(root)
  const globalIncluded = collectIncluded(structureMap)
  const classes = collectClasses(structureMap)

  // Can't use `flatMap()` here as `getGroups()` returns a `capnp.List`
  const nodes = root.groups.map((group) => {
    const structureNodes: ts.Statement[] = []
    group.structures.forEach((structure) => {
      const name = structure.fullyQualifiedName
      if (globalIncluded.has(name)) {
        const asClass = classes.has(name)
        structureNodes.push(createStructureNode(structure, { asClass }))
      }
    })
    return structureNodes
  })
  const flatNodes = nodes.flat()

  return { nodes: flatNodes, structureMap }
}

export function collectTypeScriptModules(root: StructureGroups): string {
  let result = ''

  root.modules.forEach((module) => {
    if (!module._isTsDeclarations) return
    const declarations = module.tsDeclarations
      // Looks for any lines starting with `///`, which indicates a TypeScript
      // Triple-Slash Directive (https://www.typescriptlang.org/docs/handbook/triple-slash-directives.html)
      .replaceAll(/^\/\/\/.+$/gm, (match) => {
        assert.strictEqual(
          match,
          '/// <reference types="@workerd/types-internal" />',
          `Unexpected triple-slash directive, got ${match}`,
        )
        return ''
      })

    result += `declare module "${module.specifier}" {\n${declarations}\n}\n`
  })

  return result
}
