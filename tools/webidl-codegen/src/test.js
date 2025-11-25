#!/usr/bin/env node

/**
 * Test runner for WebIDL code generator
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { parseWebIDL } from './parser.js';
import { CppGenerator } from './generator.js';
import { ImplGenerator } from './impl-generator.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const examplesDir = path.join(__dirname, '..', 'examples');

function runTest(name, idlFile, testImpl = false) {
  console.log(`\n${'='.repeat(60)}`);
  console.log(`Testing: ${name}`);
  console.log('='.repeat(60));

  try {
    const idlPath = path.join(examplesDir, idlFile);
    const idlContent = fs.readFileSync(idlPath, 'utf-8');

    console.log('\nInput WebIDL:');
    console.log('-'.repeat(60));
    console.log(idlContent);

    const definitions = parseWebIDL(idlContent);
    const generator = new CppGenerator();
    const code = generator.generate(definitions, {
      namespace: 'workerd::api',
      filename: idlFile.replace('.webidl', ''),
    });

    console.log('\nGenerated C++ Header:');
    console.log('-'.repeat(60));
    console.log(code);

    if (testImpl) {
      const implGenerator = new ImplGenerator();
      const implCode = implGenerator.generate(definitions, {
        namespace: 'workerd::api',
        headerFile: idlFile.replace('.webidl', '.h'),
      });

      console.log('\nGenerated C++ Implementation Stubs:');
      console.log('-'.repeat(60));
      console.log(implCode);
    }

    console.log(`\n✓ ${name} passed`);
    return true;
  } catch (error) {
    console.error(`\n✗ ${name} failed:`);
    console.error(`  ${error.message}`);
    if (process.env.DEBUG) {
      console.error(error.stack);
    }
    return false;
  }
}

function main() {
  console.log('WebIDL to JSG Code Generator - Test Suite');

  const tests = [
    ['TextEncoder (Simple Interface)', 'text-encoder.webidl', false],
    ['WritableStreamDefaultWriter (Complex Interface)', 'writable-stream.webidl', false],
    ['Dictionaries and Callbacks', 'example-dict.webidl', false],
    ['Simple Calculator (with Implementation Stubs)', 'simple.webidl', true],
  ];

  let passed = 0;
  let failed = 0;

  for (const [name, file, testImpl] of tests) {
    if (runTest(name, file, testImpl)) {
      passed++;
    } else {
      failed++;
    }
  }

  console.log('\n' + '='.repeat(60));
  console.log('Test Summary');
  console.log('='.repeat(60));
  console.log(`Passed: ${passed}`);
  console.log(`Failed: ${failed}`);
  console.log(`Total:  ${passed + failed}`);

  process.exit(failed > 0 ? 1 : 0);
}

main();
