// The following test crashes on ASAN with segfault.

// service leak-fetch-test: Uncaught SyntaxError: 'super' keyword unexpected here
//   at worker:6:10
// *** Received signal #11: Segmentation fault
// stack: /lib/x86_64-linux-gnu/libc.so.6@4532f src/workerd/server/workerd@9d3bde1 src/workerd/server/workerd@2eb1b1d src/workerd/server/workerd@2f07d40 src/workerd/server/workerd@2eab5b8 src/workerd/server/workerd@2eaaba4 src/workerd/server/workerd@9e6aa11 src/workerd/server/workerd@9e7373c src/workerd/server/workerd@9e6941e src/workerd/server/workerd@9e7373c src/workerd/server/workerd@9e63a6d src/workerd/server/workerd@9e632b8 src/workerd/server/workerd@2e7ff3e /lib/x86_64-linux-gnu/libc.so.6@2a1c9 /lib/x86_64-linux-gnu/libc.so.6@2a28a src/workerd/server/workerd@2da1024
export const memoryLeak = {
  async test(_ctrl, env) {
    try {
      class Hello {
        constructor() {
          super();
        }
      }
      new Hello();
    } catch {}
  },
};
