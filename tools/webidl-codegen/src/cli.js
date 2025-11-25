#!/usr/bin/env node

/**
 * CLI tool for WebIDL to JSG code generation
 */

import fs from 'fs';
import path from 'path';
import { parseWebIDL } from './parser.js';
import { CppGenerator } from './generator.js';
import { ImplGenerator } from './impl-generator.js';
import { ProtectedRegions } from './protected-regions.js';

function printSummary(headerSummary, implSummary) {
  console.log('\n' + '='.repeat(60));
  console.log('Generation Summary');
  console.log('='.repeat(60));

  // Header generation summary
  console.log('\nHeader:');
  if (headerSummary.interfaces.length > 0) {
    console.log(`  ✓ ${headerSummary.interfaces.length} interface(s): ${headerSummary.interfaces.join(', ')}`);
  }
  if (headerSummary.mixins.length > 0) {
    console.log(`  ✓ ${headerSummary.mixins.length} mixin(s): ${headerSummary.mixins.join(', ')}`);
  }
  if (headerSummary.dictionaries.length > 0) {
    console.log(`  ✓ ${headerSummary.dictionaries.length} dictionar(y/ies): ${headerSummary.dictionaries.join(', ')}`);
  }
  if (headerSummary.enums.length > 0) {
    console.log(`  ✓ ${headerSummary.enums.length} enum(s): ${headerSummary.enums.join(', ')}`);
  }
  if (headerSummary.typedefs.length > 0) {
    console.log(`  ✓ ${headerSummary.typedefs.length} typedef(s): ${headerSummary.typedefs.join(', ')}`);
  }
  if (headerSummary.callbacks.length > 0) {
    console.log(`  ✓ ${headerSummary.callbacks.length} callback(s): ${headerSummary.callbacks.join(', ')}`);
  }

  // Implementation generation summary
  if (implSummary) {
    console.log('\nImplementation:');
    if (implSummary.implementations.length > 0) {
      console.log(`  ✓ ${implSummary.implementations.length} implementation(s): ${implSummary.implementations.join(', ')}`);
    }
    if (implSummary.skipped.length > 0) {
      console.log(`  ⊘ ${implSummary.skipped.length} skipped: ${implSummary.skipped.map(s => s.name).join(', ')}`);
    }
  }

  // Skipped items
  if (headerSummary.skipped.length > 0) {
    console.log('\nSkipped (--skip-interface):');
    for (const item of headerSummary.skipped) {
      console.log(`  ⊘ ${item.type}: ${item.name}`);
    }
  }

  // Unsupported items
  if (headerSummary.unsupported.length > 0) {
    console.log('\nUnsupported (not implemented):');
    for (const item of headerSummary.unsupported) {
      console.log(`  ✗ ${item.type}: ${item.name} - ${item.reason}`);
    }
  }

  const totalGenerated = headerSummary.interfaces.length +
                         headerSummary.mixins.length +
                         headerSummary.dictionaries.length +
                         headerSummary.enums.length +
                         headerSummary.typedefs.length +
                         headerSummary.callbacks.length;
  const totalSkipped = headerSummary.skipped.length;
  const totalUnsupported = headerSummary.unsupported.length;

  console.log('\n' + '-'.repeat(60));
  console.log(`Total: ${totalGenerated} generated, ${totalSkipped} skipped, ${totalUnsupported} unsupported`);
  console.log('='.repeat(60) + '\n');
}

function printUsage() {
  console.log(`
Usage: webidl-codegen [options] <input.webidl>

Options:
  -o, --output <file>       Output file (default: stdout)
  -n, --namespace <ns>      C++ namespace (default: workerd::api)
  --impl <file>             Generate implementation stub file
  --header <file>           Header file to include in implementation (auto-detected if not specified)
  --skip-interface <name>   Skip generation for specific interface (can be used multiple times)
  --update                  Update mode: preserve manual sections in existing files
  --incremental             Incremental mode: only generate stubs for new methods (requires --update)
  -h, --help                Show this help message

Examples:
  # Generate header only
  webidl-codegen -o generated.h example.webidl

  # Generate header and implementation stubs
  webidl-codegen -o generated.h --impl generated.c++ example.webidl

  # Update existing files while preserving manual sections
  webidl-codegen -o generated.h --update example.webidl

  # Incremental update: only add stubs for new methods
  webidl-codegen -o api.h --impl api.c++ --update --incremental api.webidl

  # Specify custom header include
  webidl-codegen -o api.h --impl api.c++ --header "workerd/api/api.h" example.webidl

  # Custom namespace
  webidl-codegen -n workerd::api::streams -o streams.h --impl streams.c++ streams.webidl

  # Skip specific interfaces (e.g., Window)
  webidl-codegen -o fetch.h --impl fetch.c++ --skip-interface Window fetch.webidl
`);
}

function main() {
  const args = process.argv.slice(2);

  if (args.length === 0 || args.includes('-h') || args.includes('--help')) {
    printUsage();
    process.exit(0);
  }

  let inputFile = null;
  let outputFile = null;
  let implFile = null;
  let headerFile = null;
  let namespace = 'workerd::api';
  let updateMode = false;
  let incrementalMode = false;
  let skipInterfaces = [];

  // Parse arguments
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];

    if (arg === '-o' || arg === '--output') {
      outputFile = args[++i];
    } else if (arg === '-n' || arg === '--namespace') {
      namespace = args[++i];
    } else if (arg === '--impl') {
      implFile = args[++i];
    } else if (arg === '--header') {
      headerFile = args[++i];
    } else if (arg === '--skip-interface') {
      skipInterfaces.push(args[++i]);
    } else if (arg === '--update') {
      updateMode = true;
    } else if (arg === '--incremental') {
      incrementalMode = true;
    } else if (arg.startsWith('-')) {
      console.error(`Unknown option: ${arg}`);
      process.exit(1);
    } else {
      inputFile = arg;
    }
  }

  if (!inputFile) {
    console.error('Error: No input file specified');
    printUsage();
    process.exit(1);
  }

  try {
    // Read input file
    const idlContent = fs.readFileSync(inputFile, 'utf-8');

    // Parse WebIDL
    const definitions = parseWebIDL(idlContent);

    // If update mode, parse existing file to preserve manual sections
    let protectedRegions = null;
    if (updateMode && outputFile) {
      protectedRegions = new ProtectedRegions();
      protectedRegions.parseFile(outputFile);
    }

    // Generate C++ header code
    const generator = new CppGenerator({ skipInterfaces });
    if (protectedRegions) {
      generator.setProtectedRegions(protectedRegions);
    }
    const filename = path.basename(inputFile, '.webidl');
    const code = generator.generate(definitions, {
      namespace,
      filename,
    });

    // Write header output
    if (outputFile) {
      fs.writeFileSync(outputFile, code);
      if (updateMode && protectedRegions) {
        console.log(`Updated ${outputFile} (preserved manual sections)`);
      } else {
        console.log(`Generated ${outputFile}`);
      }
    } else {
      console.log(code);
    }

    // Collect summary information
    const headerSummary = generator.getSummary();
    let implSummary = null;

    // Generate implementation stubs if requested
    if (implFile) {
      // Parse existing implementation file in update mode
      let implProtectedRegions = null;
      if (updateMode && implFile) {
        implProtectedRegions = new ProtectedRegions();
        implProtectedRegions.parseFile(implFile);
      }

      const implGenerator = new ImplGenerator({ skipInterfaces });
      if (implProtectedRegions) {
        implGenerator.setProtectedRegions(implProtectedRegions);
      }
      if (incrementalMode) {
        implGenerator.setIncrementalMode(true);
      }

      // Auto-detect header file if not specified
      const implHeaderFile = headerFile || (outputFile ? path.basename(outputFile) : `${filename}.h`);

      const implCode = implGenerator.generate(definitions, {
        namespace,
        headerFile: implHeaderFile,
      });

      fs.writeFileSync(implFile, implCode);
      if (updateMode && implProtectedRegions) {
        console.log(`Updated ${implFile} (preserved manual sections)`);
      } else {
        console.log(`Generated ${implFile}`);
      }

      implSummary = implGenerator.getSummary();
    }

    // Print generation summary
    printSummary(headerSummary, implSummary);
  } catch (error) {
    console.error(`Error: ${error.message}`);
    if (process.env.DEBUG) {
      console.error(error.stack);
    }
    process.exit(1);
  }
}

main();
