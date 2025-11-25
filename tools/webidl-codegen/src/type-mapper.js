/**
 * Maps WebIDL types to JSG C++ types
 */

export class TypeMapper {
  constructor(definitions = []) {
    // Build a set of dictionary names so we know which types are structs vs objects
    this.dictionaries = new Set(
      definitions.filter(def => def.type === 'dictionary').map(def => def.name)
    );

    // Build a set of typedef names so we don't wrap them in jsg::Ref
    this.typedefs = new Set(
      definitions.filter(def => def.type === 'typedef').map(def => def.name)
    );

    // Build a set of enum names so we don't wrap them in jsg::Ref
    this.enums = new Set(
      definitions.filter(def => def.type === 'enum').map(def => def.name)
    );

    // Known external enum types from other specs that shouldn't be wrapped
    this.externalEnums = new Set([
      'ReferrerPolicy',  // From Referrer Policy spec
    ]);
  }

  /**
   * Convert a WebIDL type to JSG C++ type
   * @param {object} idlType - WebIDL type object from webidl2
   * @returns {string} C++ type string
   */
  mapType(idlType) {
    if (!idlType) {
      return 'void';
    }

    // If idlType is a string, map it directly
    if (typeof idlType === 'string') {
      return this.mapSimpleType(idlType);
    }

    // Handle nullable types (T?)
    if (idlType.nullable) {
      // Create a copy without nullable flag for recursive mapping
      // Note: Can't use spread operator because idlType.idlType might be non-enumerable
      const innerType = {
        type: idlType.type,
        extAttrs: idlType.extAttrs,
        generic: idlType.generic,
        nullable: false,
        union: idlType.union,
        idlType: idlType.idlType
      };
      const mappedInner = this.mapType(innerType);
      return `kj::Maybe<${mappedInner}>`;
    }

    // Handle union types
    if (idlType.union) {
      const types = idlType.idlType.map(t => this.mapType(t));
      return `kj::OneOf<${types.join(', ')}>`;
    }

    // Handle generic types (Promise, sequence, FrozenArray, record)
    if (idlType.generic) {
      return this.mapGenericType(idlType);
    }

    // Handle primitive and interface types
    return this.mapSimpleType(idlType.idlType);
  }

  /**
   * Map generic types like Promise<T>, sequence<T>, etc.
   */
  mapGenericType(idlType) {
    const generic = idlType.generic;
    const args = idlType.idlType;

    switch (generic) {
      case 'Promise':
        return `jsg::Promise<${this.mapType(args[0])}>`;

      case 'sequence':
        return `kj::Array<${this.mapType(args[0])}>`;

      case 'FrozenArray':
        return `kj::Array<const ${this.mapType(args[0])}>`;

      case 'record': {
        const keyType = this.mapType(args[0]);
        const valueType = this.mapType(args[1]);
        return `jsg::Dict<${keyType}, ${valueType}>`;
      }

      default:
        throw new Error(`Unknown generic type: ${generic}`);
    }
  }

  /**
   * Map simple (non-generic) types
   */
  mapSimpleType(typeName) {
    const typeMap = {
      // Primitive types
      'undefined': 'void',
      'boolean': 'bool',
      'byte': 'int8_t',
      'octet': 'uint8_t',
      'short': 'int16_t',
      'unsigned short': 'uint16_t',
      'long': 'int32_t',
      'unsigned long': 'uint32_t',
      'long long': 'int64_t',
      'unsigned long long': 'uint64_t',
      'float': 'float',
      'double': 'double',
      'unrestricted double': 'double',
      'unrestricted float': 'float',

      // String types
      'DOMString': 'jsg::DOMString',
      'ByteString': 'jsg::ByteString',
      'USVString': 'jsg::USVString',

      // Special types
      'any': 'jsg::JsValue',
      'object': 'jsg::JsObject',
      'SelfRef': 'jsg::SelfRef',
      'BufferSource': 'jsg::BufferSource',
      'ArrayBuffer': 'jsg::BufferSource',
      'DataView': 'jsg::BufferSource',
      'Int8Array': 'jsg::BufferSource',
      'Uint8Array': 'jsg::BufferSource',
      'Uint8ClampedArray': 'jsg::BufferSource',
      'Int16Array': 'jsg::BufferSource',
      'Uint16Array': 'jsg::BufferSource',
      'Int32Array': 'jsg::BufferSource',
      'Uint32Array': 'jsg::BufferSource',
      'Float32Array': 'jsg::BufferSource',
      'Float64Array': 'jsg::BufferSource',
      'BigInt64Array': 'jsg::BufferSource',
      'BigUint64Array': 'jsg::BufferSource',
    };

    if (typeMap[typeName]) {
      return typeMap[typeName];
    }

    // Check if this is a dictionary (struct) - if so, use directly
    if (this.dictionaries.has(typeName)) {
      return typeName;
    }

    // Check if this is a typedef - if so, use directly
    if (this.typedefs.has(typeName)) {
      return typeName;
    }

    // Check if this is an enum - if so, use directly
    if (this.enums.has(typeName)) {
      return typeName;
    }

    // Check if this is a known external enum - if so, use directly
    if (this.externalEnums.has(typeName)) {
      return typeName;
    }

    // Otherwise assume it's an interface type - wrap in jsg::Ref
    return `jsg::Ref<${typeName}>`;
  }

  /**
   * Map function parameter, handling optional parameters
   */
  mapParameter(param) {
    const baseType = this.mapType(param.idlType);

    if (param.optional) {
      return `jsg::Optional<${baseType}>`;
    }

    return baseType;
  }

  /**
   * Map a dictionary member type, handling optional and nullable
   * Rules:
   * - optional (can be undefined or T) -> jsg::Optional<T>
   * - nullable (can be null or T) -> kj::Maybe<T>
   * - optional + nullable (can be undefined, null, or T) -> jsg::Optional<kj::Maybe<T>>
   * @param {object} member - Dictionary member object from webidl2
   * @returns {string} C++ type string
   */
  mapDictionaryMemberType(member) {
    const isOptional = member.required === false;  // WebIDL members are optional by default
    const isNullable = member.idlType.nullable;

    // Get base type without nullable wrapper (we'll add it back correctly)
    // Note: Can't use spread operator because idlType.idlType is a non-enumerable property
    const baseIdlType = isNullable
      ? { ...member.idlType, nullable: false, idlType: member.idlType.idlType }
      : member.idlType;
    const baseType = this.mapType(baseIdlType);

    // Special case: SelfRef is never wrapped in Optional or Maybe
    // It's always jsg::SelfRef directly, as seen in workerd's api/basics.h and api/global-scope.h
    const rawType = baseIdlType.idlType || baseIdlType;
    if (rawType === 'SelfRef') {
      return baseType;
    }

    // Both optional and nullable: jsg::Optional<kj::Maybe<T>>
    if (isOptional && isNullable) {
      return `jsg::Optional<kj::Maybe<${baseType}>>`;
    }

    // Just nullable: kj::Maybe<T>
    if (isNullable) {
      return `kj::Maybe<${baseType}>`;
    }

    // Just optional: jsg::Optional<T>
    if (isOptional) {
      return `jsg::Optional<${baseType}>`;
    }

    // Required (neither optional nor nullable): T
    return baseType;
  }

  /**
   * Check if a type is a primitive value type (not passed by reference)
   */
  isPrimitiveType(cppType) {
    const primitives = [
      'void', 'bool',
      'int8_t', 'uint8_t', 'int16_t', 'uint16_t',
      'int32_t', 'uint32_t', 'int64_t', 'uint64_t',
      'float', 'double'
    ];

    return primitives.some(p => cppType.startsWith(p));
  }

  /**
   * Convert a WebIDL type to a string representation (for stub generation)
   * This is similar to mapType but returns the string version
   */
  mapTypeToString(idlType) {
    return this.mapType(idlType);
  }
}
