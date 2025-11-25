#!/usr/bin/env node

/**
 * Test code generation correctness
 */

import { parseWebIDL } from '../src/parser.js';
import { CppGenerator } from '../src/generator.js';
import { ImplGenerator } from '../src/impl-generator.js';
import { TestRunner, assert, assertIncludes, assertNotIncludes, assertMatches } from './test-helpers.js';

const runner = new TestRunner('Code Generation Tests');

runner.test('Simple interface generates correct class', () => {
  const webidl = `
    [Exposed=*]
    interface Calculator {
      long add(long a, long b);
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'calc' });

  assertIncludes(code, 'class Calculator: public jsg::Object', 'Should generate class');
  assertIncludes(code, 'int32_t add(jsg::Lock& js, int32_t a, int32_t b);', 'Should generate method');
  assertIncludes(code, 'JSG_RESOURCE_TYPE(Calculator)', 'Should have JSG_RESOURCE_TYPE');
  assertIncludes(code, 'JSG_METHOD(add);', 'Should register method');
});

runner.test('Interface with constructor generates static constructor', () => {
  const webidl = `
    [Exposed=*]
    interface Point {
      constructor(long x, long y);
      readonly attribute long x;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'point' });

  assertIncludes(code, 'static jsg::Ref<Point> constructor(jsg::Lock& js, int32_t x, int32_t y);', 'Should generate static constructor');
  assertIncludes(code, 'JSG_RESOURCE_TYPE(Point)', 'Should have JSG_RESOURCE_TYPE');
});runner.test('Dictionary generates struct with JSG_STRUCT', () => {
  const webidl = `
    dictionary Point {
      long x;
      long y;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'point' });

  assertIncludes(code, 'struct Point {', 'Should generate struct');
  assertIncludes(code, 'jsg::Optional<int32_t> x;', 'Should have x field');
  assertIncludes(code, 'jsg::Optional<int32_t> y;', 'Should have y field');
  assertIncludes(code, 'JSG_STRUCT(x, y);', 'Should have JSG_STRUCT macro');
});

runner.test('Enum generates enum class', () => {
  const webidl = `
    enum Color { "red", "green", "blue" };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'color' });

  assertIncludes(code, 'enum class Color {', 'Should generate enum class');
  assertIncludes(code, 'RED,', 'Should have RED value');
  assertIncludes(code, 'GREEN,', 'Should have GREEN value');
  assertIncludes(code, 'BLUE,', 'Should have BLUE value');
});

runner.test('Typedef generates using declaration', () => {
  const webidl = `
    typedef (DOMString or long) StringOrNumber;
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'types' });

  assertIncludes(code, 'using StringOrNumber = kj::OneOf<jsg::DOMString, int32_t>;', 'Should generate using declaration');
});

runner.test('Interface mixin generates plain class', () => {
  const webidl = `
    interface mixin Body {
      readonly attribute boolean bodyUsed;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'body' });

  assertIncludes(code, 'class Body {', 'Should generate class');
  assertNotIncludes(code, ': public jsg::Object', 'Mixin should not inherit from jsg::Object');
  assertIncludes(code, 'bool getBodyUsed(jsg::Lock& js);', 'Should have getter');
});

runner.test('Interface with includes inherits from mixin', () => {
  const webidl = `
    interface mixin Body {
      readonly attribute boolean bodyUsed;
    };

    [Exposed=*]
    interface Request {};
    Request includes Body;
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'request' });

  assertIncludes(code, 'class Request: public jsg::Object, public Body {', 'Should inherit from jsg::Object and Body');
  assertIncludes(code, 'JSG_READONLY_PROTOTYPE_PROPERTY(bodyUsed, getBodyUsed);', 'Should register mixin property');
});

runner.test('Optional parameters generate jsg::Optional', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      undefined doSomething(DOMString required, optional DOMString opt);
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'void doSomething(jsg::Lock& js, jsg::DOMString required, jsg::Optional<jsg::DOMString> opt);');
});

runner.test('Nullable types generate kj::Maybe', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      attribute DOMString? nullableString;
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'kj::Maybe<jsg::DOMString> getNullableString(jsg::Lock& js);');
  assertIncludes(code, 'void setNullableString(jsg::Lock& js, kj::Maybe<jsg::DOMString> value);');
});

runner.test('Promise return type generates jsg::Promise', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      Promise<DOMString> fetchData();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'jsg::Promise<jsg::DOMString> fetchData(jsg::Lock& js);');
});

runner.test('Sequence type generates kj::Array', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      sequence<DOMString> getItems();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'api' });

  assertIncludes(code, 'kj::Array<jsg::DOMString> getItems(jsg::Lock& js);');
});

runner.test('Record type generates jsg::Dict', () => {
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

runner.test('Forward declarations generated for local interfaces', () => {
  const webidl = `
    typedef (Request or DOMString) RequestInfo;

    [Exposed=*]
    interface Request {
      constructor(RequestInfo input);
    };
  `;

  const definitions = parseWebIDL(webidl);
  const generator = new CppGenerator();
  const code = generator.generate(definitions, { namespace: 'test', filename: 'request' });

  // Forward declaration should come before typedef
  const forwardDeclPos = code.indexOf('class Request;');
  const typedefPos = code.indexOf('using RequestInfo');

  assert(forwardDeclPos > 0, 'Should have forward declaration');
  assert(typedefPos > 0, 'Should have typedef');
  assert(forwardDeclPos < typedefPos, 'Forward declaration should come before typedef');
});

runner.test('Implementation stubs include TODO comments', () => {
  const webidl = `
    [Exposed=*]
    interface Calculator {
      long add(long a, long b);
    };
  `;

  const definitions = parseWebIDL(webidl);
  const implGenerator = new ImplGenerator();
  const code = implGenerator.generate(definitions, {
    namespace: 'test',
    headerFile: 'calc.h',
  });

  assertIncludes(code, '// TODO: Implement add', 'Should have TODO comment');
  assertIncludes(code, 'return 0;', 'Should have placeholder return');
});

runner.test('String return type generates kj::StringPtr suggestion', () => {
  const webidl = `
    [Exposed=*]
    interface API {
      DOMString getMessage();
    };
  `;

  const definitions = parseWebIDL(webidl);
  const implGenerator = new ImplGenerator();
  const code = implGenerator.generate(definitions, {
    namespace: 'test',
    headerFile: 'api.h',
  });

  assertIncludes(code, 'return "TODO"_kj;', 'Should suggest kj::StringPtr');
  assertIncludes(code, 'kj::StringPtr', 'Should mention kj::StringPtr optimization');
});

const results = await runner.run();
const success = runner.summary();

process.exit(success ? 0 : 1);
