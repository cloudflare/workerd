// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

#include <workerd/jsg/setup.h>
#include <workerd/jsg/snapshot.h>

#include <kj/map.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct SnapshotGlobalsContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(SnapshotGlobalsContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(SnapshotGlobalsIsolate, SnapshotGlobalsContext);

kj::Own<SnapshotGlobalsIsolate> newNormalIsolate() {
  return kj::heap<SnapshotGlobalsIsolate>(
      v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
}

// A zygote: an isolate built by a v8::SnapshotCreator.
kj::Own<SnapshotGlobalsIsolate> newZygote() {
  return kj::heap<SnapshotGlobalsIsolate>(v8System, newIsolateGroup(), nullptr,
      kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
      true, SnapshotConfig(MutableSnapshot{.artifact = kj::atomicRefcounted<SnapshotArtifact>()}));
}

// Creates a context in `isolate` and runs `fn` with it entered.
template <typename Fn>
void inNewContext(SnapshotGlobalsIsolate& isolate, Fn&& fn) {
  isolate.runInLockScope([&](SnapshotGlobalsIsolate::Lock& lock) {
    lock.withinHandleScope([&] {
      auto jsContext = lock.newContext<SnapshotGlobalsContext>();
      auto context = jsContext.getHandle(lock);
      JSG_WITHIN_CONTEXT_SCOPE(lock, context, [&](Lock& js) { fn(js, context); });
    });
  });
}

// Runs `source` as a classic script in the current context.
kj::String eval(Lock& js, kj::StringPtr source) {
  auto context = js.v8Context();
  auto script = check(v8::Script::Compile(context, v8Str(js.v8Isolate, source)));
  return JsValue(check(script->Run(context))).toString(js);
}

// Lists every property of every object reachable from globalThis through own data properties,
// one "<path>\t<kind>" line each, sorted. The kind is typeof for a primitive, "object" for an
// object or function, "accessor" for an accessor property and "throws" when reading the
// descriptor threw, which is what a native data property with a throwing getter does.
constexpr kj::StringPtr WALK_GLOBALS = R"js(
(() => {
  const seen = new Set([globalThis]);
  const queue = [[globalThis, 'globalThis']];
  const out = [];
  while (queue.length > 0) {
    const [obj, path] = queue.shift();
    for (const key of Reflect.ownKeys(obj)) {
      const childPath = path + '.' + (typeof key === 'symbol' ? key.toString() : key);
      let desc;
      try {
        desc = Object.getOwnPropertyDescriptor(obj, key);
      } catch {
        out.push(childPath + '\tthrows');
        continue;
      }
      if (!('value' in desc)) {
        out.push(childPath + '\taccessor');
        continue;
      }
      const value = desc.value;
      const isObject =
          typeof value === 'function' || (typeof value === 'object' && value !== null);
      out.push(childPath + '\t' + (isObject ? 'object' : typeof value));
      if (isObject && !seen.has(value)) {
        seen.add(value);
        queue.push([value, childPath]);
      }
    }
  }
  return out.sort().join('\n');
})()
)js"_kj;

kj::HashMap<kj::String, kj::String> parseWalk(kj::StringPtr text) {
  kj::HashMap<kj::String, kj::String> result;
  while (text.size() > 0) {
    auto lineEnd = text.findFirst('\n').orDefault(text.size());
    auto line = text.first(lineEnd);
    KJ_IF_SOME(tab, line.findFirst('\t')) {
      result.upsert(
          kj::str(line.first(tab)), kj::str(line.slice(tab + 1)), [](kj::String&, kj::String&&) {});
    }
    text = text.slice(kj::min(lineEnd + 1, text.size()));
  }
  return result;
}

kj::HashMap<kj::String, kj::String> walkNormalIsolate() {
  kj::String walk;
  auto isolate = newNormalIsolate();
  inNewContext(*isolate, [&](Lock& js, auto) { walk = eval(js, WALK_GLOBALS); });
  return parseWalk(walk);
}

// The walker's path of a table entry.
kj::String pathOf(const RestoreTimeGlobal& entry) {
  if (entry.holder == RestoreTimeHolder::GLOBAL) return kj::str("globalThis.", entry.name);
  return kj::str("globalThis.", jsNameOf(entry.holder), ".", entry.name);
}

kj::Maybe<const RestoreTimeGlobal&> findTableEntry(kj::StringPtr path) {
  for (auto& entry: RESTORE_TIME_GLOBALS) {
    if (pathOf(entry) == path) return entry;
  }
  return kj::none;
}

// Whether `path` is below a table entry: the normal isolate walks into WebAssembly and friends,
// the zygote cannot.
bool isBelowTableEntry(kj::StringPtr path) {
  for (auto& entry: RESTORE_TIME_GLOBALS) {
    if (path.startsWith(kj::str(pathOf(entry), "."))) return true;
  }
  return false;
}

constexpr kj::StringPtr STACK_TRACE_LIMIT_DESCRIPTOR =
    "JSON.stringify(Object.getOwnPropertyDescriptor(Error, 'stackTraceLimit'))"_kj;

KJ_TEST("a zygote context gets Error.stackTraceLimit as a normal isolate's context has it") {
  kj::String expected;
  {
    // V8's own Genesis::InstallSpecialObjects().
    auto normal = newNormalIsolate();
    inNewContext(
        *normal, [&](Lock& js, auto) { expected = eval(js, STACK_TRACE_LIMIT_DESCRIPTOR); });
  }
  KJ_EXPECT(expected == R"({"value":10,"writable":true,"enumerable":true,"configurable":true})"_kj,
      expected);

  auto zygote = newZygote();
  inNewContext(*zygote, [&](Lock& js, auto) {
    KJ_EXPECT(eval(js, STACK_TRACE_LIMIT_DESCRIPTOR) == expected);
    KJ_EXPECT(eval(js, "typeof new Error('probe').stack"_kj) == "string"_kj);
  });
}

KJ_TEST("a zygote context has every property of a normal context; the restore-time ones throw") {
  auto normal = walkNormalIsolate();
  kj::String walk;
  auto zygote = newZygote();
  inNewContext(*zygote, [&](Lock& js, auto) { walk = eval(js, WALK_GLOBALS); });
  auto zygoteProps = parseWalk(walk);

  for (auto& entry: normal) {
    if (isBelowTableEntry(entry.key)) continue;
    KJ_IF_SOME(kind, zygoteProps.find(entry.key)) {
      KJ_IF_SOME(row, findTableEntry(entry.key)) {
        if (row.kind == RestoreTimeKind::FAIL_CLOSED) {
          KJ_EXPECT(kind == "throws"_kj, entry.key, kind);
          continue;
        }
      }
      KJ_EXPECT(kind == entry.value, entry.key, entry.value, kind);
    } else {
      KJ_FAIL_EXPECT("property of a normal context missing from the zygote context", entry.key);
    }
  }
  for (auto& entry: zygoteProps) {
    if (isBelowTableEntry(entry.key)) continue;
    KJ_EXPECT(normal.find(entry.key) != kj::none, "property only in the zygote context", entry.key);
  }
}

KJ_TEST("after removal, exactly the properties V8 adds on restore are missing") {
  // A V8 roll that adds to or removes from what Genesis::InitializeExperimentalGlobal() and
  // Genesis::InstallSpecialObjects() install fails here: fix RESTORE_TIME_GLOBALS.
  auto normal = walkNormalIsolate();
  kj::String walk;
  auto zygote = newZygote();
  inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
    IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context);
    walk = eval(js, WALK_GLOBALS);
  });
  auto zygoteProps = parseWalk(walk);

  for (auto& entry: normal) {
    if (isBelowTableEntry(entry.key)) continue;
    bool inTable = findTableEntry(entry.key) != kj::none;
    KJ_IF_SOME(kind, zygoteProps.find(entry.key)) {
      KJ_EXPECT(!inTable, "restore-time property still present after removal", entry.key);
      KJ_EXPECT(kind == entry.value, entry.key, entry.value, kind);
    } else {
      KJ_EXPECT(
          inTable, "property missing from the zygote context but not in the table", entry.key);
    }
  }
  for (auto& entry: zygoteProps) {
    if (isBelowTableEntry(entry.key)) continue;
    KJ_EXPECT(normal.find(entry.key) != kj::none, "property only in the zygote context", entry.key);
  }
  KJ_EXPECT(zygote->mutableSnapshotArtifact().restoreTimeGlobalsIndex != kj::none);
}

KJ_TEST("fail-closed properties exist in the zygote but throw when used") {
  auto zygote = newZygote();
  inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
    auto attempt = [&](kj::StringPtr expression) {
      return eval(js,
          kj::str("(() => { 'use strict'; try { return String(", expression,
              "); } catch (e) { return String(e); } })()"));
    };
    KJ_EXPECT(attempt("typeof WebAssembly"_kj) ==
        "Error: snapshot PoC: globalThis.WebAssembly cannot be used while preparing a startup "
        "snapshot"_kj);
    KJ_EXPECT(attempt("typeof SharedArrayBuffer"_kj)
                  .contains("globalThis.SharedArrayBuffer cannot be used"_kj));
    KJ_EXPECT(
        attempt("Iterator.concat = () => 1"_kj).contains("Iterator.concat cannot be used"_kj));
    KJ_EXPECT(attempt("[].values().join"_kj).contains("Iterator.prototype.join cannot be used"_kj));
    // Defining over one reads its current value first, so a polyfill that does not delete it
    // first fails the same way.
    KJ_EXPECT(
        attempt("Object.defineProperty(Iterator, 'concat', {value() {}, configurable: true})"_kj)
            .contains("Iterator.concat cannot be used"_kj));
    // Present and, like V8's own, not enumerable.
    KJ_EXPECT(eval(js, "'WebAssembly' in globalThis"_kj) == "true"_kj);
    KJ_EXPECT(eval(js, "Object.hasOwn(globalThis, 'SharedArrayBuffer')"_kj) == "true"_kj);
    KJ_EXPECT(eval(js, "Object.keys(globalThis).includes('WebAssembly')"_kj) == "false"_kj);
    KJ_EXPECT(eval(js, "Object.hasOwn(Iterator.prototype, 'includes')"_kj) == "true"_kj);
    // Every attempt above caught its error; the uses were recorded all the same.
    KJ_EXPECT_THROW_MESSAGE("went on after the error",
        IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
  });
}

KJ_TEST("removal rejects a zygote whose top level went on after a fail-closed error") {
  constexpr kj::StringPtr WENT_ON = "went on after the error"_kj;
  auto rejects = [&](kj::StringPtr source) {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js, source);
      KJ_EXPECT_THROW_MESSAGE(WENT_ON,
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  };
  // A caught read: a feature test that bakes its wrong answer in.
  rejects("try { WebAssembly } catch {}"_kj);
  rejects("let t; try { t = typeof SharedArrayBuffer } catch { t = 'caught' }"_kj);
  // A caught write: a polyfill that gives up.
  rejects("try { Iterator.concat = () => 1 } catch {}"_kj);
  // A caught descriptor read goes through the getter too.
  rejects("try { Object.getOwnPropertyDescriptor(Iterator.prototype, 'join') } catch {}"_kj);
  // An error that rejected a promise instead of propagating: nothing threw synchronously.
  rejects("(async () => { WebAssembly })()"_kj);

  // A read from C++ under a v8::TryCatch, as a probe during context setup would do.
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      v8::TryCatch tryCatch(js.v8Isolate);
      KJ_EXPECT(
          context->Global()->Get(context, v8StrIntern(js.v8Isolate, "WebAssembly"_kj)).IsEmpty());
      tryCatch.Reset();
      KJ_EXPECT_THROW_MESSAGE(WENT_ON,
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }

  // Looking for a property without reading it is not a use: it is there on restore too.
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js,
          "'WebAssembly' in globalThis;"
          "Object.hasOwn(globalThis, 'SharedArrayBuffer');"
          "Object.keys(globalThis);"
          "Reflect.ownKeys(Iterator);"
          "Reflect.ownKeys(Iterator.prototype);"_kj);
      IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context);
    });
    KJ_EXPECT(zygote->mutableSnapshotArtifact().restoreTimeGlobalsIndex != kj::none);
  }
}

KJ_TEST("removal removes Error.stackTraceLimit and pins its final state") {
  auto zygote = newZygote();
  inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
    eval(js, "Error.stackTraceLimit = 50"_kj);
    IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context);
    KJ_EXPECT(eval(js, "Object.hasOwn(Error, 'stackTraceLimit')"_kj) == "false"_kj);
  });
  KJ_EXPECT(zygote->mutableSnapshotArtifact().restoreTimeGlobalsIndex != kj::none);
}

KJ_TEST("removal accepts properties the top level deleted") {
  auto zygote = newZygote();
  inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
    eval(js,
        "delete Error.stackTraceLimit;"
        "delete globalThis.WebAssembly;"
        "delete Iterator.prototype.join;"_kj);
    IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context);
    KJ_EXPECT(eval(js, "Object.hasOwn(Error, 'stackTraceLimit')"_kj) == "false"_kj);
    KJ_EXPECT(eval(js, "Object.hasOwn(globalThis, 'WebAssembly')"_kj) == "false"_kj);
  });
  KJ_EXPECT(zygote->mutableSnapshotArtifact().restoreTimeGlobalsIndex != kj::none);
}

KJ_TEST("removal works on the intrinsics even when their global bindings were replaced") {
  auto zygote = newZygote();
  inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
    eval(js,
        "const OriginalError = Error;"
        "const OriginalIterator = Iterator;"
        "globalThis.Error = function FakeError() {};"
        "globalThis.Iterator = function FakeIterator() {};"
        "OriginalError.stackTraceLimit = 7;"_kj);
    IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context);
    KJ_EXPECT(eval(js, "Object.hasOwn(OriginalError, 'stackTraceLimit')"_kj) == "false"_kj);
    KJ_EXPECT(eval(js, "Object.hasOwn(OriginalIterator, 'concat')"_kj) == "false"_kj);
    KJ_EXPECT(eval(js, "Object.hasOwn(OriginalIterator.prototype, 'join')"_kj) == "false"_kj);
  });
}

KJ_TEST("removal rejects an Error.stackTraceLimit V8 cannot re-add on restore") {
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      // On an existing property, defineProperty keeps the attributes it does not name.
      eval(js,
          "Object.defineProperty(Error, 'stackTraceLimit', {value: 5, configurable: false})"_kj);
      KJ_EXPECT_THROW_MESSAGE("non-configurable or an accessor",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js,
          "Object.defineProperty(Error, 'stackTraceLimit', "
          "{get() { return 5; }, configurable: true})"_kj);
      KJ_EXPECT_THROW_MESSAGE("non-configurable or an accessor",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
}

KJ_TEST("removal rejects a property the top level put in a fail-closed property's place") {
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js,
          "delete Iterator.concat;"
          "Object.defineProperty(Iterator, 'concat', "
          "{value() {}, writable: true, configurable: true})"_kj);
      KJ_EXPECT_THROW_MESSAGE("defined a property that V8 adds",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js, "delete globalThis.WebAssembly; globalThis.WebAssembly = {};"_kj);
      KJ_EXPECT_THROW_MESSAGE("defined a property that V8 adds",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js,
          "delete globalThis.SharedArrayBuffer;"
          "Object.defineProperty(globalThis, 'SharedArrayBuffer', "
          "{get() { throw new Error('disabled'); }, configurable: true})"_kj);
      KJ_EXPECT_THROW_MESSAGE("defined a property that V8 adds",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
}

KJ_TEST("removal rejects a non-extensible holder") {
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js, "Object.freeze(Error)"_kj);
      KJ_EXPECT_THROW_MESSAGE("non-extensible",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js, "Object.preventExtensions(globalThis)"_kj);
      KJ_EXPECT_THROW_MESSAGE("non-extensible",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
  {
    auto zygote = newZygote();
    inNewContext(*zygote, [&](Lock& js, v8::Local<v8::Context> context) {
      eval(js, "Object.seal(Iterator.prototype)"_kj);
      KJ_EXPECT_THROW_MESSAGE("non-extensible",
          IsolateBase::from(js.v8Isolate).removeRestoreTimeGlobalsBeforeSnapshot(js, context));
    });
  }
}

// =======================================================================================
// Live-wrappable transfer: the objects a zygote's top level leaves reachable, serialized into
// the artifact and attached to their restored wrappers in an isolate built from the blob.

// Transferred by tag alone; the restore constructs a fresh one.
struct StatelessThing: public Object {
  static constexpr bool jsgSnapshotStateless = true;
  inline static uint constructed = 0;

  StatelessThing() {
    ++constructed;
  }

  static Ref<StatelessThing> constructor(Lock& js) {
    return js.alloc<StatelessThing>();
  }

  int kind() {
    return 1;
  }

  JSG_RESOURCE_TYPE(StatelessThing) {
    JSG_METHOD(kind);
  }
};

// Transferred as a snapshotClone(); `value` must survive.
struct StatefulThing: public Object {
  inline static uint cloneCalls = 0;
  int value;

  explicit StatefulThing(int value): value(value) {}

  static Ref<StatefulThing> constructor(Lock& js, int value) {
    return js.alloc<StatefulThing>(value);
  }

  int getValue() {
    return value;
  }
  void setValue(int newValue) {
    value = newValue;
  }

  // Qualified: inside a subclass the bare name resolves through Object's private base.
  kj::Maybe<kj::Own<jsg::Wrappable>> snapshotClone() const override {
    ++cloneCalls;
    return ownAsWrappable(kj::refcounted<StatefulThing>(value));
  }

  // An instance the way a restore builds one, for the metadata test below.
  static kj::Own<jsg::Wrappable> freshForRestore(int value) {
    return ownAsWrappable(kj::refcounted<StatefulThing>(value));
  }

  JSG_RESOURCE_TYPE(StatefulThing) {
    JSG_METHOD(getValue);
    JSG_METHOD(setValue);
  }
};

// A subclass that inherits its base's snapshotClone(): the clone is a StatefulThing, which must
// not be attached to a StatefulSubThing wrapper.
struct StatefulSubThing: public StatefulThing {
  explicit StatefulSubThing(int value): StatefulThing(value) {}

  static Ref<StatefulSubThing> constructor(Lock& js, int value) {
    return js.alloc<StatefulSubThing>(value);
  }

  JSG_RESOURCE_TYPE(StatefulSubThing) {
    JSG_INHERIT(StatefulThing);
  }
};

// Neither: a context that reaches one cannot be snapshotted.
struct OpaqueThing: public Object {
  static Ref<OpaqueThing> constructor(Lock& js) {
    return js.alloc<OpaqueThing>();
  }

  JSG_RESOURCE_TYPE(OpaqueThing) {}
};

// Holds a JavaScript object through a member handle, as API types do; the clone drops it, as the
// clone contract requires. Once the top level has let go of its own references, the held object
// is reachable through C++ alone.
struct HolderThing: public Object {
  kj::Maybe<JsRef<JsObject>> held;

  static Ref<HolderThing> constructor(Lock& js) {
    return js.alloc<HolderThing>();
  }

  void hold(Lock& js, JsObject object) {
    held = JsRef<JsObject>(js, object);
  }

  kj::Maybe<kj::Own<jsg::Wrappable>> snapshotClone() const override {
    return ownAsWrappable(kj::refcounted<HolderThing>());
  }

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(held);
  }

  JSG_RESOURCE_TYPE(HolderThing) {
    JSG_METHOD(hold);
  }
};

struct SnapshotTransferContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(SnapshotTransferContext) {
    JSG_NESTED_TYPE(StatelessThing);
    JSG_NESTED_TYPE(StatefulThing);
    JSG_NESTED_TYPE(StatefulSubThing);
    JSG_NESTED_TYPE(OpaqueThing);
    JSG_NESTED_TYPE(HolderThing);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SnapshotTransferIsolate,
    SnapshotTransferContext,
    StatelessThing,
    StatefulThing,
    StatefulSubThing,
    OpaqueThing,
    HolderThing);

// Runs `topLevel` in a zygote's context and serializes the context the way Worker::Worker does:
// the serializer callbacks are registered with the context, the top level runs, the
// FinalizationRegistry cleanups it left due are settled, the restore-time globals are removed,
// the context handle is handed to createSnapshotBlob() and nothing else of the context is kept.
kj::Own<SnapshotArtifact> bakeSnapshot(kj::StringPtr topLevel) {
  auto zygote = kj::heap<SnapshotTransferIsolate>(v8System, newIsolateGroup(), nullptr,
      kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
      true, SnapshotConfig(MutableSnapshot{.artifact = kj::atomicRefcounted<SnapshotArtifact>()}));
  zygote->runInLockScope([&](SnapshotTransferIsolate::Lock& lock) {
    uint32_t badTagFieldsCount = 0;
    // Under the lock, as in Worker::Worker: the metadata roots the shims it records with
    // cppgc::Persistents, which must be freed on the thread that holds the isolate.
    WrappablesSerializeMetadata metadata;
    auto& isolateBase = IsolateBase::from(lock.v8Isolate);
    v8::Global<v8::Context> contextHandle;
    lock.withinHandleScope([&] {
      // With the WeakRef globals, for the FinalizationRegistry tests below.
      auto jsContext =
          lock.newContext<SnapshotTransferContext>(NewContextOptions{.enableWeakRef = true});
      auto context = jsContext.getHandle(lock);
      JSG_WITHIN_CONTEXT_SCOPE(lock, context, [&](Lock& js) {
        isolateBase.setSnapshotDefaultContext(context, &badTagFieldsCount, &metadata);
        eval(js, topLevel);
        isolateBase.settleFinalizationRegistriesForSnapshot(js);
        isolateBase.removeRestoreTimeGlobalsBeforeSnapshot(js, context);
      });
      contextHandle = jsContext.extractContextGlobalForSnapshot();
    });
    isolateBase.createSnapshotBlob(kj::mv(contextHandle), {}, &badTagFieldsCount, &metadata);
  });
  return zygote->extractSnapshotArtifact();
}

kj::Own<SnapshotTransferIsolate> newIsolateFromSnapshot(SnapshotArtifact& artifact) {
  return kj::heap<SnapshotTransferIsolate>(v8System, newIsolateGroup(), nullptr,
      kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
      true, SnapshotConfig(FinalizedSnapshot{.artifact = artifact.addRef()}));
}

// Creates a context in `isolate` and runs `fn` with it entered. In an isolate built from a
// snapshot the context comes out of the blob, with the transferred objects attached.
template <typename Fn>
void inRestoredContext(SnapshotTransferIsolate& isolate, Fn&& fn) {
  isolate.runInLockScope([&](SnapshotTransferIsolate::Lock& lock) {
    lock.withinHandleScope([&] {
      auto jsContext = lock.newContext<SnapshotTransferContext>();
      auto context = jsContext.getHandle(lock);
      JSG_WITHIN_CONTEXT_SCOPE(lock, context, [&](Lock& js) { fn(js); });
    });
  });
}

size_t countTransfers(const SnapshotArtifact& artifact) {
  size_t count = 0;
  for (auto& slot: artifact.liveWrappables) {
    if (slot.transfer != nullptr) ++count;
  }
  return count;
}

KJ_TEST("a stateful object's state and a stateless object survive a snapshot round trip") {
  StatefulThing::cloneCalls = 0;
  StatelessThing::constructed = 0;
  auto artifact = bakeSnapshot("globalThis.stateful = new StatefulThing(7);"
                               "globalThis.stateless = new StatelessThing();"_kj);
  // Once in the zygote, on the original.
  KJ_EXPECT(StatefulThing::cloneCalls == 1);
  KJ_EXPECT(StatelessThing::constructed == 1);
  KJ_EXPECT(countTransfers(*artifact) == 2, countTransfers(*artifact));

  auto restored = newIsolateFromSnapshot(*artifact);
  inRestoredContext(*restored, [&](Lock& js) {
    // Once more per restored worker, on the stored clone; the stateless type was constructed
    // afresh.
    KJ_EXPECT(StatefulThing::cloneCalls == 2);
    KJ_EXPECT(StatelessThing::constructed == 2);
    KJ_EXPECT(eval(js, "stateful.getValue()"_kj) == "7"_kj);
    KJ_EXPECT(eval(js, "stateful instanceof StatefulThing"_kj) == "true"_kj);
    KJ_EXPECT(eval(js, "stateful.setValue(8); stateful.getValue()"_kj) == "8"_kj);
    KJ_EXPECT(eval(js, "stateless.kind()"_kj) == "1"_kj);
    KJ_EXPECT(eval(js, "stateless instanceof StatelessThing"_kj) == "true"_kj);
    // The restored context can still build new instances of the baked types.
    KJ_EXPECT(
        eval(js, "new StatefulThing(3).getValue() + new StatelessThing().kind()"_kj) == "4"_kj);
  });
}

KJ_TEST("an object the context no longer reaches at CreateBlob is dropped from the artifact") {
  StatefulThing::cloneCalls = 0;
  // The held instance is reachable through the HolderThing's handle alone: live when the
  // artifact records the wrappables, garbage once the HolderThing is destroyed ahead of
  // CreateBlob(), so its clone is made and then dropped. (A plain temporary would already be
  // collected by the settling that precedes the record.)
  auto artifact = bakeSnapshot("globalThis.holder = new HolderThing();"
                               "holder.hold(new StatefulThing(5));"
                               "globalThis.kept = new StatefulThing(6);"_kj);
  // Three recorded; the holder and the kept instance transfer.
  KJ_EXPECT(artifact->liveWrappables.size() == 3, artifact->liveWrappables.size());
  KJ_EXPECT(countTransfers(*artifact) == 2, countTransfers(*artifact));

  auto restored = newIsolateFromSnapshot(*artifact);
  inRestoredContext(
      *restored, [&](Lock& js) { KJ_EXPECT(eval(js, "kept.getValue()"_kj) == "6"_kj); });
  // Both were cloned in the zygote; only the kept one at the restore.
  KJ_EXPECT(StatefulThing::cloneCalls == 3, StatefulThing::cloneCalls);
}

KJ_TEST("a type with no snapshot transfer fails the zygote when the context reaches it") {
  KJ_EXPECT_THROW_MESSAGE("neither set jsgSnapshotStateless nor implement snapshotClone()",
      bakeSnapshot("globalThis.opaque = new OpaqueThing()"_kj));
}

KJ_TEST(
    "a subclass that inherits snapshotClone() fails the zygote instead of transferring as its base") {
  KJ_EXPECT_THROW_MESSAGE("snapshotClone() returned an instance of another type",
      bakeSnapshot("globalThis.sub = new StatefulSubThing(4)"_kj));
}

KJ_TEST("a type with no snapshot transfer does not matter once unreachable") {
  auto artifact = bakeSnapshot("new OpaqueThing();"
                               "globalThis.kept = new StatefulThing(9);"_kj);
  auto restored = newIsolateFromSnapshot(*artifact);
  inRestoredContext(*restored, [&](Lock& js) {
    KJ_EXPECT(eval(js, "kept.getValue()"_kj) == "9"_kj);
    KJ_EXPECT(eval(js, "typeof opaque"_kj) == "undefined"_kj);
  });
}

// =======================================================================================
// FinalizationRegistry cleanups around the snapshot. A cleanup the top level leaves due runs in
// the zygote before anything is recorded, so what the callback does is part of the snapshot; a
// cleanup that becomes due only because the zygote's objects were destroyed is never run.

// A registry whose callback records the held value in a new StatefulThing on the global.
constexpr kj::StringPtr RECORDING_REGISTRY =
    "globalThis.registry = new FinalizationRegistry(held => {"
    "  globalThis.fromCleanup = new StatefulThing(held);"
    "});"_kj;

KJ_TEST("a cleanup due at the end of the top level runs before the live objects are recorded") {
  StatefulThing::cloneCalls = 0;
  // The registered object is garbage as soon as the function returns; the zygote has no gc().
  auto artifact =
      bakeSnapshot(kj::str(RECORDING_REGISTRY, "(() => { registry.register({}, 42); })();"));
  // The object the callback created was recorded like any other live object.
  KJ_EXPECT(StatefulThing::cloneCalls == 1);
  KJ_EXPECT(countTransfers(*artifact) == 1, countTransfers(*artifact));

  auto restored = newIsolateFromSnapshot(*artifact);
  inRestoredContext(*restored, [&](Lock& js) {
    KJ_EXPECT(eval(js, "fromCleanup.getValue()"_kj) == "42"_kj);
    KJ_EXPECT(eval(js, "fromCleanup instanceof StatefulThing"_kj) == "true"_kj);
  });
}

KJ_TEST("a target the top level kept through WeakRef.deref() is settled too") {
  // deref() keeps the target alive until the end of the job; the settling clears that before
  // each collection, so CreateBlob()'s own GC is not the first to find it.
  auto artifact = bakeSnapshot(kj::str(RECORDING_REGISTRY,
      "(() => { const o = {}; registry.register(o, 7); new WeakRef(o).deref(); })();"));
  auto restored = newIsolateFromSnapshot(*artifact);
  inRestoredContext(
      *restored, [&](Lock& js) { KJ_EXPECT(eval(js, "fromCleanup.getValue()"_kj) == "7"_kj); });
}

KJ_TEST("a cleanup that registers another doomed object is settled in a later round") {
  auto artifact = bakeSnapshot("let first = true;"
                               "globalThis.registry = new FinalizationRegistry(held => {"
                               "  globalThis.fromCleanup = new StatefulThing(held);"
                               "  if (first) { first = false; registry.register({}, held + 1); }"
                               "});"
                               "(() => { registry.register({}, 1); })();"_kj);
  auto restored = newIsolateFromSnapshot(*artifact);
  inRestoredContext(*restored, [&](Lock& js) {
    // Both callbacks ran: the second one's target was only collected by a later round.
    KJ_EXPECT(eval(js, "fromCleanup.getValue()"_kj) == "2"_kj);
  });
}

KJ_TEST("a cleanup due only once the zygote's objects are destroyed fails the zygote") {
  // The registered object is reachable through the HolderThing's handle alone. It survives the
  // settling and dies when the HolderThing is destroyed ahead of CreateBlob(); the callback must
  // not run then, and the snapshot is not taken.
  KJ_EXPECT_THROW_MESSAGE("became due only once the zygote's objects were destroyed",
      bakeSnapshot(
          "globalThis.registry = new FinalizationRegistry(() => { globalThis.ran = true; });"
          "globalThis.h = new HolderThing();"
          "(() => { const x = {}; h.hold(x); registry.register(x, 'held'); })();"_kj));
}

KJ_TEST("cleanups that keep dooming new objects fail the zygote") {
  KJ_EXPECT_THROW_MESSAGE("did not settle",
      bakeSnapshot("globalThis.registry = new FinalizationRegistry(held => {"
                   "  registry.register({}, held + 1);"
                   "});"
                   "(() => { registry.register({}, 1); })();"_kj));
}

KJ_TEST("a cleanup callback that throws does not fail the zygote") {
  // Reported the way an uncaught exception at request time is; the callback's effects up to the
  // throw are part of the snapshot.
  auto artifact = bakeSnapshot("globalThis.registry = new FinalizationRegistry(held => {"
                               "  globalThis.fromCleanup = new StatefulThing(held);"
                               "  throw new Error('cleanup failed');"
                               "});"
                               "(() => { registry.register({}, 3); })();"_kj);
  auto restored = newIsolateFromSnapshot(*artifact);
  inRestoredContext(
      *restored, [&](Lock& js) { KJ_EXPECT(eval(js, "fromCleanup.getValue()"_kj) == "3"_kj); });
}

KJ_TEST("dropUnreachableWrappables() raises what the serializer callback could only record") {
  auto artifact = kj::atomicRefcounted<SnapshotArtifact>();
  {
    WrappablesSerializeMetadata metadata;
    metadata.unknownWrappersCount = 1;
    KJ_EXPECT_THROW_MESSAGE("were not recorded", metadata.dropUnreachableWrappables(*artifact));
  }
  {
    WrappablesSerializeMetadata metadata;
    metadata.unclonable.add(kj::str("SomeType"));
    KJ_EXPECT_THROW_MESSAGE("SomeType", metadata.dropUnreachableWrappables(*artifact));
  }
}

KJ_TEST("releaseSlotsWithChecks() raises the deserializer's findings and an unlinked slot") {
  {
    WrappableDeserializeMetadata metadata;
    metadata.badPayloadsCount = 1;
    KJ_EXPECT_THROW_MESSAGE("do not match the artifact", metadata.releaseSlotsWithChecks());
  }
  {
    WrappableDeserializeMetadata metadata;
    metadata.duplicateLinksCount = 1;
    KJ_EXPECT_THROW_MESSAGE("more than one", metadata.releaseSlotsWithChecks());
  }
  // A slot whose shim was pre-allocated but never linked by the deserializer callback: the blob
  // did not reference it.
  auto isolate = kj::heap<SnapshotTransferIsolate>(
      v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  isolate->runInLockScope([&](SnapshotTransferIsolate::Lock& lock) {
    WrappableDeserializeMetadata metadata;
    metadata.isolate = lock.v8Isolate;
    metadata.slots = kj::heapArray<WrappableDeserializeMetadata::Slot>(1);
    metadata.slots[0].shim = HeapTracer::getTracer(lock.v8Isolate)
                                 .allocateShimForSnapshotRestore(
                                     StatefulThing::freshForRestore(1), Wrappable::WRAPPABLE_TAG);
    KJ_EXPECT_THROW_MESSAGE("never restored", metadata.releaseSlotsWithChecks());
    // The check released the root either way.
    KJ_EXPECT(metadata.slots == nullptr);
  });
}

}  // namespace
}  // namespace workerd::jsg::test
