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

}  // namespace
}  // namespace workerd::jsg::test
