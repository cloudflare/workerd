import { defineConfig } from "eslint/config";
import eslint from '@eslint/js';
import tseslint from 'typescript-eslint';
import customRules from './custom-eslint-rules.mjs';

// Web platform and Workers runtime globals available in the execution
// environment. Listed here so `no-undef` stays enabled for real typos.
const workerdGlobals = {
  // Web platform
  AbortController: 'readonly',
  AbortSignal: 'readonly',
  Blob: 'readonly',
  ByteLengthQueuingStrategy: 'readonly',
  CloseEvent: 'readonly',
  CompressionStream: 'readonly',
  CountQueuingStrategy: 'readonly',
  CryptoKey: 'readonly',
  CustomEvent: 'readonly',
  DecompressionStream: 'readonly',
  DOMException: 'readonly',
  Event: 'readonly',
  EventSource: 'readonly',
  EventTarget: 'readonly',
  ExtendableEvent: 'readonly',
  File: 'readonly',
  FileSystemDirectoryHandle: 'readonly',
  FileSystemFileHandle: 'readonly',
  FileSystemWritableFileStream: 'readonly',
  FormData: 'readonly',
  Headers: 'readonly',
  HTMLRewriter: 'readonly',
  MessageChannel: 'readonly',
  MessageEvent: 'readonly',
  MessagePort: 'readonly',
  Navigator: 'readonly',
  ReadableByteStreamController: 'readonly',
  ReadableStream: 'readonly',
  ReadableStreamBYOBReader: 'readonly',
  ReadableStreamBYOBRequest: 'readonly',
  ReadableStreamDefaultController: 'readonly',
  ReadableStreamDefaultReader: 'readonly',
  Request: 'readonly',
  Response: 'readonly',
  StorageManager: 'readonly',
  TextDecoder: 'readonly',
  TextDecoderStream: 'readonly',
  TextEncoder: 'readonly',
  TextEncoderStream: 'readonly',
  TransformStream: 'readonly',
  URL: 'readonly',
  URLPattern: 'readonly',
  URLSearchParams: 'readonly',
  WebAssembly: 'readonly',
  WebSocket: 'readonly',
  WebSocketPair: 'readonly',
  WritableStream: 'readonly',
  WritableStreamDefaultController: 'readonly',
  WritableStreamDefaultWriter: 'readonly',
  // Workers-specific
  Cloudflare: 'readonly',
  FixedLengthStream: 'readonly',
  IdentityTransformStream: 'readonly',
  ScheduledController: 'readonly',
  // Node.js globals
  Buffer: 'readonly',
  global: 'readonly',
  process: 'readonly',
  // Web APIs
  addEventListener: 'readonly',
  atob: 'readonly',
  btoa: 'readonly',
  caches: 'readonly',
  console: 'readonly',
  crypto: 'readonly',
  dispatchEvent: 'readonly',
  fetch: 'readonly',
  gc: 'readonly',
  navigator: 'readonly',
  performance: 'readonly',
  queueMicrotask: 'readonly',
  removeEventListener: 'readonly',
  reportError: 'readonly',
  scheduler: 'writable',
  self: 'readonly',
  setTimeout: 'readonly',
  setInterval: 'readonly',
  clearImmediate: 'readonly',
  clearTimeout: 'readonly',
  clearInterval: 'readonly',
  setImmediate: 'readonly',
  structuredClone: 'readonly',
};

/**
 * @returns {import('eslint/config').Config}
 */
export function baseConfig() {
  return defineConfig([
    eslint.configs.recommended,
    ...tseslint.configs.strictTypeChecked,
    {
      plugins: {
        workerd: customRules,
      },
      rules: {
        'workerd/require-copyright-header': 'error',
      },
    },
    {
      languageOptions: {
        globals: workerdGlobals,
        parserOptions: {
          ecmaVersion: 'latest',
          sourceType: 'module',
          projectService: true,
          tsconfigRootDir: import.meta.dirname,
          jsDocParsingMode: 'all',
        },
      },
      rules: {
        '@typescript-eslint/explicit-function-return-type': 'error',
        '@typescript-eslint/explicit-member-accessibility': [
          'error',
          { accessibility: 'no-public' },
        ],
        '@typescript-eslint/explicit-module-boundary-types': 'off',
        '@typescript-eslint/no-require-imports': 'error',
        '@typescript-eslint/prefer-enum-initializers': 'error',
        '@typescript-eslint/restrict-template-expressions': 'off',
        '@typescript-eslint/no-non-null-assertion': 'error',
        '@typescript-eslint/no-extraneous-class': 'off',
        '@typescript-eslint/unified-signatures': 'off',
        '@typescript-eslint/no-unused-vars': [
          'error',
          {
            args: 'all',
            argsIgnorePattern: '^_',
            caughtErrors: 'all',
            caughtErrorsIgnorePattern: '^_',
            destructuredArrayIgnorePattern: '^_',
            varsIgnorePattern: '^_',
            ignoreRestSiblings: true,
          },
        ],
        'no-restricted-syntax': [
          'error',
          {
            selector: "MethodDefinition[accessibility='private']",
            message:
              "Use private field syntax (#) instead of 'private' keyword for methods",
          },
          {
            selector:
              "PropertyDefinition[accessibility='private']:not([computed=true])",
            message:
              "Use private field syntax (#) instead of 'private' keyword for simple properties",
          },
          {
            selector: "TSParameterProperty[accessibility='private']",
            message:
              "Use private field syntax (#) instead of 'private' keyword for constructor parameters",
          },
        ],
      },
    },
    {
      files: ['**/*.js', '**/*.mjs', '**/*.cjs'],
      ...tseslint.configs.disableTypeChecked,
    },
    {
      files: ['**/*.js', '**/*.mjs', '**/*.cjs'],
      rules: {
        '@typescript-eslint/explicit-function-return-type': 'off',
        // Handler signatures (request, env, ctx) are framework-dictated; leading
        // unused args are common and intentional in JS.
        '@typescript-eslint/no-unused-vars': [
          'error',
          {
            args: 'none',
            caughtErrors: 'all',
            caughtErrorsIgnorePattern: '^_',
            destructuredArrayIgnorePattern: '^_',
            varsIgnorePattern: '^_',
            ignoreRestSiblings: true,
          },
        ],
      },
    },
  ]);
}

export default baseConfig();
