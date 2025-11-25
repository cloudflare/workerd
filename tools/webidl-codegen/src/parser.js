/**
 * WebIDL parser using webidl2 library
 */

import * as WebIDL2 from 'webidl2';

/**
 * Parse WebIDL text into an AST
 * @param {string} idlText - WebIDL source text
 * @returns {Array} Parsed WebIDL definitions
 */
export function parseWebIDL(idlText) {
  try {
    return WebIDL2.parse(idlText);
  } catch (error) {
    // Enhance error message
    if (error.line !== undefined) {
      throw new Error(
        `WebIDL parse error at line ${error.line}:${error.column}: ${error.message}`
      );
    }
    throw error;
  }
}

/**
 * Validate WebIDL definitions for JSG compatibility
 * @param {Array} definitions - Parsed WebIDL definitions
 */
export function validateForJSG(definitions) {
  const errors = [];

  for (const def of definitions) {
    if (def.type === 'interface') {
      validateInterface(def, errors);
    }
  }

  if (errors.length > 0) {
    throw new Error(
      'WebIDL validation failed:\n' + errors.map(e => `  - ${e}`).join('\n')
    );
  }
}

function validateInterface(iface, errors) {
  // Check for unsupported features
  if (iface.partial) {
    errors.push(`Partial interfaces not yet supported: ${iface.name}`);
  }

  if (iface.includes && iface.includes.length > 0) {
    errors.push(`Mixins not yet supported in interface: ${iface.name}`);
  }

  // Validate members
  for (const member of iface.members) {
    if (member.type === 'iterable') {
      errors.push(`Iterables not yet supported in interface: ${iface.name}`);
    }

    if (member.type === 'maplike' || member.type === 'setlike') {
      errors.push(`Maplike/setlike not yet supported in interface: ${iface.name}`);
    }
  }
}
