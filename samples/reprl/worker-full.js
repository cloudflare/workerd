// Enhanced REPRL worker script with all API bindings exposed
import { default as Stdin } from "workerd:stdin";
import crypto from 'crypto';
import * as fs from 'node:fs';

// Expose all APIs to the global scope for fuzzing
export default {
  async fetch(request, env, ctx) {
    return new Response("REPRL mode active");
  },
  
  async test() {
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
