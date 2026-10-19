# Experimental worker generations

A local Worker Loader can opt into a single-use child generation with synchronous shared memory and native termination. Run workerd with `--experimental` and enable `enable_worker_generations` on the parent. The runtime must advertise support for colocated generations. The open-source server rejects the capability in sandbox-enabled V8 builds because each worker has a separate isolate group.

```js
const generation = env.loader.createGeneration({ stamp: 36, marks: 12 });
const stamp = new Int32Array(generation.getSharedMemory('stamp'));
const worker = env.loader.load({
  compatibilityDate: '2026-10-01',
  mainModule: 'main.js',
  modules: { 'main.js': source },
  allowExperimental: true,
  generation,
});
```

The child receives the same backing stores as `env.stamp` and `env.marks`. Each worker creates its own typed-array views. Individual JavaScript `Atomics` operations provide synchronous visibility. Groups of cell operations do not form a transaction or a consistent snapshot. Blocking `Atomics.wait()` remains disabled. Names must not conflict with other environment bindings. A generation supports at most sixteen named memories totaling 65,536 bytes.

Call `terminate()` to request stop and join completion. `whenTerminated()` joins without requesting stop and can be called again after a canceled wait. Only a fulfilled join acknowledges that startup has ended and the isolate, its API state and owned asynchronous resources have been destroyed. A rejected or canceled wait supplies no acknowledgement. A host retaining capacity for the child must keep that capacity until a join fulfills.

The server runs parent and child JavaScript on one event-loop thread. A parent JavaScript callback cannot interrupt a child turn while that thread is busy. Before dispatch, the parent can arm `terminateAfter(milliseconds)` once, with a delay between zero and 60,000 milliseconds. Its native background task can stop synchronous execution without event-loop progress. The deadline is a stop request; a join still waits for cleanup.

A generation can be passed only to `load()`, once. It cannot expose Durable Object classes, reload silently, or restart after stopping. Create a fresh generation after completion. Old backing stores and stop handles remain attached to their original generation and cannot affect its successor. This capability is experimental local runtime support; it does not establish hosted availability.
