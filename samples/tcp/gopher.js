// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This is an ESM module implementing a simple Gopher client. Gopher is an old alternative to HTTP,
// it is also a simple protocol which makes it ideal for demoing TCP sockets in workerd.
//
// This module implements both direct and proxied TCP connections. In order to make use of the
// proxy you'll need to have an HTTP proxy running on port 1234 (this can be changed in config.capnp).
//
// Example usage once you have workerd running:
//
// - curl localhost:8080/ # Retrieve the main segment in gopher.floodgap.com.
// - curl localhost:8080/gstats # Retrieve the "gstats" segment.
// - curl localhost:8080/gstats?use_proxy=1 # Retrieve the "gstats" segment using the configured proxy.
import { connect } from 'cloudflare:sockets';

export default {
  async fetch(req, env) {
    const gopherAddr = "gopher.floodgap.com:70";

    const url = new URL(req.url);
    const useProxy = url.searchParams.has("use_proxy");

    try {
      const socket = useProxy ? env.proxy.connect(gopherAddr) : connect(gopherAddr);

      // Write data to the socket. Specifically the "selector" followed by CR+LF.
      const writer = socket.writable.getWriter()
      const encoder = new TextEncoder();
      const encoded = encoder.encode(url.pathname + "\r\n");
      await writer.write(encoded);

      // The Gopher server should now return a response to us and close the connection.
      // So start reading from the socket.
      const decoder = new TextDecoder();

      let gopherResponse = "";
      for await (const chunk of socket.readable) {
        gopherResponse += decoder.decode(chunk, { stream: true });
      }
      gopherResponse += decoder.decode();

      console.log("Read ", gopherResponse.length, " from Gopher server");

      return new Response(gopherResponse, { headers: { "Content-Type": "text/plain" } });
    } catch (error) {
      return new Response("Socket connection failed: " + error, { status: 500 });
    }
  }
};
