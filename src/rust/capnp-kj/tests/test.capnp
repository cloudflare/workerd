# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xd6a2c5f1e8b94a37;

# The interface capnp-kj's tests serve and call in both directions.

interface TestEcho {
  echo @0 (text :Text) -> (text :Text);
  # Returns `text`, with the server's prefix.

  fail @1 (description :Text) -> ();
  # Fails with `description`.

  child @2 (prefix :Text) -> (child :TestEcho);
  # Returns a server whose prefix is this server's prefix followed by `prefix`. For returned
  # capabilities and pipelining.

  callBack @3 (callee :TestEcho, text :Text) -> (text :Text);
  # Returns `callee.echo(text)`. For capabilities in params.

  write @4 (data :Data) -> stream;
  # Appends `data` to what `written()` returns.

  written @5 () -> (data :Data);
  # Returns everything written so far.

  hang @6 () -> ();
  # Never completes. For cancellation.
}
