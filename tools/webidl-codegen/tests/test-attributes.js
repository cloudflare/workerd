#!/usr/bin/env node

/**
 * Test all extended attributes (Jsg-prefixed)
 */

import { parseWebIDL } from '../src/parser.js';
import { CppGenerator } from '../src/generator.js';
import { ImplGenerator } from '../src/impl-generator.js';
import { TestRunner, assertIncludes, assertNotIncludes } from './test-helpers.js';

const runner = new TestRunner('Extended Attributes Tests');

// JsgCompatFlag
runner.test('[JsgCompatFlag] generates runtime compat flag check', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      [JsgCompatFlag=experimental_feature] undefined newMethod();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'void newMethod(jsg::Lock& js);', 'Should have method');
  assertIncludes(code, 'JSG_RESOURCE_TYPE(API, CompatibilityFlags::Reader flags)', 'Should have flags parameter');
  assertIncludes(code, 'if (flags.getexperimental_feature())', 'Should have runtime check');
  assertIncludes(code, 'JSG_METHOD(newMethod);', 'Should register method');
});

runner.test('[JsgCompatFlag] on attribute generates runtime check', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      [JsgCompatFlag=experimental_feature] readonly attribute DOMString value;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::DOMString getValue(jsg::Lock& js);');
  assertIncludes(code, 'if (flags.getexperimental_feature())');
  assertIncludes(code, 'JSG_READONLY_PROTOTYPE_PROPERTY(value, getValue);');
});

// JsgMethodName
runner.test('[JsgMethodName] changes C++ method name', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      [JsgMethodName=customMethod] undefined jsMethod();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'void customMethod(jsg::Lock& js);', 'Should use custom method name');
  assertIncludes(code, 'JSG_METHOD_NAMED(jsMethod, customMethod);', 'Should register with JSG_METHOD_NAMED');
  assertNotIncludes(code, 'void jsMethod(jsg::Lock& js);', 'Should not use original name');
});

// JsgPropertyScope
runner.test('[JsgPropertyScope=instance] generates JSG_INSTANCE_PROPERTY', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      [JsgPropertyScope=instance] readonly attribute DOMString instanceProp;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'JSG_READONLY_INSTANCE_PROPERTY(instanceProp, getInstanceProp);');
  assertNotIncludes(code, 'JSG_READONLY_PROTOTYPE_PROPERTY');
});

runner.test('No [JsgPropertyScope] defaults to prototype', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      readonly attribute DOMString defaultProp;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'JSG_READONLY_PROTOTYPE_PROPERTY(defaultProp, getDefaultProp);');
});

// JsgInternal
runner.test('[JsgInternal] excludes field from JSG_STRUCT', () => {
  const webidl = `
    dictionary Options {
      DOMString publicField;
      [JsgInternal] DOMString internalField;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'options' });

  assertIncludes(code, 'jsg::Optional<jsg::DOMString> publicField;');
  assertIncludes(code, 'jsg::Optional<jsg::DOMString> internalField;');
  assertIncludes(code, 'JSG_STRUCT(publicField);', 'Should only include publicField in JSG_STRUCT');
  assertNotIncludes(code, 'JSG_STRUCT(publicField, internalField);', 'Should not include internalField');
});

const results = await runner.run();
const success = runner.summary();

process.exit(success ? 0 : 1);
