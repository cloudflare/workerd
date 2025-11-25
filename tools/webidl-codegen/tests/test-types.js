#!/usr/bin/env node

/**
 * Test type mapping for all WebIDL types
 */

import { parseWebIDL } from '../src/parser.js';
import { CppGenerator } from '../src/generator.js';
import { TestRunner, assertIncludes } from './test-helpers.js';

const runner = new TestRunner('Type Mapping Tests');

// Primitive types
runner.test('boolean maps to bool', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      boolean getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'bool getValue(jsg::Lock& js);');
});

runner.test('byte maps to int8_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      byte getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'int8_t getValue(jsg::Lock& js);');
});

runner.test('octet maps to uint8_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      octet getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'uint8_t getValue(jsg::Lock& js);');
});

runner.test('short maps to int16_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      short getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'int16_t getValue(jsg::Lock& js);');
});

runner.test('unsigned short maps to uint16_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      unsigned short getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'uint16_t getValue(jsg::Lock& js);');
});

runner.test('long maps to int32_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      long getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'int32_t getValue(jsg::Lock& js);');
});

runner.test('unsigned long maps to uint32_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      unsigned long getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'uint32_t getValue(jsg::Lock& js);');
});

runner.test('long long maps to int64_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      long long getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'int64_t getValue(jsg::Lock& js);');
});

runner.test('unsigned long long maps to uint64_t', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      unsigned long long getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'uint64_t getValue(jsg::Lock& js);');
});

runner.test('float maps to float', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      float getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'float getValue(jsg::Lock& js);');
});

runner.test('unrestricted float maps to float', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      unrestricted float getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'float getValue(jsg::Lock& js);');
});

runner.test('double maps to double', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      double getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'double getValue(jsg::Lock& js);');
});

runner.test('unrestricted double maps to double', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      unrestricted double getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'double getValue(jsg::Lock& js);');
});

runner.test('DOMString maps to jsg::DOMString', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      DOMString getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::DOMString getValue(jsg::Lock& js);');
});

runner.test('ByteString maps to jsg::ByteString', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      ByteString getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::ByteString getValue(jsg::Lock& js);');
});

runner.test('USVString maps to jsg::USVString', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      USVString getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::USVString getValue(jsg::Lock& js);');
});

runner.test('undefined maps to void', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      undefined doSomething();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'void doSomething(jsg::Lock& js);');
});

runner.test('any maps to jsg::JsValue', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      any getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::JsValue getValue(jsg::Lock& js);');
});

runner.test('object maps to jsg::JsObject', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      object getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::JsObject getValue(jsg::Lock& js);');
});

// Generic types
runner.test('Promise<T> maps to jsg::Promise<T>', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      Promise<DOMString> getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::Promise<jsg::DOMString> getValue(jsg::Lock& js);');
});

runner.test('sequence<T> maps to kj::Array<T>', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      sequence<long> getValues();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'kj::Array<int32_t> getValues(jsg::Lock& js);');
});

runner.test('record<K, V> maps to jsg::Dict<K, V>', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      record<DOMString, long> getMap();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::Dict<jsg::DOMString, int32_t> getMap(jsg::Lock& js);');
});

runner.test('FrozenArray<T> maps to kj::Array<const T>', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      readonly attribute FrozenArray<DOMString> items;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'kj::Array<const jsg::DOMString> getItems(jsg::Lock& js);');
});

// Optional and nullable
runner.test('optional parameter maps to jsg::Optional<T>', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      undefined doSomething(optional DOMString value);
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'void doSomething(jsg::Lock& js, jsg::Optional<jsg::DOMString> value);');
});

runner.test('nullable return type maps to kj::Maybe<T>', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      DOMString? getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'kj::Maybe<jsg::DOMString> getValue(jsg::Lock& js);');
});

// Union types
runner.test('union type maps to kj::OneOf', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      (DOMString or long) getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'kj::OneOf<jsg::DOMString, int32_t> getValue(jsg::Lock& js);');
});

runner.test('three-way union maps to kj::OneOf with three types', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      (DOMString or long or boolean) getValue();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'kj::OneOf<jsg::DOMString, int32_t, bool> getValue(jsg::Lock& js);');
});

// Buffer types
runner.test('ArrayBuffer maps to jsg::BufferSource', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      ArrayBuffer getData();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::BufferSource getData(jsg::Lock& js);');
});

runner.test('BufferSource maps to jsg::BufferSource', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      BufferSource getData();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::BufferSource getData(jsg::Lock& js);');
});

// Interface types
runner.test('local interface maps to jsg::Ref<T>', () => {
  const webidl = `
    [Exposed=*]
    interface Data {};

    [Exposed=*]
    interface API {
      Data getData();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::Ref<Data> getData(jsg::Lock& js);');
});

runner.test('external interface maps to jsg::Ref<T>', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      Request getRequest();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator({ externalInterfaces: ['Request'] });
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::Ref<Request> getRequest(jsg::Lock& js);');
});

runner.test('enum type not wrapped in jsg::Ref', () => {
  const webidl = `
    enum Color { "red", "green" };

    [Exposed=*]
    interface API {
      Color getColor();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'Color getColor(jsg::Lock& js);');
});

runner.test('dictionary type not wrapped in jsg::Ref', () => {
  const webidl = `
    dictionary Options {
      DOMString value;
    };

    [Exposed=*]
    interface API {
      Options getOptions();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'Options getOptions(jsg::Lock& js);');
});

const results = await runner.run();
const success = runner.summary();

process.exit(success ? 0 : 1);
