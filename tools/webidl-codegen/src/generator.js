/**
 * C++ code generator for JSG bindings from WebIDL
 */

import { TypeMapper } from './type-mapper.js';
import { ProtectedRegions } from './protected-regions.js';

export class CppGenerator {
  constructor(options = {}) {
    this.typeMapper = new TypeMapper();
    this.protectedRegions = null; // Set via setProtectedRegions() if preserving content
    // List of interfaces to skip (already defined in workerd)
    this.skipInterfaces = new Set(options.skipInterfaces || [
      'Event',
      'EventTarget',
      'EventListener',
      'AbortSignal',
      'AbortController',
    ]);
    // Mixin tracking
    this.mixins = new Map(); // name -> mixin definition
    this.includesMap = new Map(); // interface name -> [mixin names]
    // Generation summary tracking
    this.summary = {
      interfaces: [],
      mixins: [],
      dictionaries: [],
      enums: [],
      typedefs: [],
      callbacks: [],
      skipped: [],
      unsupported: [],
    };
  }

  /**
   * Set protected regions from existing file
   */
  setProtectedRegions(protectedRegions) {
    this.protectedRegions = protectedRegions;
  }

  /**
   * Get generation summary
   */
  getSummary() {
    return this.summary;
  }

  /**
   * Extract compat flag from WebIDL extended attribute
   * Supports [JsgCompatFlag=FlagName] or [JsgCompatFlag="FlagName"]
   */
  getCompatFlag(extAttrs) {
    if (!extAttrs) return null;

    const compatAttr = extAttrs.find(attr => attr.name === 'JsgCompatFlag');
    if (!compatAttr) return null;

    // Handle [CompatFlag=FlagName] or [CompatFlag="FlagName"]
    if (compatAttr.rhs) {
      return compatAttr.rhs.value || compatAttr.rhs;
    }

    return null;
  }

  /**
   * Check if method should be in the else block (when compat flag is OFF)
   * Supports [JsgCompatFlagOff=FlagName]
   */
  getCompatFlagOff(extAttrs) {
    if (!extAttrs) return null;

    const compatAttr = extAttrs.find(attr => attr.name === 'JsgCompatFlagOff');
    if (!compatAttr) return null;

    // Handle [CompatFlagOff=FlagName]
    if (compatAttr.rhs) {
      return compatAttr.rhs.value || compatAttr.rhs;
    }

    return null;
  }

  /**
   * Extract custom method name from WebIDL extended attribute
   * Supports [JsgMethodName=customName] or [JsgMethodName="customName"]
   */
  getMethodName(extAttrs) {
    if (!extAttrs) return null;

    const methodNameAttr = extAttrs.find(attr => attr.name === 'JsgMethodName');
    if (!methodNameAttr) return null;

    // Handle [MethodName=name] or [MethodName="name"]
    if (methodNameAttr.rhs) {
      return methodNameAttr.rhs.value || methodNameAttr.rhs;
    }

    return null;
  }

  /**
   * Extract TypeScript override from WebIDL extended attribute
   * Supports [JsgTsOverride="typescript code"] for JSG_TS_OVERRIDE
   */
  getTsOverride(extAttrs) {
    if (!extAttrs) return null;

    const tsOverrideAttr = extAttrs.find(attr => attr.name === 'JsgTsOverride');
    if (!tsOverrideAttr) return null;

    // Handle [TsOverride="..."]
    if (tsOverrideAttr.rhs) {
      return tsOverrideAttr.rhs.value || tsOverrideAttr.rhs;
    }

    return null;
  }

  /**
   * Extract TypeScript definitions from WebIDL extended attribute
   * Supports [JsgTsDefine="typescript code"] for JSG_TS_DEFINE
   */
  getTsDefine(extAttrs) {
    if (!extAttrs) return null;

    const tsDefineAttr = extAttrs.find(attr => attr.name === 'JsgTsDefine');
    if (!tsDefineAttr) return null;

    // Handle [TsDefine="..."]
    if (tsDefineAttr.rhs) {
      return tsDefineAttr.rhs.value || tsDefineAttr.rhs;
    }

    return null;
  }

  /**
   * Extract property scope from WebIDL extended attribute
   * Supports [JsgPropertyScope=instance] or [JsgPropertyScope=prototype]
   * Used to control JSG_*_INSTANCE_PROPERTY vs JSG_*_PROTOTYPE_PROPERTY
   */
  getPropertyScope(extAttrs) {
    if (!extAttrs) return null;

    const scopeAttr = extAttrs.find(attr => attr.name === 'JsgPropertyScope');
    if (!scopeAttr) return null;

    // Handle [PropertyScope=instance] or [PropertyScope=prototype]
    if (scopeAttr.rhs) {
      const value = scopeAttr.rhs.value || scopeAttr.rhs;
      return value === 'instance' || value === 'prototype' ? value : null;
    }

    return null;
  }

  /**
   * Check if a dictionary member should be excluded from JSG_STRUCT parameters
   * Returns true for fields marked with [JsgInternal] or special types like SelfRef
   * These fields exist in the C++ struct but aren't exposed to the type wrapper
   */
  isJsgInternalField(member) {
    // Check for [JsgInternal] extended attribute
    if (member.extAttrs && member.extAttrs.some(attr => attr.name === 'JsgInternal')) {
      return true;
    }

    // SelfRef, Unimplemented, and WontImplement are always internal
    const baseType = member.idlType.idlType || member.idlType;
    return baseType === 'SelfRef' || baseType === 'Unimplemented' || baseType === 'WontImplement';
  }

  /**
   * Get custom C++ code from [JsgCode] attribute
   * Supports [JsgCode="..."] for adding constructors, methods, etc.
   */
  getJsgCode(extAttrs) {
    if (!extAttrs) return null;

    const jsgCodeAttr = extAttrs.find(attr => attr.name === 'JsgCode');
    if (!jsgCodeAttr) return null;

    // Handle [JsgCode="..."]
    if (jsgCodeAttr.rhs) {
      let code = jsgCodeAttr.rhs.value || jsgCodeAttr.rhs;

      // The WebIDL parser returns the string with outer quotes included
      // Strip them if present
      if (typeof code === 'string' && code.length >= 2) {
        if ((code.startsWith('"') && code.endsWith('"')) ||
            (code.startsWith("'") && code.endsWith("'"))) {
          code = code.slice(1, -1);
        }
      }

      return code;
    }

    return null;
  }  /**
   * Check if dictionary should be a TypeScript root type
   * Supports [JsgTsRoot] for JSG_STRUCT_TS_ROOT
   */
  getTsRoot(extAttrs) {
    if (!extAttrs) return false;
    return extAttrs.some(attr => attr.name === 'JsgTsRoot');
  }

  /**
   * Generate complete C++ header from parsed WebIDL
   * @param {Array} definitions - Parsed WebIDL definitions from webidl2
   * @param {object} options - Generation options
   * @returns {string} Generated C++ header code
   */
  generate(definitions, options = {}) {
    // Store definitions for inheritance lookups
    this.definitions = definitions;

    // Reset summary
    this.summary = {
      interfaces: [],
      mixins: [],
      dictionaries: [],
      enums: [],
      typedefs: [],
      callbacks: [],
      skipped: [],
      unsupported: [],
    };

    // Update type mapper with definitions so it can distinguish dictionaries from interfaces
    this.typeMapper = new TypeMapper(definitions);

    // Process mixins and includes statements
    this.processMixinsAndIncludes(definitions);

    const { namespace = 'workerd::api', includeGuard } = options;

    let code = '';

    // Header guard
    if (includeGuard !== false) {
      const guard = this.makeIncludeGuard(options.filename || 'generated');
      code += `#pragma once\n`;
      code += `// Generated from WebIDL - DO NOT EDIT\n\n`;
    }

    // Includes
    code += this.generateIncludes();

    // Namespace (use :: notation for nested namespaces)
    code += `namespace ${namespace} {\n\n`;

    // Generate forward declarations for undefined types
    code += this.generateForwardDeclarations(definitions);

    // Sort definitions by dependency order:
    // 1. typedefs (may be used by other types)
    // 2. enums (simple value types)
    // 3. callbacks (function types)
    // 4. dictionaries (value types)
    // 5. mixins (base classes)
    // 6. interfaces (complex types that may use all of the above)
    const typeOrder = {
      'typedef': 1,
      'enum': 2,
      'callback': 3,
      'dictionary': 4,
      'interface mixin': 5,
      'interface': 6,
      'includes': 7
    };

    const sortedDefinitions = [...definitions].sort((a, b) => {
      const orderA = typeOrder[a.type] || 999;
      const orderB = typeOrder[b.type] || 999;
      return orderA - orderB;
    });

    // Generate code for each definition
    for (const def of sortedDefinitions) {
      // Skip interfaces/mixins that are already defined in workerd or explicitly excluded
      if ((def.type === 'interface' || def.type === 'interface mixin') && this.skipInterfaces.has(def.name)) {
        this.summary.skipped.push({ type: def.type === 'interface mixin' ? 'mixin' : 'interface', name: def.name });
        code += `// ${def.name} is defined in workerd (skipped)\n\n`;
        continue;
      }

      code += this.generateDefinition(def);
      code += '\n';
    }

    // Close namespace
    code += `\n}  // namespace ${namespace}\n`;

    return code;
  }

  /**
   * Process mixin definitions and includes statements
   */
  processMixinsAndIncludes(definitions) {
    // First pass: collect all mixins
    for (const def of definitions) {
      if (def.type === 'interface mixin') {
        this.mixins.set(def.name, def);
      }
    }

    // Second pass: collect includes statements
    for (const def of definitions) {
      if (def.type === 'includes') {
        if (!this.includesMap.has(def.target)) {
          this.includesMap.set(def.target, []);
        }
        this.includesMap.get(def.target).push(def.includes);
      }
    }
  }

  generateIncludes() {
    return `#include <workerd/jsg/jsg.h>
#include <kj/string.h>
#include <kj/array.h>

// Note: Base classes like Event, EventTarget may need additional includes:
// #include <workerd/api/basics.h>

`;
  }

  /**
   * Generate forward declarations for types referenced but not defined in this file,
   * plus interfaces defined in this file (to handle forward references in typedefs)
   */
  generateForwardDeclarations(definitions) {
    // Collect interface names that will be defined in this file
    const localInterfaces = new Set();
    for (const def of definitions) {
      if ((def.type === 'interface' || def.type === 'interface mixin') && def.name) {
        localInterfaces.add(def.name);
      }
    }

    // Collect all non-interface types defined in this file (enums, typedefs, dictionaries)
    // These should NOT be forward declared
    const localNonInterfaces = new Set();
    for (const def of definitions) {
      if (def.name && def.type !== 'interface' && def.type !== 'interface mixin') {
        localNonInterfaces.add(def.name);
      }
    }

    // Collect all referenced types
    const referencedTypes = new Set();

    const extractTypesFromIdlType = (idlType) => {
      if (!idlType) return;

      if (typeof idlType === 'string') {
        referencedTypes.add(idlType);
        return;
      }

      if (idlType.union && Array.isArray(idlType.idlType)) {
        idlType.idlType.forEach(extractTypesFromIdlType);
        return;
      }

      if (idlType.generic && Array.isArray(idlType.idlType)) {
        idlType.idlType.forEach(extractTypesFromIdlType);
        return;
      }

      if (idlType.idlType) {
        extractTypesFromIdlType(idlType.idlType);
      }
    };

    for (const def of definitions) {
      // Extract from interface members
      if (def.type === 'interface' || def.type === 'interface mixin') {
        // Check inheritance
        if (def.inheritance) {
          referencedTypes.add(def.inheritance);
        }

        // Check members
        for (const member of def.members || []) {
          if (member.idlType) {
            extractTypesFromIdlType(member.idlType);
          }
          // Check arguments
          if (member.arguments) {
            for (const arg of member.arguments) {
              if (arg.idlType) {
                extractTypesFromIdlType(arg.idlType);
              }
            }
          }
        }
      }

      // Extract from dictionary members
      if (def.type === 'dictionary') {
        if (def.inheritance) {
          referencedTypes.add(def.inheritance);
        }
        for (const member of def.members || []) {
          if (member.idlType) {
            extractTypesFromIdlType(member.idlType);
          }
        }
      }

      // Extract from typedef
      if (def.type === 'typedef' && def.idlType) {
        extractTypesFromIdlType(def.idlType);
      }
    }

    // Filter out primitive types and types that are defined in this file
    const primitiveTypes = new Set([
      'boolean', 'byte', 'octet', 'short', 'unsigned short', 'long', 'unsigned long',
      'long long', 'unsigned long long', 'float', 'double', 'unrestricted float',
      'unrestricted double', 'DOMString', 'ByteString', 'USVString', 'any', 'object',
      'undefined', 'ArrayBuffer', 'DataView', 'Int8Array', 'Uint8Array', 'Uint8ClampedArray',
      'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'Float32Array', 'Float64Array',
      'BigInt64Array', 'BigUint64Array', 'SelfRef', 'void'
    ]);

    // Known external enums that shouldn't be forward declared (they need to be included)
    const externalEnums = new Set([
      'ReferrerPolicy',  // From Referrer Policy spec
    ]);

    // Types to forward declare are:
    // 1. Referenced types that aren't primitives, external enums, or local non-interfaces
    // 2. This includes both external interfaces and local interfaces (for forward reference support)
    const typesToForwardDeclare = [...referencedTypes].filter(type =>
      !primitiveTypes.has(type) &&
      !externalEnums.has(type) &&
      !localNonInterfaces.has(type)
    ).sort();

    if (typesToForwardDeclare.length === 0) {
      return '';
    }

    let code = '// Forward declarations for types defined elsewhere\n';
    for (const type of typesToForwardDeclare) {
      code += `class ${type};\n`;
    }
    code += '\n';

    return code;
  }

  makeIncludeGuard(filename) {
    return filename
      .toUpperCase()
      .replace(/[^A-Z0-9]/g, '_')
      + '_H_';
  }

  generateDefinition(def) {
    switch (def.type) {
      case 'interface':
        if (def.partial) {
          this.summary.unsupported.push({ type: 'partial interface', name: def.name, reason: 'Partial interfaces not yet supported (treating as regular interface)' });
        }
        this.summary.interfaces.push(def.name);
        return this.generateInterface(def);
      case 'interface mixin':
        if (def.partial) {
          this.summary.unsupported.push({ type: 'partial mixin', name: def.name, reason: 'Partial mixins not yet supported (treating as regular mixin)' });
        }
        this.summary.mixins.push(def.name);
        // Mixins never inherit from jsg::Object to avoid diamond inheritance
        return this.generateMixin(def, false);
      case 'dictionary':
        if (def.partial) {
          this.summary.unsupported.push({ type: 'partial dictionary', name: def.name, reason: 'Partial dictionaries not yet supported (treating as regular dictionary)' });
        }
        this.summary.dictionaries.push(def.name);
        return this.generateDictionary(def);
      case 'enum':
        this.summary.enums.push(def.name);
        return this.generateEnum(def);
      case 'callback':
        this.summary.callbacks.push(def.name);
        return this.generateCallback(def);
      case 'typedef':
        this.summary.typedefs.push(def.name);
        return this.generateTypedef(def);
      case 'includes':
        // Includes statements are processed separately, no code generation needed
        return '';
      case 'namespace':
        this.summary.unsupported.push({ type: 'namespace', name: def.name, reason: 'Namespaces not yet implemented' });
        return `// Namespace ${def.name} not yet supported\n`;
      default:
        this.summary.unsupported.push({ type: def.type, name: def.name || 'unknown', reason: `Unsupported definition type: ${def.type}` });
        return `// Unsupported definition type: ${def.type}\n`;
    }
  }

  generateInterface(iface) {
    let code = '';

    // Class declaration with inheritance
    // Check if this interface includes any mixins
    const mixinNames = this.includesMap.get(iface.name) || [];
    const baseClasses = [];

    // Add interface inheritance or jsg::Object first
    if (iface.inheritance) {
      baseClasses.push(iface.inheritance);
    } else {
      // Always add jsg::Object if there's no explicit inheritance
      // (mixins don't inherit from jsg::Object to avoid diamond inheritance)
      baseClasses.push('jsg::Object');
    }

    // Add mixins after jsg::Object (in order they were included)
    for (const mixinName of mixinNames) {
      baseClasses.push(mixinName);
    }

    const baseClassList = baseClasses.join(', public ');
    code += `class ${iface.name}: public ${baseClassList} {\n`;
    code += ` public:\n`;

    // C++ constructor (always needed for jsg::Object instances)
    code += `  ${iface.name}();\n\n`;

    // Generate method declarations
    const operations = iface.members.filter(m => m.type === 'operation');
    const attributes = iface.members.filter(m => m.type === 'attribute');
    const constructor = iface.members.find(m => m.type === 'constructor');

    // JavaScript constructor (static method, only if WebIDL has constructor)
    if (constructor) {
      code += this.generateConstructor(iface.name, constructor);
    }

    // Group operations by name to handle overloads
    const operationsByName = new Map();
    for (const op of operations) {
      if (!op.name) continue;
      if (!operationsByName.has(op.name)) {
        operationsByName.set(op.name, []);
      }
      operationsByName.get(op.name).push(op);
    }

    // Generate operations - for overloads, generate all variants with suffix
    for (const [name, overloads] of operationsByName) {
      if (overloads.length === 1) {
        // Single signature - check for custom method name
        const op = overloads[0];
        const customName = this.getMethodName(op.extAttrs);
        const methodName = customName ? customName : this.escapeCppKeyword(op.name);
        code += this.generateOperation(op, '', methodName);
      } else {
        // Multiple overloads - add suffix based on compat flag or param count
        for (let i = 0; i < overloads.length; i++) {
          const op = overloads[i];
          const customName = this.getMethodName(op.extAttrs);
          const compatFlag = this.getCompatFlag(op.extAttrs);
          let suffix = '';

          if (customName) {
            // Use custom method name directly (no suffix)
            code += this.generateOperation(op, '', customName);
          } else if (compatFlag) {
            // Use compat flag as suffix
            suffix = `_${compatFlag}`;
            code += this.generateOperation(op, suffix);
          } else if (i > 0) {
            // Fallback: use numeric suffix for subsequent overloads
            suffix = `_overload${i}`;
            code += this.generateOperation(op, suffix);
          } else {
            // First overload without custom name or flag
            code += this.generateOperation(op);
          }
        }
      }
    }

    // Attribute getters/setters
    for (const attr of attributes) {
      code += this.generateAttribute(attr);
    }

    // Protected region for custom public members
    code += '\n';
    if (this.protectedRegions) {
      const regionName = `${iface.name}::public`;
      const defaultContent = `  // Add custom public methods, fields, or nested types here\n`;
      code += this.protectedRegions.generateRegion(regionName, defaultContent);
    } else {
      code += `  // BEGIN MANUAL SECTION: ${iface.name}::public\n`;
      code += `  // Add custom public methods, fields, or nested types here\n`;
      code += `  // END MANUAL SECTION: ${iface.name}::public\n`;
    }

    // JSG_RESOURCE_TYPE block
    code += '\n';
    code += this.generateJsgResourceType(iface);

    // Private section (optional)
    code += '\n private:\n';
    if (this.protectedRegions) {
      const regionName = `${iface.name}::private`;
      const defaultContent = `  // Add private member variables here\n`;
      code += this.protectedRegions.generateRegion(regionName, defaultContent);
    } else {
      code += `  // BEGIN MANUAL SECTION: ${iface.name}::private\n`;
      code += `  // Add private member variables here\n`;
      code += `  // END MANUAL SECTION: ${iface.name}::private\n`;
    }

    code += `};\n`;
    return code;
  }

  /**
   * Generate a mixin class
   * @param {object} mixin - The mixin definition
   * @param {boolean} needsJsgObject - Whether this mixin should inherit from jsg::Object
   *   Should be false to avoid diamond inheritance when interfaces include multiple mixins
   */
  generateMixin(mixin, needsJsgObject = false) {
    let code = '';

    // Comment indicating this is a mixin
    code += `// Mixin: ${mixin.name}\n`;
    code += `// Used by interfaces via C++ inheritance\n`;

    // Class declaration - only inherit from jsg::Object if needed
    if (needsJsgObject) {
      code += `class ${mixin.name}: public jsg::Object {\n`;
    } else {
      code += `class ${mixin.name} {\n`;
    }
    code += ` public:\n`;

    // Generate method declarations
    const operations = mixin.members.filter(m => m.type === 'operation');
    const attributes = mixin.members.filter(m => m.type === 'attribute');

    // Mixin operations
    for (const op of operations) {
      if (!op.name) continue;
      code += this.generateOperation(op);
    }

    // Mixin attribute getters/setters
    for (const attr of attributes) {
      code += this.generateAttribute(attr);
    }

    // Protected region for custom public members
    code += '\n';
    if (this.protectedRegions) {
      const regionName = `${mixin.name}::public`;
      const defaultContent = `  // Add custom public methods or fields here\n`;
      code += this.protectedRegions.generateRegion(regionName, defaultContent);
    } else {
      code += `  // BEGIN MANUAL SECTION: ${mixin.name}::public\n`;
      code += `  // Add custom public methods or fields here\n`;
      code += `  // END MANUAL SECTION: ${mixin.name}::public\n`;
    }

    // Note: Mixins don't have JSG_RESOURCE_TYPE - their members are registered
    // in the JSG_RESOURCE_TYPE of interfaces that include them

    // Private section
    code += '\n private:\n';
    if (this.protectedRegions) {
      const regionName = `${mixin.name}::private`;
      const defaultContent = `  // Add private member variables here\n`;
      code += this.protectedRegions.generateRegion(regionName, defaultContent);
    } else {
      code += `  // BEGIN MANUAL SECTION: ${mixin.name}::private\n`;
      code += `  // Add private member variables here\n`;
      code += `  // END MANUAL SECTION: ${mixin.name}::private\n`;
    }

    code += `};\n`;
    return code;
  }

  generateConstructor(className, ctor) {
    const params = ['jsg::Lock& js'];
    for (const arg of ctor.arguments) {
      const type = this.typeMapper.mapParameter(arg);
      params.push(`${type} ${arg.name}`);
    }

    return `  static jsg::Ref<${className}> constructor(${params.join(', ')});\n\n`;
  }

  generateOperation(op, suffix = '', customMethodName = null) {
    // Handle special operations (getters, setters, etc.)
    if (op.special) {
      return `  // Special operation: ${op.special}\n`;
    }

    const name = op.name;
    const returnType = this.typeMapper.mapType(op.idlType);

    const params = ['jsg::Lock& js'];
    for (const arg of op.arguments) {
      const type = this.typeMapper.mapParameter(arg);
      params.push(`${type} ${arg.name}`);
    }

    // Use custom method name if provided, otherwise escape and add suffix to original name
    const methodName = customMethodName || this.escapeCppKeyword(name) + suffix;

    return `  ${returnType} ${methodName}(${params.join(', ')});\n`;
  }

  generateAttribute(attr) {
    let code = '';
    const type = this.typeMapper.mapType(attr.idlType);
    const getterName = this.makeGetterName(attr.name);

    // Getter
    code += `  ${type} ${getterName}(jsg::Lock& js);\n`;

    // Setter (if not readonly)
    if (!attr.readonly) {
      const setterName = this.makeSetterName(attr.name);
      code += `  void ${setterName}(jsg::Lock& js, ${type} value);\n`;
    }

    return code;
  }

  generateJsgResourceType(iface) {
    let code = '';

    // Collect members from this interface and any included mixins
    const allMembers = [...iface.members];
    const mixinNames = this.includesMap.get(iface.name) || [];
    for (const mixinName of mixinNames) {
      const mixin = this.mixins.get(mixinName);
      if (mixin) {
        allMembers.push(...mixin.members);
      }
    }

    // Check if any members have compat flags - if so, add flags parameter
    const hasCompatFlags = allMembers.some(m => this.getCompatFlag(m.extAttrs));

    if (hasCompatFlags) {
      code += `  JSG_RESOURCE_TYPE(${iface.name}, CompatibilityFlags::Reader flags) {\n`;
    } else {
      code += `  JSG_RESOURCE_TYPE(${iface.name}) {\n`;
    }

    // Add JSG_INHERIT if this interface has a base class (other than the skip list)
    if (iface.inheritance && !this.skipInterfaces.has(iface.inheritance)) {
      code += `    JSG_INHERIT(${iface.inheritance});\n`;
    }

    const operations = allMembers.filter(m => m.type === 'operation' && m.name);
    const attributes = allMembers.filter(m => m.type === 'attribute');

    // Group operations by name to detect overloads
    const operationsByName = new Map();
    for (const op of operations) {
      if (!operationsByName.has(op.name)) {
        operationsByName.set(op.name, []);
      }
      operationsByName.get(op.name).push(op);
    }

    // Collect all members (attributes + operations) with their flags
    const membersByFlag = new Map();
    membersByFlag.set(null, []); // Members without flags

    // Track members for else blocks (CompatFlagOff)
    const membersByFlagOff = new Map();

    // Add attributes
    for (const attr of attributes) {
      const flag = this.getCompatFlag(attr.extAttrs);
      const flagOff = this.getCompatFlagOff(attr.extAttrs);
      const propertyScope = this.getPropertyScope(attr.extAttrs);

      if (flagOff) {
        if (!membersByFlagOff.has(flagOff)) {
          membersByFlagOff.set(flagOff, []);
        }
        membersByFlagOff.get(flagOff).push({
          type: 'attribute',
          member: attr,
          propertyScope: propertyScope
        });
      } else {
        if (!membersByFlag.has(flag)) {
          membersByFlag.set(flag, []);
        }
        membersByFlag.get(flag).push({
          type: 'attribute',
          member: attr,
          propertyScope: propertyScope
        });
      }
    }    // Add operations (handling overloads)
    for (const [opName, overloads] of operationsByName) {
      if (overloads.length === 1) {
        // Single signature - register normally
        const op = overloads[0];
        const flag = this.getCompatFlag(op.extAttrs);
        const flagOff = this.getCompatFlagOff(op.extAttrs);
        const customName = this.getMethodName(op.extAttrs);
        const escapedName = this.escapeCppKeyword(op.name);
        const cppMethodName = customName || escapedName;
        const needsNaming = customName || (escapedName !== op.name);

        if (flagOff) {
          if (!membersByFlagOff.has(flagOff)) {
            membersByFlagOff.set(flagOff, []);
          }
          membersByFlagOff.get(flagOff).push({
            type: 'operation',
            member: op,
            methodName: cppMethodName,
            jsName: needsNaming ? op.name : null
          });
        } else {
          if (!membersByFlag.has(flag)) {
            membersByFlag.set(flag, []);
          }
          membersByFlag.get(flag).push({
            type: 'operation',
            member: op,
            methodName: cppMethodName,
            jsName: needsNaming ? op.name : null
          });
        }
      } else {
        // Multiple overloads - register each with its own flag
        for (let i = 0; i < overloads.length; i++) {
          const op = overloads[i];
          const compatFlag = this.getCompatFlag(op.extAttrs);
          const compatFlagOff = this.getCompatFlagOff(op.extAttrs);
          const customName = this.getMethodName(op.extAttrs);
          const escapedName = this.escapeCppKeyword(op.name);

          let cppMethodName;
          if (customName) {
            // Use custom method name directly
            cppMethodName = customName;
          } else if (compatFlag || compatFlagOff) {
            // Use compat flag as suffix
            const flagName = compatFlag || compatFlagOff;
            cppMethodName = escapedName + `_${flagName}`;
          } else if (i > 0) {
            // Fallback: use numeric suffix
            cppMethodName = escapedName + `_overload${i}`;
          } else {
            // First overload without custom name or flag
            cppMethodName = escapedName;
          }

          // Add to the appropriate flag group
          if (compatFlagOff) {
            if (!membersByFlagOff.has(compatFlagOff)) {
              membersByFlagOff.set(compatFlagOff, []);
            }
            membersByFlagOff.get(compatFlagOff).push({
              type: 'operation',
              member: op,
              methodName: cppMethodName,
              jsName: op.name
            });
          } else {
            const flag = compatFlag;
            if (!membersByFlag.has(flag)) {
              membersByFlag.set(flag, []);
            }
            membersByFlag.get(flag).push({
              type: 'operation',
              member: op,
              methodName: cppMethodName,
              jsName: op.name  // The JS name stays the same for overloads
            });
          }
        }
      }
    }    // Generate unconditional members first
    const unconditionalMembers = membersByFlag.get(null) || [];
    for (const item of unconditionalMembers) {
      if (item.type === 'attribute') {
        const attr = item.member;
        const propName = attr.name;
        const getterName = this.makeGetterName(attr.name);
        const scope = item.propertyScope || 'prototype';  // Default to prototype

        if (attr.readonly) {
          if (scope === 'instance') {
            code += `    JSG_READONLY_INSTANCE_PROPERTY(${propName}, ${getterName});\n`;
          } else {
            code += `    JSG_READONLY_PROTOTYPE_PROPERTY(${propName}, ${getterName});\n`;
          }
        } else {
          const setterName = this.makeSetterName(attr.name);
          if (scope === 'instance') {
            code += `    JSG_INSTANCE_PROPERTY(${propName}, ${getterName}, ${setterName});\n`;
          } else {
            code += `    JSG_PROTOTYPE_PROPERTY(${propName}, ${getterName}, ${setterName});\n`;
          }
        }
      } else if (item.type === 'operation') {
        const jsName = item.jsName || item.member.name;
        const cppName = item.methodName;

        if (jsName === cppName) {
          code += `    JSG_METHOD(${cppName});\n`;
        } else {
          code += `    JSG_METHOD_NAMED(${jsName}, ${cppName});\n`;
        }
      }
    }

    // Generate conditional members grouped by flag
    for (const [flag, members] of membersByFlag) {
      if (flag === null || members.length === 0) continue;

      // Check if there are corresponding else block members
      const elseMembers = membersByFlagOff.get(flag) || [];

      code += `\n    if (flags.get${flag}()) {\n`;

      for (const item of members) {
        if (item.type === 'attribute') {
          const attr = item.member;
          const propName = attr.name;
          const getterName = this.makeGetterName(attr.name);
          const scope = item.propertyScope || 'prototype';  // Default to prototype

          if (attr.readonly) {
            if (scope === 'instance') {
              code += `      JSG_READONLY_INSTANCE_PROPERTY(${propName}, ${getterName});\n`;
            } else {
              code += `      JSG_READONLY_PROTOTYPE_PROPERTY(${propName}, ${getterName});\n`;
            }
          } else {
            const setterName = this.makeSetterName(attr.name);
            if (scope === 'instance') {
              code += `      JSG_INSTANCE_PROPERTY(${propName}, ${getterName}, ${setterName});\n`;
            } else {
              code += `      JSG_PROTOTYPE_PROPERTY(${propName}, ${getterName}, ${setterName});\n`;
            }
          }
        } else if (item.type === 'operation') {
          const jsName = item.jsName || item.member.name;
          const cppName = item.methodName;

          if (jsName === cppName) {
            code += `      JSG_METHOD(${cppName});\n`;
          } else {
            code += `      JSG_METHOD_NAMED(${jsName}, ${cppName});\n`;
          }
        }
      }

      // Generate else block if there are members for when flag is OFF
      if (elseMembers.length > 0) {
        code += `    } else {\n`;

        for (const item of elseMembers) {
          if (item.type === 'attribute') {
            const attr = item.member;
            const propName = attr.name;
            const getterName = this.makeGetterName(attr.name);
            const scope = item.propertyScope || 'prototype';  // Default to prototype

            if (attr.readonly) {
              if (scope === 'instance') {
                code += `      JSG_READONLY_INSTANCE_PROPERTY(${propName}, ${getterName});\n`;
              } else {
                code += `      JSG_READONLY_PROTOTYPE_PROPERTY(${propName}, ${getterName});\n`;
              }
            } else {
              const setterName = this.makeSetterName(attr.name);
              if (scope === 'instance') {
                code += `      JSG_INSTANCE_PROPERTY(${propName}, ${getterName}, ${setterName});\n`;
              } else {
                code += `      JSG_PROTOTYPE_PROPERTY(${propName}, ${getterName}, ${setterName});\n`;
              }
            }
          } else if (item.type === 'operation') {
            const jsName = item.jsName || item.member.name;
            const cppName = item.methodName;

            if (jsName === cppName) {
              code += `      JSG_METHOD(${cppName});\n`;
            } else {
              code += `      JSG_METHOD_NAMED(${jsName}, ${cppName});\n`;
            }
          }
        }
      }

      code += `    }\n`;
    }

    // Add TypeScript overrides and definitions if present
    const tsDefine = this.getTsDefine(iface.extAttrs);
    const tsOverride = this.getTsOverride(iface.extAttrs);

    if (tsDefine) {
      code += `\n    JSG_TS_DEFINE(${tsDefine});\n`;
    }

    if (tsOverride) {
      code += `\n    JSG_TS_OVERRIDE(${tsOverride});\n`;
    }

    code += `  }\n`;
    return code;
  }  /**
   * Recursively collect all members from a dictionary and its parents
   * @param {object} dict - Dictionary definition
   * @returns {Array} All members including inherited ones
   */
  collectDictionaryMembers(dict) {
    const members = [...dict.members];

    // If this dictionary inherits from another, recursively collect parent members
    if (dict.inheritance) {
      const parentDict = this.definitions.find(
        def => def.type === 'dictionary' && def.name === dict.inheritance
      );

      if (parentDict) {
        // Parent members come first (for proper initialization order)
        const parentMembers = this.collectDictionaryMembers(parentDict);
        return [...parentMembers, ...members];
      }
    }

    return members;
  }

  generateDictionary(dict) {
    let code = '';

    // Struct declaration - NO inheritance in C++ (we flatten instead)
    code += `struct ${dict.name} {\n`;

    // Collect all members including inherited ones
    const allMembers = this.collectDictionaryMembers(dict);

    // Separate exposed and internal fields
    const exposedFields = [];
    const internalFields = [];

    for (const member of allMembers) {
      if (this.isJsgInternalField(member)) {
        internalFields.push(member);
      } else {
        exposedFields.push(member);
      }
    }

    // Generate exposed fields first
    for (const member of exposedFields) {
      const type = this.typeMapper.mapDictionaryMemberType(member);
      code += `  ${type} ${member.name}`;

      // Add comment for default value if present (but don't generate it)
      if (member.default) {
        code += `;  // default: ${this.formatDefaultValue(member.default)}`;
      } else {
        code += `;`;
      }

      code += `\n`;
    }

    // Add blank line before JSG_STRUCT if there are fields
    if (exposedFields.length > 0) {
      code += `\n`;
    }

    // JSG_STRUCT with exposed field names only
    const fieldNames = exposedFields.map(m => m.name).join(', ');
    code += `  JSG_STRUCT(${fieldNames});\n`;

    // Always add JSG_STRUCT_TS_OVERRIDE if there's inheritance
    // This ensures TypeScript sees the inheritance relationship even though C++ is flattened
    if (dict.inheritance) {
      code += `  JSG_STRUCT_TS_OVERRIDE(${dict.name} extends ${dict.inheritance});
`;
    }

    // Add JSG_STRUCT_TS_ROOT if requested
    const tsRoot = this.getTsRoot(dict.extAttrs);
    if (tsRoot) {
      code += `  JSG_STRUCT_TS_ROOT();\n`;
    }

    // Add TypeScript definitions if present
    const tsDefine = this.getTsDefine(dict.extAttrs);
    if (tsDefine) {
      code += `  JSG_STRUCT_TS_DEFINE(${tsDefine});\n`;
    }

    // Add TypeScript override if present
    const tsOverride = this.getTsOverride(dict.extAttrs);
    if (tsOverride) {
      code += `  JSG_TS_OVERRIDE(${tsOverride});\n`;
    }

    // Generate internal fields after JSG_STRUCT
    if (internalFields.length > 0) {
      code += `\n`;
      for (const member of internalFields) {
        const type = this.typeMapper.mapDictionaryMemberType(member);
        code += `  ${type} ${member.name}`;

        // Add comment for SelfRef explaining its purpose
        const baseType = member.idlType.idlType || member.idlType;
        if (baseType === 'SelfRef') {
          code += `;  // Reference to the JavaScript object`;
        } else {
          code += `;  // Internal field, not exposed to JS`;
        }

        code += `\n`;
      }
    }

    // Add custom C++ code (constructors, methods) if specified with [JsgCode]
    const jsgCode = this.getJsgCode(dict.extAttrs);
    if (jsgCode) {
      code += `\n${jsgCode}\n`;
    }

    code += `};\n`;

    return code;
  }  generateEnum(enumDef) {
    let code = '';
    code += `enum class ${enumDef.name} {\n`;

    for (const value of enumDef.values) {
      // Convert "kebab-case" to SCREAMING_SNAKE_CASE for enum values
      const enumValue = value.value
        .replace(/-/g, '_')
        .toUpperCase();
      code += `  ${enumValue},\n`;
    }

    code += `};\n`;
    return code;
  }

  generateTypedef(typedef) {
    // Generate a C++ using alias for the typedef
    const mappedType = this.typeMapper.mapType(typedef.idlType);
    return `using ${typedef.name} = ${mappedType};\n`;
  }

  generateCallback(callback) {
    const returnType = this.typeMapper.mapType(callback.idlType);
    const params = callback.arguments.map(arg => {
      const type = this.typeMapper.mapParameter(arg.idlType);
      return type;
    }).join(', ');

    return `using ${callback.name} = kj::Function<${returnType}(${params})>;\n`;
  }

  /**
   * Escape C++ keywords by adding a trailing underscore
   */
  escapeCppKeyword(name) {
    const cppKeywords = new Set([
      'alignas', 'alignof', 'and', 'and_eq', 'asm', 'auto', 'bitand', 'bitor',
      'bool', 'break', 'case', 'catch', 'char', 'char8_t', 'char16_t', 'char32_t',
      'class', 'compl', 'concept', 'const', 'consteval', 'constexpr', 'constinit',
      'const_cast', 'continue', 'co_await', 'co_return', 'co_yield', 'decltype',
      'default', 'delete', 'do', 'double', 'dynamic_cast', 'else', 'enum',
      'explicit', 'export', 'extern', 'false', 'float', 'for', 'friend', 'goto',
      'if', 'inline', 'int', 'long', 'mutable', 'namespace', 'new', 'noexcept',
      'not', 'not_eq', 'nullptr', 'operator', 'or', 'or_eq', 'private', 'protected',
      'public', 'register', 'reinterpret_cast', 'requires', 'return', 'short',
      'signed', 'sizeof', 'static', 'static_assert', 'static_cast', 'struct',
      'switch', 'template', 'this', 'thread_local', 'throw', 'true', 'try',
      'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using', 'virtual',
      'void', 'volatile', 'wchar_t', 'while', 'xor', 'xor_eq'
    ]);

    return cppKeywords.has(name) ? name + '_' : name;
  }

  makeGetterName(propName) {
    // Convert camelCase to getterName format
    return `get${propName.charAt(0).toUpperCase()}${propName.slice(1)}`;
  }

  makeSetterName(propName) {
    return `set${propName.charAt(0).toUpperCase()}${propName.slice(1)}`;
  }

  formatDefaultValue(defaultValue) {
    switch (defaultValue.type) {
      case 'boolean':
        return defaultValue.value ? 'true' : 'false';
      case 'number':
        return String(defaultValue.value);
      case 'string':
        return `"${defaultValue.value}"_kj`;
      case 'null':
        return 'nullptr';
      case 'sequence':
      case 'dictionary':
        return '{}';
      default:
        return '/* default */';
    }
  }
}
