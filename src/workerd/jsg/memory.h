// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Implements mechanism for incorporating details about the native (c++) objects
// in a v8 heap snapshot. The design of the API and implementation were heavily
// influenced by Node.js' implementation of the same feature.

#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/hash.h>
#include <kj/map.h>
#include <kj/table.h>
#include <kj/string.h>
#include <v8-profiler.h>
#include <v8-array-buffer.h>
#include <stack>
#include <string>

namespace workerd::jsg {

// The MemoryTracker is used to integrate with v8's BuildEmbedderGraph API.
// It constructs the graph of embedder objects to be included in a generated
// heap snapshot.
//
// The API is implemented using a visitor pattern. V8 calls the BuilderEmbedderGraph
// callback (in setup.h) which in turn begins walking through the known embedder
// objects collecting the necessary information.
//
// To instrument a struct or class so that it can be included in the graph, the
// type must implement *at least* the following three methods:
//
//   kj::StringPtr jsgGetMemoryName() const;
//   size_t jsgGetMemorySelfSize() const;
//   void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
//
// The jsgGetMemoryName() method returns the name that should be used to identify
// the type in the graph. This will be prefixed with "workerd / " in the actual
// generated snapshot. For instance, if this method returns "Foo"_kjc, the heap
// snapshot will contain "workerd / Foo".
//
// The jsgGetMemorySelfSize() method returns the *shallow* size of the type.
// This would typically be implemented using sizeof(Type), and in the vast
// majority of cases that's all it does. It is provided as a method, however,
// in order to allow a type the ability to customize the size calculation.
//
// The jsgGetMemoryInfo(...) method is the method that is actually called to
// visit instances of the type to collect details for the graph. Note that this
// method is NOT expected to be called within the scope of an IoContext. It will
// be called while within the isolate lock, however.
//
// Types may also implement the following additional methods to further customize
// how they are represented in the graph:
//
//   v8::Local<v8::Object> jsgGetMemoryInfoWrapperObject();
//   MemoryInfoDetachedState jsgGetMemoryInfoDetachedState() const;
//   bool jsgGetMemoryInfoIsRootNode() const;
//
// Note that the `jsgGetMemoryInfoWrapperObject() method is called from within
// a v8::HandleScope.
//
// For extremely simple cases, the JSG_MEMORY_INFO macro can be used to simplify
// implementing these methods. It is a shortcut that provides basic implementations
// of the jsgGetMemoryName() and jsgGetMemorySelfSize() methods:
//
//    JSG_MEMORY_INFO(Foo) {
//      tracker.trackField("bar", bar);
//    }
//
// ... is equivalent to:
//
//    kj::StringPtr jsgGetMemoryName() const { return "Foo"_kjc; }
//    size_t jsgGetMemorySelfSize() const { return sizeof(Foo); }
//    void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
//      tracker.trackField("bar", bar);
//    }
//
// All jsg::Object instances provide a basic implementation of these methods.
// Within a jsg::Object, your only responsibility would be to implement the
// helper visitForMemoryInfo(jsg::MemoryTracker& tracker) const method only
// if the type has additional fields that need to be tracked. This works a
// lot like the visitForGc(...) method used for gc tracing:
//
//   class Foo : public jsg::Object {
//   public:
//     JSG_RESOURCE_TYPE(Foo) {}
//
//     void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
//       tracker.trackField("bar", bar);
//     }
//     // ...
//   };
//
// The constructed graph should include any fields that materially contribute
// the retained memory of the type. This graph is primarily used for analysis
// and investigation of memory issues in an application (e.g. hunting down
// memory leaks, detecting bugs, optimizing memory usage, etc) so the information
// should include details that are most useful for those purposes.
//
// This code is only ever called when a heap snapshot is being generated so
// typically it should have very little cost. Heap snapshots are generally
// fairly expensive to create, however, so care should be taken not to make
// things too complicated. Ideally, none of the implementation methods in a
// type should allocate. There is some allocation occurring internally while
// building the graph, of course, but the methods for visitation (in particular
// the jsgGetMemoryInfo(...) method) should not perform any allocations if it
// can be avoided.

class MemoryTracker;
class MemoryRetainerNode;

template <typename T> class V8Ref;
template <typename T> class Ref;

enum class MemoryInfoDetachedState {
  UNKNOWN,
  ATTACHED,
  DETACHED,
};

template <typename T>
concept MemoryRetainer = requires(const T* a) {
  std::is_member_function_pointer_v<decltype(&T::jsgGetMemoryInfo)>;
  std::is_member_function_pointer_v<decltype(&T::jsgGetMemoryName)>;
  std::is_member_function_pointer_v<decltype(&T::jsgGetMemorySelfSize)>;
};

template <typename T>
concept MemoryRetainerObject = requires(T a) {
  MemoryRetainer<T>;
  std::is_member_function_pointer_v<decltype(&T::jsgGetMemoryInfoWrapperObject)>;
};

template <typename T>
concept MemoryRetainerDetachedState = requires(T a) {
  MemoryRetainer<T>;
  std::is_member_function_pointer_v<decltype(&T::jsgGetMemoryInfoDetachedState)>;
};

template <typename T>
concept MemoryRetainerIsRootNode = requires(T a) {
  MemoryRetainer<T>;
  std::is_member_function_pointer_v<decltype(&T::jsgGetMemoryInfoIsRootNode)>;
};

template <typename T>
concept V8Value = requires(T a) {
  std::is_assignable_v<v8::Value, T>;
};

#define JSG_MEMORY_INFO(Name)                                                            \
  kj::StringPtr jsgGetMemoryName() const {                                               \
    return #Name ## _kjc;                                                                \
  }                                                                                      \
  size_t jsgGetMemorySelfSize() const { return sizeof(Name); }                           \
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const

// jsg::MemoryTracker is used to construct the embedder graph for v8 heap
// snapshot construction.
class MemoryTracker final {
public:
  inline void trackFieldWithSize(
      kj::StringPtr edgeName, size_t size,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  inline void trackInlineFieldWithSize(
      kj::StringPtr edgeName, size_t size,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T>
  inline void trackField(
      kj::StringPtr edgeName,
      const kj::Own<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T, typename D>
  inline void trackField(
      kj::StringPtr edgeName,
      const std::unique_ptr<T, D>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T>
  inline void trackField(
      kj::StringPtr edgeName,
      const std::shared_ptr<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <V8Value T>
  inline void trackField(
      kj::StringPtr edgeName,
      const V8Ref<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T>
  inline void trackField(
      kj::StringPtr edgeName,
      const Ref<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T>
  inline void trackField(
      kj::StringPtr edgeName,
      const T& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <typename T>
  inline void trackField(
      kj::StringPtr edgeName,
      const kj::Maybe<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <typename T>
  inline void trackField(
      kj::StringPtr edgeName,
      const kj::Maybe<T&>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  inline void trackField(
      kj::StringPtr edgeName,
      const kj::String& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  inline void trackField(
      kj::StringPtr edgeName,
      const kj::Exception& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <typename T,
            typename test = typename std::
                enable_if<std::numeric_limits<T>::is_specialized, bool>::type,
            typename dummy = bool>
  inline void trackField(
      kj::StringPtr edgeName,
      const kj::Array<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T, typename ... Indexes>
  void trackField(
      kj::StringPtr edgeName,
      const kj::Table<T, Indexes...>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true);

  template <typename Key, typename Value>
  void trackField(
      kj::StringPtr edgeName,
      const kj::HashMap<Key, Value>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T, typename Iterator = typename T::const_iterator>
  inline void trackField(
      kj::StringPtr edgeName,
      const T& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true);

  template <MemoryRetainer T>
  inline void trackField(
      kj::StringPtr edgeName,
      const kj::ArrayPtr<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true);

  template <MemoryRetainer T>
  inline void trackField(
      kj::StringPtr edgeName,
      const kj::ArrayPtr<T* const>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true);

  template <MemoryRetainer T>
  inline void trackField(
      kj::StringPtr edgeName,
      const T* value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <typename T>
  inline void trackField(
      kj::StringPtr edgeName,
      const std::basic_string<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <V8Value T>
  inline void trackField(
      kj::StringPtr edgeName,
      const v8::Eternal<T>& value,
      kj::StringPtr nodeName);

  template <V8Value T>
  inline void trackField(
      kj::StringPtr edgeName,
      const v8::PersistentBase<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <V8Value T>
  inline void trackField(
      kj::StringPtr edgeName,
      const v8::Local<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  inline void trackField(
      kj::StringPtr edgeName,
      const v8::BackingStore* value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none);

  template <MemoryRetainer T>
  inline void track(
      const T* retainer,
      kj::Maybe<kj::StringPtr> edgeName = kj::none);

  template <MemoryRetainer T>
  inline void trackInlineField(
      const T* retainer,
      kj::Maybe<kj::StringPtr> edgeName = kj::none);

  inline v8::Isolate* isolate() { return isolate_; }

  KJ_DISALLOW_COPY_AND_MOVE(MemoryTracker);

private:
  v8::Isolate* isolate_;
  v8::EmbedderGraph* graph_;
  std::stack<MemoryRetainerNode*> nodeStack_;
  kj::HashMap<const void*, MemoryRetainerNode*> seen_;
  KJ_DISALLOW_AS_COROUTINE_PARAM;

  explicit MemoryTracker(v8::Isolate* isolate, v8::EmbedderGraph* graph);

  KJ_NOINLINE MemoryRetainerNode* addNode(
      const void* retainer,
      const kj::StringPtr name,
      const size_t size,
      v8::Local<v8::Object> obj,
      kj::Maybe<kj::Function<bool()>> checkIsRootNode,
      MemoryInfoDetachedState detachedness,
      kj::Maybe<kj::StringPtr> edgeName);

  template <MemoryRetainer T>
  inline MemoryRetainerNode* pushNode(
      const T* retainer,
      kj::Maybe<kj::StringPtr> edgeName = kj::none);

  KJ_NOINLINE MemoryRetainerNode* addNode(
      kj::StringPtr node_name,
      size_t size,
      kj::Maybe<kj::StringPtr> edgeName = kj::none);

  KJ_NOINLINE MemoryRetainerNode* pushNode(
      kj::StringPtr node_name,
      size_t size,
      kj::Maybe<kj::StringPtr> edgeName = kj::none);

  KJ_NOINLINE void addEdge(MemoryRetainerNode* node, kj::StringPtr edgeName);
  KJ_NOINLINE void addEdge(v8::EmbedderGraph::Node* node, kj::StringPtr edgeName);
  void decCurrentNodeSize(size_t size);

  friend class IsolateBase;
  friend class MemoryRetainerNode;
};

// ======================================================================================

void MemoryTracker::trackFieldWithSize(
    kj::StringPtr edgeName, size_t size,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (size > 0) addNode(nodeName.orDefault(edgeName), size, edgeName);
}

void MemoryTracker::trackInlineFieldWithSize(
    kj::StringPtr edgeName, size_t size,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (size > 0) addNode(nodeName.orDefault(edgeName), size, edgeName);
}

void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::BackingStore* value,
    kj::Maybe<kj::StringPtr> nodeName) {
  trackFieldWithSize(edgeName, value->ByteLength(), "BackingStore"_kjc);
}

void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::String& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  trackFieldWithSize(edgeName, value.size(), "kj::String"_kjc);
}

void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Exception& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  // Note that the size of the kj::Exception here only includes the
  // shallow size of the kj::Exception type itself plus the length
  // of the description string. We ignore the size of the stack and
  // the context (if any). We could provide more detail but it's
  // likely unnecessary.
  trackFieldWithSize(edgeName,
      sizeof(kj::Exception) + value.getDescription().size(),
      "kj::Exception"_kjc);
}

template <MemoryRetainer T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Own<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.get() != nullptr) {
    trackField(edgeName, value.get(), nodeName);
  }
}

template <MemoryRetainer T, typename D>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const std::unique_ptr<T, D>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.get() != nullptr) {
    return trackField(edgeName, value.get(), nodeName);
  }
}

template <MemoryRetainer T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const std::shared_ptr<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.get() != nullptr) {
    return trackField(edgeName, value.get(), nodeName);
  }
}

template <typename T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Maybe<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  KJ_IF_SOME(v, value) {
    trackField(edgeName, v, nodeName);
  }
}

template <typename T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Maybe<T&>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  KJ_IF_SOME(v, value) {
    trackField(edgeName, v, nodeName);
  }
}

template <typename T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const std::basic_string<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  trackFieldWithSize(edgeName, value.size() * sizeof(T), "std::basic_string"_kjc);
}

template <typename T, typename test, typename dummy>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Array<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  trackFieldWithSize(edgeName, value.size() * sizeof(T), "kj::Array<T>"_kjc);
}

template <MemoryRetainer T, typename ... Indexes>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Table<T, Indexes...>& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return;
  if (subtractFromSelf) {
    decCurrentNodeSize(sizeof(T));
  }
  pushNode(nodeName.orDefault(edgeName), sizeof(T), edgeName);
  for (auto it = value.begin(); it != value.end(); ++it) {
    trackField(nullptr, *it, elementName);
  }
  nodeStack_.pop();
}

template <typename Key, typename Value>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::HashMap<Key, Value>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.size() == 0) return;
  pushNode(nodeName.orDefault(edgeName), sizeof(kj::HashMap<Key, Value>), edgeName);

  for (const auto& entry : value) {
    trackField("key", entry.key);
    trackField("value", entry.value);
  }
  nodeStack_.pop();
}

template <MemoryRetainer T, typename Iterator>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const T& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return;
  if (subtractFromSelf) {
    decCurrentNodeSize(sizeof(T));
  }
  pushNode(nodeName.orDefault(edgeName), sizeof(T), edgeName);
  for (Iterator it = value.begin(); it != value.end(); ++it) {
    trackField(nullptr, *it, elementName);
  }
  nodeStack_.pop();
}

template <MemoryRetainer T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::ArrayPtr<T>& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return;
  if (subtractFromSelf) {
    decCurrentNodeSize(sizeof(T));
  }
  pushNode(nodeName.orDefault(edgeName), sizeof(T), edgeName);
  for (const auto& item : value) {
    trackField(nullptr, item, elementName);
  }
  nodeStack_.pop();
}

template <MemoryRetainer T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::ArrayPtr<T* const>& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return;
  if (subtractFromSelf) {
    decCurrentNodeSize(sizeof(T));
  }
  pushNode(nodeName.orDefault(edgeName), sizeof(T), edgeName);
  for (const auto& item : value) {
    trackField(nullptr, item, elementName);
  }
  nodeStack_.pop();
}

template <MemoryRetainer T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const T& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  trackField(edgeName, &value, nodeName);
}

template <MemoryRetainer T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const T* value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value == nullptr) return;
  KJ_IF_SOME(found, seen_.find(value)) {
    addEdge(found, edgeName);
    return;
  }
  track(value, edgeName);
}

template <V8Value T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::Eternal<T>& value,
    kj::StringPtr nodeName) {
  trackField(edgeName, value.Get(isolate_));
}

template <V8Value T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::PersistentBase<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (!value.IsEmpty() && !value.IsWeak()) {
    trackField(edgeName, value.Get(isolate_));
  }
}

template <V8Value T>
void MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::Local<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (!value.IsEmpty()) {
    addEdge(graph_->V8Node(value.template As<v8::Value>()), edgeName);
  }
}

template <MemoryRetainer T>
void MemoryTracker::track(const T* retainer, kj::Maybe<kj::StringPtr> edgeName) {
  v8::HandleScope handle_scope(isolate_);
  KJ_IF_SOME(found, seen_.find(retainer)) {
    addEdge(found, edgeName.orDefault(nullptr));
    return;
  }

  pushNode(retainer, edgeName);
  retainer->jsgGetMemoryInfo(*this);
  nodeStack_.pop();
}

template <MemoryRetainer T>
void MemoryTracker::trackInlineField(
    const T* retainer,
    kj::Maybe<kj::StringPtr> edgeName) {
  track(retainer, edgeName);
}

template <MemoryRetainer T>
MemoryRetainerNode* MemoryTracker::pushNode(const T* retainer,
                                            kj::Maybe<kj::StringPtr> edgeName) {
  const kj::StringPtr name = retainer->jsgGetMemoryName();
  const size_t size = retainer->jsgGetMemorySelfSize();
  v8::Local<v8::Object> obj;
  kj::Maybe<kj::Function<bool()>> checkIsRootNode = kj::none;
  MemoryInfoDetachedState detachedness = MemoryInfoDetachedState::UNKNOWN;
  v8::HandleScope handleScope(isolate());
  if constexpr (MemoryRetainerObject<T>) {
    obj = const_cast<T*>(retainer)->jsgGetMemoryInfoWrapperObject(isolate());
  }
  if constexpr (MemoryRetainerIsRootNode<T>) {
    checkIsRootNode = [retainer]() { return retainer->jsgGetMemoryInfoIsRootNode(); };
  }
  if constexpr (MemoryRetainerDetachedState<T>) {
    detachedness = retainer->jsgGetMemoryInfoDetachedState();
  }

  MemoryRetainerNode* n = addNode(retainer, name, size, obj,
                                  kj::mv(checkIsRootNode),
                                  detachedness, edgeName);
  nodeStack_.push(n);
  return n;
}

template <typename T>
inline void visitSubclassForMemoryInfo(const T* obj, MemoryTracker& tracker) {
  if constexpr (&T::visitForMemoryInfo != &T::jsgSuper::visitForMemoryInfo) {
    obj->visitForMemoryInfo(tracker);
  }
}

// ======================================================================================

class HeapSnapshotActivity final: public v8::ActivityControl {
public:
  using Callback = kj::Function<bool(uint32_t done, uint32_t total)>;

  HeapSnapshotActivity(Callback callback);
  ~HeapSnapshotActivity() noexcept(true) = default;

  ControlOption ReportProgressValue(uint32_t done, uint32_t total) override;

private:
  Callback callback;
};

class HeapSnapshotWriter final: public v8::OutputStream {
public:
  using Callback = kj::Function<bool(kj::Maybe<kj::ArrayPtr<char>>)>;

  HeapSnapshotWriter(Callback callback, size_t chunkSize = 65536);
  ~HeapSnapshotWriter() noexcept(true) = default;

  void EndOfStream() override;

  int GetChunkSize() override;

  v8::OutputStream::WriteResult WriteAsciiChunk(char* data, int size) override;

private:
  Callback callback;
  size_t chunkSize;
};

struct HeapSnapshotDeleter: public kj::Disposer {
  inline void disposeImpl(void* ptr) const override {
    auto snapshot = const_cast<v8::HeapSnapshot*>(static_cast<const v8::HeapSnapshot*>(ptr));
    snapshot->Delete();
  }
};

}  // namespace workerd::jsg
