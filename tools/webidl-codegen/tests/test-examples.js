#!/usr/bin/env node

/**
 * Test all example WebIDL files
 */

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { parseWebIDL } from '../src/parser.js';
import { CppGenerator } from '../src/generator.js';
import { ImplGenerator } from '../src/impl-generator.js';
import { TestRunner, assert, assertIncludes } from './test-helpers.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const examplesDir = path.join(__dirname, '..', 'examples');

const runner = new TestRunner('Example Files Generation Tests');

// Get all example files
const exampleFiles = fs.readdirSync(examplesDir)
  .filter(f => f.endsWith('.webidl'))
  .sort();

console.log(`Found ${exampleFiles.length} example files to test`);

for (const file of exampleFiles) {
  runner.test(`${file} - Header generation`, () => {
    const idlPath = path.join(examplesDir, file);
    const idlContent = fs.readFileSync(idlPath, 'utf-8');
    const definitions = parseWebIDL(idlContent);
    const generator = new CppGenerator();
    const code = generator.generate(definitions, {
      namespace: 'workerd::api',
      filename: file.replace('.webidl', ''),
    });

    assert(code.length > 0, 'Generated code should not be empty');
    assertIncludes(code, '#pragma once', 'Should have pragma once');
    assertIncludes(code, 'namespace workerd::api', 'Should have namespace');
    assertIncludes(code, '}  // namespace workerd::api', 'Should close namespace');
  });

  runner.test(`${file} - Implementation generation`, () => {
    const idlPath = path.join(examplesDir, file);
    const idlContent = fs.readFileSync(idlPath, 'utf-8');
    const definitions = parseWebIDL(idlContent);
    const implGenerator = new ImplGenerator();
    const code = implGenerator.generate(definitions, {
      namespace: 'workerd::api',
      headerFile: file.replace('.webidl', '.h'),
    });

    assert(code.length > 0, 'Generated implementation should not be empty');
    assertIncludes(code, 'namespace workerd::api', 'Should have namespace');
    assertIncludes(code, '}  // namespace workerd::api', 'Should close namespace');
  });

  runner.test(`${file} - Summary generation`, () => {
    const idlPath = path.join(examplesDir, file);
    const idlContent = fs.readFileSync(idlPath, 'utf-8');
    const definitions = parseWebIDL(idlContent);
    const generator = new CppGenerator();
    generator.generate(definitions, {
      namespace: 'workerd::api',
      filename: file.replace('.webidl', ''),
    });

    const summary = generator.getSummary();
    assert(summary, 'Should have summary');
    assert(Array.isArray(summary.interfaces), 'Summary should have interfaces array');
    assert(Array.isArray(summary.mixins), 'Summary should have mixins array');
    assert(Array.isArray(summary.dictionaries), 'Summary should have dictionaries array');
    assert(Array.isArray(summary.enums), 'Summary should have enums array');
    assert(Array.isArray(summary.typedefs), 'Summary should have typedefs array');
    assert(Array.isArray(summary.callbacks), 'Summary should have callbacks array');
    assert(Array.isArray(summary.skipped), 'Summary should have skipped array');
    assert(Array.isArray(summary.unsupported), 'Summary should have unsupported array');
  });
}

const results = await runner.run();
const success = runner.summary();

process.exit(success ? 0 : 1);
