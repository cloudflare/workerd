// index.mjs
import { Buffer } from "node:buffer";

const encoder = new TextEncoder();

async function pbkdf2Derive(password) {
  const passwordArray = encoder.encode(password);
  const passwordKey = await crypto.subtle.importKey(
    "raw", passwordArray, "PBKDF2", false, ["deriveBits"]
  );
  const saltArray = crypto.getRandomValues(new Uint8Array(16));
  const keyBuffer = await crypto.subtle.deriveBits(
    { name: "PBKDF2", hash: "SHA-256", salt: saltArray, iterations: 1_000_000 },
    passwordKey, 256
  );
  return Buffer.from(keyBuffer).toString("base64");
}

export default {
  async fetch(request, env, ctx) {
    return new Response(await pbkdf2Derive("hello!"));
  }
}
