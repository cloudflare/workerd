// Enhanced REPRL worker script with all API bindings exposed
import { default as Stdin } from "workerd:stdin";
import crypto from 'crypto';
import * as fs from 'node:fs';


// TestDurableObject implementation
export class TestDurableObject {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.storage = state.storage;
  }

  async fetch(request) {
    return new Response("Durable Object endpoint");
  }
}

// Expose all APIs to the global scope for fuzzing
export default {
  async fetch(request, env, ctx) {
    return new Response("REPRL mode active");
  },
  
  async test() {
    // Initialize global variables to expose all APIs to the fuzzer
    globalThis.env = {
      // Standard bindings
      secret: secret,
      
      // Cache API
      CACHE: CACHE,
      caches: caches,
      
      // KV API
      KV_NAMESPACE: KV_NAMESPACE,
      
      // R2 API
      R2_BUCKET: R2_BUCKET,
      R2_ADMIN: R2_ADMIN,
      
      // Queue API
      QUEUE: QUEUE,
      
      // Analytics Engine API
      ANALYTICS: ANALYTICS,
      
      // D1 Database API
      DB: DB,
      
      // Durable Object API
      DURABLE_OBJECT: DURABLE_OBJECT,
      
      // Service binding
      SERVICE: SERVICE,
      
      // Hyperdrive binding
      HYPERDRIVE: HYPERDRIVE,
      
      // Email API
      SMTP: SMTP,
      
      // WebSocket API
      WEBSOCKET_SERVICE: WEBSOCKET_SERVICE
    };
    
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
    
    // Expose WebSocket API
    globalThis.WebSocket = WebSocket;
    
    // Expose Encoding APIs
    globalThis.atob = atob;
    globalThis.btoa = btoa;
    globalThis.fs = fs;
    
    // Expose HTML Rewriter
    globalThis.HTMLRewriter = HTMLRewriter;
    
    // Enter the REPRL loop
    Stdin.reprl();
  }
};
