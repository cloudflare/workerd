/**
 * Test helpers and utilities
 */

export class TestRunner {
  constructor(name) {
    this.name = name;
    this.passed = 0;
    this.failed = 0;
    this.tests = [];
  }

  test(description, fn) {
    this.tests.push({ description, fn });
  }

  async run() {
    console.log(`\n${'='.repeat(70)}`);
    console.log(`${this.name}`);
    console.log('='.repeat(70));

    for (const { description, fn } of this.tests) {
      try {
        await fn();
        console.log(`  ✓ ${description}`);
        this.passed++;
      } catch (error) {
        console.log(`  ✗ ${description}`);
        console.log(`    Error: ${error.message}`);
        if (process.env.DEBUG) {
          console.log(error.stack);
        }
        this.failed++;
      }
    }

    return { passed: this.passed, failed: this.failed };
  }

  summary() {
    console.log(`\n${'-'.repeat(70)}`);
    console.log(`Results: ${this.passed} passed, ${this.failed} failed`);
    return this.failed === 0;
  }
}

export function assert(condition, message) {
  if (!condition) {
    throw new Error(message || 'Assertion failed');
  }
}

export function assertEqual(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(
      message || `Expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`
    );
  }
}

export function assertIncludes(text, substring, message) {
  if (!text.includes(substring)) {
    throw new Error(
      message || `Expected text to include "${substring}"\nGot: ${text.substring(0, 200)}...`
    );
  }
}

export function assertNotIncludes(text, substring, message) {
  if (text.includes(substring)) {
    throw new Error(
      message || `Expected text to NOT include "${substring}"`
    );
  }
}

export function assertMatches(text, pattern, message) {
  if (!pattern.test(text)) {
    throw new Error(
      message || `Expected text to match pattern ${pattern}\nGot: ${text.substring(0, 200)}...`
    );
  }
}

export function assertThrows(fn, expectedMessage, message) {
  try {
    fn();
    throw new Error(message || 'Expected function to throw');
  } catch (error) {
    if (expectedMessage && !error.message.includes(expectedMessage)) {
      throw new Error(
        `Expected error message to include "${expectedMessage}", got "${error.message}"`
      );
    }
  }
}
