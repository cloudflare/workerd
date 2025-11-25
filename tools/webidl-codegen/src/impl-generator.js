/**
 * C++ implementation stub generator for JSG bindings from WebIDL
 * Generates .c++ files with placeholder implementations for user override
 */

import { TypeMapper } from './type-mapper.js';
import { ProtectedRegions } from './protected-regions.js';

export class ImplGenerator {
  constructor(options = {}) {
    this.typeMapper = new TypeMapper();
    this.protectedRegions = null; // Set via setProtectedRegions() if preserving content
    this.incrementalMode = false; // Set to true to only generate stubs for new methods
    this.skipInterfaces = new Set(options.skipInterfaces || [
      'Event',
      'EventTarget',
      'EventListener',
      'AbortSignal',
      'AbortController',
    ]);
    // Mixin tracking
    this.mixins = new Map(); // name -> mixin definition
    // Generation summary tracking
    this.summary = {
      implementations: [],
      skipped: [],
    };
  }

  /**
   * Set protected regions from existing file
   */
  setProtectedRegions(protectedRegions) {
    this.protectedRegions = protectedRegions;
  }

  /**
   * Enable incremental mode - only generate stubs for new methods
   */
  setIncrementalMode(enabled) {
    this.incrementalMode = enabled;
  }

  /**
   * Get generation summary
   */
  getSummary() {
    return this.summary;
  }

  /**
   * Generate implementation stub file
   */
  generate(definitions, options = {}) {
    // Reset summary
    this.summary = {
      implementations: [],
      skipped: [],
    };

    // Update type mapper with definitions so it can distinguish dictionaries from interfaces
    this.typeMapper = new TypeMapper(definitions);

    // Process mixins
    this.processMixins(definitions);

    const { namespace = 'workerd::api', headerFile } = options;

    let code = '';

    // Header comment
    code += `// Generated implementation stubs - EDIT THIS FILE\n`;
    code += `// This file contains placeholder implementations.\n`;
    code += `// Replace TODO comments with actual logic.\n\n`;

    // Include the generated header
    if (headerFile) {
      code += `#include "${headerFile}"\n`;
    } else {
      code += `// #include "generated.h"  // Include your generated header\n`;
    }
    code += `\n`;

    // Additional includes that might be needed
    code += `// Add additional includes as needed:\n`;
    code += `// #include <workerd/jsg/jsg.h>\n`;
    code += `// #include <workerd/api/basics.h>\n`;
    code += `\n`;

    // Namespace (use :: notation for nested namespaces)
    code += `namespace ${namespace} {\n\n`;

    // Generate implementations for each definition
    for (const def of definitions) {
      // Skip interfaces/mixins that are already defined in workerd or explicitly excluded
      if ((def.type === 'interface' || def.type === 'interface mixin') && this.skipInterfaces.has(def.name)) {
        this.summary.skipped.push({ type: def.type === 'interface mixin' ? 'mixin' : 'interface', name: def.name });
        continue;
      }

      if (def.type === 'interface' && !def.mixin) {
        this.summary.implementations.push(def.name);
      }

      code += this.generateDefinitionImpl(def);
      code += '\n';
    }

    // Close namespace
    code += `}  // namespace ${namespace}\n`;

    return code;
  }

  /**
   * Process mixin definitions
   */
  processMixins(definitions) {
    for (const def of definitions) {
      if (def.type === 'interface mixin') {
        this.mixins.set(def.name, def);
      }
    }
  }

  generateDefinitionImpl(def) {
    switch (def.type) {
      case 'interface':
        return this.generateInterfaceImpl(def);
      case 'interface mixin':
        return this.generateMixinImpl(def);
      case 'dictionary':
        return this.generateDictionaryImpl(def);
      case 'includes':
        // Includes statements don't need implementation
        return '';
      default:
        return ``;  // No implementation needed for enums, callbacks
    }
  }

  generateInterfaceImpl(iface) {
    let code = '';

    const operations = iface.members.filter(m => m.type === 'operation');
    const attributes = iface.members.filter(m => m.type === 'attribute');
    const constructor = iface.members.find(m => m.type === 'constructor');

    // C++ constructor (always needed for jsg::Object instances)
    code += this.generateCppConstructorImpl(iface.name);
    code += '\n';

    // JavaScript constructor (static method, only if WebIDL has constructor)
    if (constructor) {
      code += this.generateJsConstructorImpl(iface.name, constructor);
      code += '\n';
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

    // Operation implementations
    for (const [opName, overloads] of operationsByName) {
      for (let i = 0; i < overloads.length; i++) {
        const op = overloads[i];
        const compatFlag = this.getCompatFlag(op.extAttrs);
        const customName = this.getMethodName(op.extAttrs);

        let suffix = '';
        if (customName) {
          code += this.generateOperationImpl(iface.name, op, '', customName);
        } else if (compatFlag) {
          suffix = `_${compatFlag}`;
          code += this.generateOperationImpl(iface.name, op, suffix);
        } else if (i > 0) {
          suffix = `_overload${i}`;
          code += this.generateOperationImpl(iface.name, op, suffix);
        } else {
          code += this.generateOperationImpl(iface.name, op);
        }
        code += '\n';
      }
    }

    // Attribute implementations
    for (const attr of attributes) {
      code += this.generateAttributeImpl(iface.name, attr);
      code += '\n';
    }

    return code;
  }

  generateCppConstructorImpl(className) {
    const regionName = `${className}::constructor`;

    // In incremental mode, skip if already implemented
    if (this.incrementalMode && this.protectedRegions && this.protectedRegions.hasRegion(regionName)) {
      return this.protectedRegions.generateInlineRegion(regionName, '');
    }

    // Default content if no preserved version exists
    const defaultContent = `${className}::${className}() {
  // TODO: Initialize member variables
}
`;    if (this.protectedRegions && this.protectedRegions.hasRegion(regionName)) {
      // Use preserved implementation
      return this.protectedRegions.generateInlineRegion(regionName, defaultContent);
    }

    // Generate default with protected region markers
    let code = '';
    code += `// BEGIN MANUAL SECTION: ${regionName}
`;
    code += `// C++ constructor - add parameters as needed for your implementation
`;
    code += defaultContent;
    code += `// END MANUAL SECTION: ${regionName}
`;
    return code;
  }

  generateJsConstructorImpl(className, ctor) {
    const regionName = `${className}::constructor(js)`;

    const params = ['jsg::Lock& js'];
    const paramNames = ['js'];

    for (const arg of ctor.arguments) {
      const type = this.typeMapper.mapParameter(arg);
      params.push(`${type} ${arg.name}`);
      paramNames.push(arg.name);
    }

    // In incremental mode, skip if already implemented
    if (this.incrementalMode && this.protectedRegions && this.protectedRegions.hasRegion(regionName)) {
      return this.protectedRegions.generateInlineRegion(regionName, '');
    }

    // Default content if no preserved version exists
    const defaultContent = `jsg::Ref<${className}> ${className}::constructor(${params.join(', ')}) {
  // TODO: Implement JavaScript constructor
  // Create and return a new instance using js.alloc
  // The C++ constructor will be called automatically
  return js.alloc<${className}>();
}
`;

    if (this.protectedRegions && this.protectedRegions.hasRegion(regionName)) {
      return this.protectedRegions.generateInlineRegion(regionName, defaultContent);
    }

    let code = '';
    code += `// BEGIN MANUAL SECTION: ${regionName}\n`;
    code += `// JavaScript constructor (static method for JS 'new' operator)\n`;
    code += defaultContent;
    code += `// END MANUAL SECTION: ${regionName}\n`;
    return code;
  }

  generateOperationImpl(className, op, suffix = '', customMethodName = null) {
    if (op.special) {
      return '';  // Skip special operations
    }

    const name = op.name;
    const returnType = this.typeMapper.mapType(op.idlType);
    const methodName = customMethodName || (name + suffix);
    const regionName = `${className}::${methodName}`;

    const params = ['jsg::Lock& js'];
    const paramNames = ['js'];

    for (const arg of op.arguments) {
      const type = this.typeMapper.mapParameter(arg);
      params.push(`${type} ${arg.name}`);
      paramNames.push(arg.name);
    }

    // Build default implementation
    let defaultContent = `${returnType} ${className}::${methodName}(${params.join(', ')}) {\n`;
    defaultContent += `  // TODO: Implement ${methodName}\n`;

    // Add helpful comments based on return type
    const returnTypeStr = this.typeMapper.mapTypeToString(op.idlType);
    if (returnTypeStr.includes('Promise')) {
      defaultContent += `  // Return a promise that resolves with the result\n`;
      defaultContent += `  // Example: return js.resolvedPromise(...);\n`;
    } else if (returnTypeStr === 'void' || returnTypeStr === 'undefined') {
      defaultContent += `  // Perform the operation\n`;
    } else if (returnTypeStr.includes('jsg::Ref')) {
      defaultContent += `  // Create and return a new object reference\n`;
      defaultContent += `  // Example: return js.alloc<SomeType>(...);\n`;
    } else {
      defaultContent += `  // Return the result\n`;
    }

    // Generate a basic return statement
    if (returnTypeStr === 'void' || returnTypeStr === 'undefined') {
      // No return needed
    } else if (returnTypeStr.includes('Promise')) {
      defaultContent += `  return js.resolvedPromise();\n`;
    } else if (returnTypeStr.includes('jsg::Ref')) {
      // Extract the type from jsg::Ref<Type>
      const match = returnTypeStr.match(/jsg::Ref<(.+)>/);
      if (match) {
        defaultContent += `  return js.alloc<${match[1]}>();\n`;
      } else {
        defaultContent += `  // return ...\n`;
      }
    } else if (returnTypeStr.includes('kj::String') ||
               returnTypeStr.includes('jsg::DOMString') ||
               returnTypeStr.includes('jsg::USVString') ||
               returnTypeStr.includes('jsg::ByteString')) {
      defaultContent += `  return "TODO"_kj;  // kj::StringPtr avoids allocation for string literals\n`;
      defaultContent += `  // Note: Consider changing return type to kj::StringPtr in header if returning literals/fixed strings\n`;
    } else if (returnTypeStr.includes('bool')) {
      defaultContent += `  return false;\n`;
    } else if (returnTypeStr.includes('int') || returnTypeStr.includes('double') || returnTypeStr.includes('float')) {
      defaultContent += `  return 0;\n`;
    } else if (returnTypeStr.includes('jsg::Optional')) {
      defaultContent += `  return kj::none;\n`;
    } else if (returnTypeStr.includes('kj::Maybe')) {
      defaultContent += `  return kj::none;\n`;
    } else if (returnTypeStr.includes('kj::Array')) {
      defaultContent += `  return kj::Array<...>();  // TODO: Fill in array type\n`;
    } else {
      defaultContent += `  // return ...\n`;
    }

    defaultContent += `}\n`;

    // Check if we have a preserved implementation
    if (this.protectedRegions && this.protectedRegions.hasRegion(regionName)) {
      return this.protectedRegions.generateInlineRegion(regionName, defaultContent);
    }

    // Generate with protected region markers
    let code = '';
    code += `// BEGIN MANUAL SECTION: ${regionName}\n`;
    code += defaultContent;
    code += `// END MANUAL SECTION: ${regionName}\n`;

    return code;
  }

  generateAttributeImpl(className, attr) {
    const type = this.typeMapper.mapType(attr.idlType);
    const typeStr = this.typeMapper.mapTypeToString(attr.idlType);
    const getterName = this.makeGetterName(attr.name);
    const getterRegionName = `${className}::${getterName}`;

    // Build getter default content
    let getterContent = `${type} ${className}::${getterName}(jsg::Lock& js) {\n`;
    getterContent += `  // TODO: Implement getter for ${attr.name}\n`;

    // Generate a basic return statement based on type
    if (typeStr.includes('kj::String') ||
        typeStr.includes('jsg::DOMString') ||
        typeStr.includes('jsg::USVString') ||
        typeStr.includes('jsg::ByteString')) {
      getterContent += `  return "TODO"_kj;  // kj::StringPtr avoids allocation for string literals\n`;
      getterContent += `  // Note: Consider changing return type to kj::StringPtr in header if returning literals/fixed strings\n`;
    } else if (typeStr.includes('bool')) {
      getterContent += `  return false;\n`;
    } else if (typeStr.includes('int') || typeStr.includes('double') || typeStr.includes('float')) {
      getterContent += `  return 0;\n`;
    } else if (typeStr.includes('jsg::Optional')) {
      getterContent += `  return kj::none;\n`;
    } else if (typeStr.includes('kj::Maybe')) {
      getterContent += `  return kj::none;\n`;
    } else if (typeStr.includes('jsg::Ref')) {
      const match = typeStr.match(/jsg::Ref<(.+)>/);
      if (match) {
        getterContent += `  return js.alloc<${match[1]}>();\n`;
      } else {
        getterContent += `  // return ...\n`;
      }
    } else {
      getterContent += `  // return ...\n`;
    }

    getterContent += `}\n`;

    // Generate getter with protected region
    let code = '';
    // In incremental mode, skip if already implemented
    if (this.incrementalMode && this.protectedRegions && this.protectedRegions.hasRegion(getterRegionName)) {
      code += this.protectedRegions.generateInlineRegion(getterRegionName, '');
    } else if (this.protectedRegions && this.protectedRegions.hasRegion(getterRegionName)) {
      code += this.protectedRegions.generateInlineRegion(getterRegionName, getterContent);
    } else {
      code += `// BEGIN MANUAL SECTION: ${getterRegionName}\n`;
      code += getterContent;
      code += `// END MANUAL SECTION: ${getterRegionName}\n`;
    }

    // Setter implementation (if not readonly)
    if (!attr.readonly) {
      const setterName = this.makeSetterName(attr.name);
      const setterRegionName = `${className}::${setterName}`;

      let setterContent = `void ${className}::${setterName}(jsg::Lock& js, ${type} value) {\n`;
      setterContent += `  // TODO: Implement setter for ${attr.name}\n`;
      setterContent += `  // Store the value in your class member\n`;
      setterContent += `}\n`;

      code += `\n`;
      // In incremental mode, skip if already implemented
      if (this.incrementalMode && this.protectedRegions && this.protectedRegions.hasRegion(setterRegionName)) {
        code += this.protectedRegions.generateInlineRegion(setterRegionName, '');
      } else if (this.protectedRegions && this.protectedRegions.hasRegion(setterRegionName)) {
        code += this.protectedRegions.generateInlineRegion(setterRegionName, setterContent);
      } else {
        code += `// BEGIN MANUAL SECTION: ${setterRegionName}\n`;
        code += setterContent;
        code += `// END MANUAL SECTION: ${setterRegionName}\n`;
      }
    }

    return code;
  }

  generateMixinImpl(mixin) {
    let code = '';

    code += `// Mixin implementation: ${mixin.name}\n`;
    code += `// These methods will be inherited by interfaces that include this mixin\n\n`;

    const operations = mixin.members.filter(m => m.type === 'operation');
    const attributes = mixin.members.filter(m => m.type === 'attribute');

    // Generate operations
    for (const op of operations) {
      if (!op.name) continue;
      code += this.generateOperationImpl(mixin.name, op);
      code += '\n';
    }

    // Generate attribute getters/setters
    for (const attr of attributes) {
      code += this.generateAttributeImpl(mixin.name, attr);
      code += '\n';
    }

    return code;
  }

  generateDictionaryImpl(dict) {
    let code = '';

    // Check if there's custom code that might need implementations
    const customCode = this.getJsgCode(dict.extAttrs);
    if (!customCode) {
      return '';  // No custom methods to implement
    }

    code += `// ${dict.name} custom method implementations\n`;
    code += `// Add implementations for methods declared in [JsgCode]\n`;
    code += `\n`;

    // We can't easily parse the custom code, so just add a comment
    code += `// Example:\n`;
    code += `// void ${dict.name}::validate(jsg::Lock& js) {\n`;
    code += `//   // Validate the struct members\n`;
    code += `//   JSG_REQUIRE(!someField.empty(), TypeError, "someField cannot be empty");\n`;
    code += `// }\n`;
    code += `\n`;

    return code;
  }

  makeGetterName(attrName) {
    return 'get' + attrName.charAt(0).toUpperCase() + attrName.slice(1);
  }

  makeSetterName(attrName) {
    return 'set' + attrName.charAt(0).toUpperCase() + attrName.slice(1);
  }

  getCompatFlag(extAttrs) {
    if (!extAttrs) return null;
    const compatAttr = extAttrs.find(attr => attr.name === 'JsgCompatFlag');
    if (!compatAttr) return null;
    if (compatAttr.rhs) {
      return compatAttr.rhs.value || compatAttr.rhs;
    }
    return null;
  }

  getMethodName(extAttrs) {
    if (!extAttrs) return null;
    const methodAttr = extAttrs.find(attr => attr.name === 'JsgMethodName');
    if (!methodAttr) return null;
    if (methodAttr.rhs) {
      return methodAttr.rhs.value || methodAttr.rhs;
    }
    return null;
  }

  getJsgCode(extAttrs) {
    if (!extAttrs) return null;
    const jsgAttr = extAttrs.find(attr => attr.name === 'JsgCode');
    if (!jsgAttr) return null;
    if (jsgAttr.rhs) {
      return jsgAttr.rhs.value || jsgAttr.rhs;
    }
    return null;
  }
}
