// Enhanced REPRL worker script with all API bindings exposed
import { default as Stdin } from "workerd:stdin";
import crypto from 'crypto';
import * as fs from 'node:fs';
import { Buffer } from 'node:buffer';
import { ok, match, rejects, strictEqual, throws, deepStrictEqual, notStrictEqual } from 'node:assert';
import { mock } from 'node:test';
import { Writable } from 'node:stream';
import { EventEmitter } from 'node:events';
import { env } from "node:process";

// Expose all APIs to the global scope for fuzzing
export default {
  async fetch(request, env, ctx) {
    return new Response("REPRL mode active");
  },

  async test(request, env, ctx) {
    // Expose bindings
    //globalThis.CONSUMER_FETCH = env.CONSUMER.fetch;

    globalThis.env = env;
    process = undefined;
    globalThis.process = undefined;
    
    // used for scheduler.await
    globalThis.scheduler = scheduler;

    // Expose Web Crypto API
    globalThis.crypto = crypto;

    // Expose Web Standards
    globalThis.Response = Response;
    globalThis.Request = Request;
    globalThis.Headers = Headers;
    globalThis.URL = URL;
    globalThis.URLPattern = URLPattern;
    globalThis.URLSearchParams = URLSearchParams;
    globalThis.TextEncoder = TextEncoder;
    globalThis.TextDecoder = TextDecoder;

    // Expose streams
    globalThis.ReadableStream = ReadableStream;
    globalThis.WritableStream = WritableStream;
    globalThis.TransformStream = TransformStream;
    globalThis.ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;
    globalThis.CountQueuingStrategy = CountQueuingStrategy;

    // Expose fetch API
    globalThis.fetch = fetch;

    // Expose WebSocket and EventSource APIs
    globalThis.WebSocket = WebSocket;
    globalThis.EventSource = EventSource;

    // Expose Encoding APIs
    globalThis.atob = atob;
    globalThis.btoa = btoa;

    // Expose Blob and FormData APIs
    globalThis.Blob = Blob;
    globalThis.File = File;
    globalThis.FormData = FormData;

    // Expose HTML Rewriter
    globalThis.HTMLRewriter = HTMLRewriter;

    // Expose MessageChannel APIs
    globalThis.MessageChannel = MessageChannel;
    globalThis.MessagePort = MessagePort;

    // Expose AbortController for fetch
    globalThis.AbortController = AbortController;
    globalThis.AbortSignal = AbortSignal;

    // Expose Node.js APIs
    globalThis.fs = fs;
    globalThis.Buffer = Buffer;
    globalThis.Writable = Writable;
    globalThis.EventEmitter = EventEmitter;

    // Expose testing APIs
    globalThis.ok = ok;
    globalThis.match = match;
    globalThis.rejects = rejects;
    globalThis.throws = throws;
    globalThis.strictEqual = strictEqual;
    globalThis.deepStrictEqual = deepStrictEqual;
    globalThis.notStrictEqual = notStrictEqual;
    globalThis.mock = mock;

    // Mock Cloudflare APIs for fuzzing
    globalThis.MOCK_KV = {
      get: (key, options) => Promise.resolve(options?.type === 'json' ? { test: 'value' } : 'test-value'),
      put: (key, value, options) => Promise.resolve(),
      delete: (key) => Promise.resolve(),
      list: (options) => Promise.resolve({ keys: [{ name: 'key1' }], list_complete: true })
    };

    globalThis.MOCK_D1 = {
      prepare: (query) => ({
        bind: (...params) => ({
          first: () => Promise.resolve({ id: 1, name: 'test' }),
          all: () => Promise.resolve({ results: [{ id: 1 }], meta: {} }),
          run: () => Promise.resolve({ success: true, meta: { changes: 1 } })
        })
      }),
      batch: (stmts) => Promise.resolve(stmts.map(() => ({ success: true })))
    };

    globalThis.MOCK_R2 = {
      get: (key) => Promise.resolve({
        body: new ReadableStream(),
        text: () => Promise.resolve('content'),
        json: () => Promise.resolve({ data: 'test' }),
        arrayBuffer: () => Promise.resolve(new ArrayBuffer(8))
      }),
      put: (key, value, options) => Promise.resolve({ etag: 'test-etag' }),
      delete: (keys) => Promise.resolve({ deleted: Array.isArray(keys) ? keys.map(k => ({ key: k })) : [{ key: keys }] }),
      list: (options) => Promise.resolve({ objects: [{ key: 'test.txt', size: 100 }] })
    };

    // Enter the REPRL loop
    Stdin.reprl();
  }
};
