// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test: iterating over globalThis properties should not crash.
// This tests that constructing and inspecting various global objects works correctly.
//
// Note: We skip `process` in the iteration because when nodejs_compat is enabled,
// the iteration logic may call process.exit() which terminates the test.
export const transformCrashRegression = {
  test() {
    function iterate(obj, depth = 0, results = {}, originalKey) {
      for (const key in obj) {
        if (depth > 100) return results;
        if (
          key === 'parent' ||
          key === 'globalThis' ||
          key === 'self' ||
          key === 'ServiceWorkerGlobalScope' ||
          key === 'global' ||
          key === 'process'
        )
          continue;
        if (typeof obj[key] === 'object') {
          results[key] = iterate(obj[key], ++depth, results[key] ?? {}, key);
        } else {
          const properties = new Set([
            ...Object.getOwnPropertyNames(obj[key]),
            ...Object.getOwnPropertySymbols(obj[key]),
            ...Object.keys(obj[key]),
          ]);
          for (const prop in obj[key]) {
            properties.add(prop);
          }
          if (properties.size > 0) {
            results[key] = {
              __proto: typeof obj[key].__proto__,
            };
            for (const property of properties) {
              if (
                property === 'caller' ||
                property === 'callee' ||
                property === 'arguments' ||
                property === 'constructor'
              )
                continue;
              let writeProp =
                property === 'prototype' ? '_prototype' : property;
              try {
                if (typeof obj[key][property] === 'object') {
                  results[key][writeProp] = iterate(
                    obj[key][property],
                    ++depth,
                    results[key][property] ?? {},
                    key
                  );
                } else {
                  results[key][writeProp] = typeof obj[key][property];
                }
              } catch (_err) {
                // expected
              }
              let instance;
              try {
                if (property === 'prototype') {
                  try {
                    instance = new obj[key]();
                  } catch (internalConstructError) {
                    if (
                      internalConstructError.message.includes(
                        'Failed to construct'
                      ) ||
                      internalConstructError.message.includes(
                        'Illegal constructor'
                      )
                    ) {
                      instance = new obj[key]('');
                    } else {
                      throw internalConstructError;
                    }
                  }
                } else {
                  instance = new obj[key][property]();
                }
                const instanceProperties = new Set([
                  ...Object.getOwnPropertyNames(instance),
                  ...Object.getOwnPropertySymbols(instance),
                  ...Object.keys(instance),
                  ...(instance.constructor
                    ? Object.getOwnPropertyNames(instance.constructor)
                    : []),
                  ...(instance.constructor
                    ? Object.getOwnPropertySymbols(instance.constructor)
                    : []),
                  ...(instance.constructor
                    ? Object.keys(instance.constructor)
                    : []),
                ]);
                for (const prop in instance) {
                  instanceProperties.add(prop);
                }
                if (instanceProperties.size > 0) {
                  if (results[key][writeProp] === undefined) {
                    results[key][writeProp] = {};
                  }
                  for (const instanceProperty of instanceProperties) {
                    try {
                      if (
                        results[key][writeProp][instanceProperty] === undefined
                      ) {
                        results[key][writeProp][instanceProperty] =
                          typeof instance[instanceProperty];
                      }
                    } catch {
                      // intentionally empty
                    }
                  }
                } else {
                  if (results[key][writeProp] === undefined) {
                    results[key][writeProp] = typeof instance;
                  }
                }
              } catch (_e) {
                if (results[key][writeProp] === undefined) {
                  results[key][writeProp] = typeof obj[key][property];
                }
                continue;
              }
            }
          } else {
            results[key] = typeof obj[key];
          }
        }
      }
      return results;
    }

    function order(obj) {
      if (typeof obj === 'object') {
        const ordered = {};
        Object.keys(obj)
          .sort()
          .forEach(function (key) {
            ordered[key] = order(obj[key]);
          });
        return ordered;
      } else {
        return obj;
      }
    }

    const iterated = iterate(globalThis);
    const ordered = order(iterated);
    const toWrite = JSON.stringify(ordered);

    // Test passes if we get here without crashing
    ok(toWrite.length > 0);
  },
};
