# src/per_isolate/webfs/

TypeScript reimplementation of the File System Access API's writable stream,
needed when the `typescript_implemented_streams` compat flag is on. Parent
directory conventions (primordials discipline, private-brand dispatch, no
`instanceof`) apply — see `src/per_isolate/AGENTS.md`.

## WHY THIS EXISTS

`FileSystemWritableFileStream` is one of only two `WritableStream` subclasses in
the runtime. When the streams flag swaps `globalThis.WritableStream` for the
TypeScript class, a C++ subclass of the _C++_ `WritableStream` no longer passes
the brand checks used by `pipeTo`, and `instanceof WritableStream` becomes
false. Reimplementing the subclass in TypeScript is what restores the hierarchy.

The other subclass is `DigestStream` — see `src/per_isolate/crypto/AGENTS.md`.

## CONSTRUCTION

This class has no user-callable constructor: instances come from
`FileSystemFileHandle.createWritable()`, a JSG method on a C++ type. So `main.ts`
installs two things, and both are required — replacing only the global leaves
`createWritable()` handing back C++ instances, and replacing only the method
leaves `instanceof` comparing against the wrong class.

`createWritable` is exported from `writable-file-stream.ts` and installed with
the same property attributes `JSG_METHOD` uses, so the swap is invisible to
property introspection. `ts-webfs-writable-test.js`'s
`createWritableDescriptorMatchesJsg` holds that.

The install is also conditional on the webfs globals existing at all: the file
system is opt-in behind `enable_web_file_system`, and without it there is
nothing to replace.

## THE C++ CONTRACT

`writable-file-stream.ts` owns only the stream wiring. The file semantics are a
native `FileSystemWriteContextHandle` (`src/workerd/api/filesystem.h`), reached
through `utils.createFileSystemWriteContext(fileHandle, keepExistingData)`,
registered in `src/workerd/io/per-isolate-bootstrap.c++` via the deliberately
filesystem-free declaration in `src/workerd/api/filesystem-bootstrap.h`.

The same context backs both implementations, so the file semantics are not
written twice and cannot drift. `FileSystemWriteContextHandle::open()` is shared
by the C++ `createWritable()` and the bootstrap factory; it returns the
DOMException rather than throwing it, because the two callers need it in
different shapes (see below).

Parts of the contract that are easy to break:

- **`write()`, `seek()` and `truncate()` bypass the queue.** They reach the
  context directly, as the C++ methods do, so a write is complete when its
  promise settles rather than when the queue drains. Chunks written through a
  writer (or by `pipeTo`) go through the sink instead, which lands in the same
  context.
- **`write()` acquires and immediately releases a writer.** That looks pointless
  but is what makes a stream already locked to a writer fail, and it is the C++
  behavior. `seek()` and `truncate()` deliberately do not, so they succeed on a
  locked stream. `writeRejectsWhenLockedButSeekDoesNot` pins the asymmetry,
  including the message, which says "reader" though the stream is writable.
- **Strings are NOT encoded on the TypeScript side.** The file system writes
  them as WTF-8, which `TextEncoder` cannot produce. Forward them to the
  context, exactly as `crypto/digest-stream.ts` forwards to `update()`.
- **The two failure modes have different shapes.** JSG unwraps arguments before
  a method body runs, so a bad option bag is a synchronous throw while a file
  that cannot be opened is a rejection. `createWritable` is therefore
  deliberately **not** an `async function` — that would turn the former into a
  rejection too. It classifies by exception type: a DOMException becomes a
  rejection, anything else is rethrown. `badOptionsThrowSynchronously` and
  `createWritableRejectsForeignReceiver` hold the two apart.
- **The option bag is coerced, not type-checked.** JSG unwraps
  `jsg::Optional<bool>` with ToBoolean, so `{keepExistingData: 'false'}` opts
  _in_. The TypeScript side reproduces this with `!!`, held by
  `keepExistingDataIsCoerced`.
- **The write is transactional and holds a lock.** Writes land in a temporary
  file; `close()` replaces the original's contents and `abort()` discards them,
  and the VFS lock is held from `createWritable()` until one of them runs. A
  stream that is never closed nor aborted leaves the lock held until the context
  is collected.

## PARITY

Both implementations run `webfs-test.js` unmodified, under `webfs-test.wd-test`
(C++) and `webfs-ts-test.wd-test` (TypeScript). Behavior shared between them
belongs in that file so drift fails one config or the other. TypeScript-only
behavior belongs in `ts-webfs-writable-test.js`.

`ts-webfs-writable-test.js`'s `isRealWritableStreamSubclass` is the canary that
the `main.ts` install actually took effect; without it the rest of the suite
would pass against the C++ implementation too. `pipeToWorks`,
`methodsAreBrandChecked` and `hasCorrectStringTag` also fail against C++, so all
four are load-bearing.

The storage root is read-only. Tests that need to write must use the `tmp`
directory in it.

## INTENTIONAL DIVERGENCES

- **`Symbol.toStringTag` is set unconditionally.** JSG gates it on the
  `set_tostring_tag` compat flag, so a worker with a compatibility date before
  that flag's 2024-09-26 default sees a tag here where the C++ implementation
  exposes none. This matches the rest of the TypeScript streams implementation,
  which also sets it unconditionally.
- **The option bag is read before the receiver is checked**, the opposite of
  JSG's order. A call that is wrong in both ways reports the bag where C++
  reports the receiver. Both are synchronous TypeErrors, so only the message
  differs.

## ANTI-PATTERNS

- **NEVER** encode string chunks here; forward them to the context.
- **NEVER** reference `this` in the sink closures passed to `super()` — they are
  evaluated before `super()` returns, while `this` is still in its temporal dead
  zone. Capture the context in a local first.
- **NEVER** make `createWritable` an `async function`; it would collapse the two
  failure shapes into one.
- **NEVER** declare `type: 'bytes'` on the sink. `webstreams/writable.ts`
  rejects any non-undefined `type`, byte-oriented writable streams not being a
  thing in the standard.
