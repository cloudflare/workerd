// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/test.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::api {
namespace {

KJ_TEST("node:buffer import without capability") {
  KJ_EXPECT_LOG(ERROR, "script startup threw exception");

  try {
    TestFixture fixture({
      .mainModuleSource = R"SCRIPT(
        import { Buffer } from 'node:buffer';

        export default {
          fetch(request) {
            return new Response(new Buffer("test").toString());
          },
        };
      )SCRIPT"_kj});

    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getDescription() == "script startup threw exception"_kj);
  }
}

KJ_TEST("Verify maximum Buffer size") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer, kMaxLength } from 'node:buffer';

      try {
        Buffer.alloc(kMaxLength + 1);
        throw new Error('alloc should have failed');
      } catch (err) {
        if (!err.message.startsWith("The value of \"size\" is out of range"))
          throw err;
      }

      export default {
        fetch(request) {
          return new Response("test");
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Create 0-length buffers") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';
      export default {
        fetch(request) {
          Buffer.from('');
          Buffer.from('', 'ascii');
          Buffer.from('', 'latin1');
          Buffer.alloc(0);
          Buffer.allocUnsafe(0);
          new Buffer('');
          new Buffer('', 'ascii');
          new Buffer('', 'latin1');
          new Buffer('', 'binary');
          Buffer(0);
          return new Response("test");
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("new Buffer(string)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';
      export default {
        fetch(request) {
          const b = new Buffer("test");
          return new Response(b.toString("utf8"));
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.allocUnsafe(), Buffer.alloc(), Buffer.allocUnsafeSlow()") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          [
            'alloc',
            'allocUnsafe',
            'allocUnsafeSlow'
          ].forEach((alloc) => {
            const b = Buffer[alloc](1024);
            if (b.length !== 1024) {
              throw new Error(`Incorrect buffer length [${b.length}]`);
            }

            // In Node.js' implementation, the Buffer is sliced off a larger pool.
            // We don't do that, so the underlying ArrayBuffer length and offsets
            // should be what we expect.
            if (b.length !== b.buffer.byteLength) {
              throw new Error('b.buffer.byteLength does not match');
            }
            if (b.byteOffset !== 0) {
              throw new Error(`Incorrect b.byteOffset [${b.byteOffset}]`);
            }

            // In Node.js' implementation of allocUnsafe, and allocUnsafeSlow(),
            // the Buffer is filled with uninitialized memory. We don't do that,
            // so everything should be zeroes.
            for (const i of b) {
              if (i !== 0) {
                throw new Error(`Index should be zeroed out [${i}]`);
              }
            }

            b[0] = -1;
            if (b[0] !== 255) {
              throw new Error(`Incorrect index value [${b[0]}]`);
            }
          });

          return new Response("test");
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.from(string)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          return new Response(Buffer.from("test").toString());
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.from(string, 'utf8')") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          return new Response(Buffer.from("test", 'utf8').toString('utf8'));
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.from(string, 'ucs2')") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          return new Response(Buffer.from("test", 'ucs2').toString('ucs2'));
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.from(string, 'hex')") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          // Only the valid hex in the input will be decoded. Anything after
          // the first invalid hex pair will be ignored.
          const buf = Buffer.from("74657374 invalid from here", 'hex');
          if (buf.length !== 4) {
            throw new Error(`invalid buffer length [${buf.length}]`);
          }
          return new Response(Buffer.from("74657374 invalid from here", 'hex').toString());
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.from(string, 'base64')") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          // Invalid characters within the encoding are ignored...
          return new Response(Buffer.from("dGV^^^^^^zdA==", 'base64').toString());
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("new Buffer(string, 'base64')") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          return new Response(new Buffer("dGVzdA==", 'base64').toString());
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.from(string, 'base64url')") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          return new Response(Buffer.from("dGVzdA", 'base64').toString());
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("Buffer.from(Uint8Array)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          const u8 = new Uint8Array([74, 65, 73, 74]);
          const buffer = Buffer.from(u8);
          if (buffer.length !== 4) {
            throw new Error(`Unexpected buffer length [${buffer.length}]`);
          }
          if (buffer[0] !== 74) {
            throw new Error(`Unexpected buffer value [${buffer[0]}]`);
          }
          u8.fill(0);
          return new Response(buffer);
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "JAIJ");
}

KJ_TEST("new Buffer(Uint8Array)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          const u8 = new Uint8Array([74, 65, 73, 74]);
          const buffer = new Buffer(u8);
          if (buffer.length !== 4) {
            throw new Error(`Unexpected buffer length [${buffer.length}]`);
          }
          if (buffer[0] !== 74) {
            throw new Error(`Unexpected buffer value [${buffer[0]}]`);
          }
          u8.fill(0);
          return new Response(buffer);
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "JAIJ");
}

KJ_TEST("Buffer.from(Uint32Array)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          const u32 = new Uint32Array([1953719668]);
          const buffer = Buffer.from(u32);
          if (buffer.length !== 1) {
            throw new Error(`Unexpected buffer length [${buffer.length}]`);
          }
          if (buffer[0] !== 116) {
            throw new Error(`Unexpected buffer value [${buffer[0]}]`);
          }
          u32.fill(0);
          return new Response(buffer);
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "t");
}

KJ_TEST("Buffer.from(ArrayBuffer)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          const u32 = new Uint32Array([1953719668]);
          const buffer = Buffer.from(u32.buffer);
          if (buffer.length !== 4) {
            throw new Error(`Unexpected buffer length [${buffer.length}]`);
          }
          if (buffer[0] !== 116) {
            throw new Error(`Unexpected buffer value [${buffer[0]}]`);
          }
          return new Response(buffer);
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("new Buffer(ArrayBuffer)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          const u32 = new Uint32Array([1953719668]);
          const buffer = new Buffer(u32.buffer);
          if (buffer.length !== 4) {
            throw new Error(`Unexpected buffer length [${buffer.length}]`);
          }
          if (buffer[0] !== 116) {
            throw new Error(`Unexpected buffer value [${buffer[0]}]`);
          }
          return new Response(buffer);
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}


KJ_TEST("Buffer.prototype.indexOf/lastIndexOf") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {

          const b = Buffer.from('helloabcabcthere');

          if (b.indexOf('abc') !== 5) {
            throw new Error('Incorrect index');
          }

          if (b.indexOf('abc', -8) !== 8) {
            throw new Error('Incorrect index');
          }

          if (b.indexOf('abc', 6) !== 8) {
            throw new Error('Incorrect index');
          }

          if (b.lastIndexOf('abc') !== 8) {
            throw new Error('Incorrect last index');
          }

          if (b.indexOf(Buffer.from('abc')) !== 5) {
            throw new Error('Incorrect index');
          }

          return new Response('test');
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}


}  // namespace
}  // namespace workerd::api

