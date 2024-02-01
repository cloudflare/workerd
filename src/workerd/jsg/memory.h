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
#include <v8.h>
#include <stack>
#include <string>

namespace workerd::jsg {

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

#define JSG_MEMORY_INFO_GET_DETACHED_STATE()                                             \
  MemoryInfoDetachedState jsgGetMemoryInfoDetachedState() const

#define JSG_MEMORY_INFO_GET_DETACHEDNESS()                                               \
  MemoryInfoDetachedState jsgGetMemoryInfoDetachedState()

#define JSG_MEMORY_INFO_IS_ROOTNODE()                                                    \
  bool jsgGetMemoryInfoIsRootNode() const

class MemoryTracker final {
public:
  inline MemoryTracker& trackFieldWithSize(
      kj::StringPtr edgeName, size_t size,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  inline MemoryTracker& trackInlineFieldWithSize(
      kj::StringPtr edgeName, size_t size,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::Own<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T, typename D>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const std::unique_ptr<T, D>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const std::shared_ptr<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <V8Value T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const V8Ref<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const Ref<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const T& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <typename T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::Maybe<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <typename T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::Maybe<T&>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::String& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <typename T,
            typename test = typename std::
                enable_if<std::numeric_limits<T>::is_specialized, bool>::type,
            typename dummy = bool>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::Array<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T, typename ... Indexes>
  MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::Table<T, Indexes...>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true) KJ_LIFETIMEBOUND;

  template <typename Key, typename Value>
  MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::HashMap<Key, Value>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  // template <typename Key, typename Value>
  // MemoryTracker& trackField(
  //     kj::StringPtr edgeName,
  //     const Key& key,
  //     const Value& value,
  //     kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T, typename Iterator = typename T::const_iterator>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const T& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::ArrayPtr<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const kj::ArrayPtr<T* const>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none,
      kj::Maybe<kj::StringPtr> elementName = kj::none,
      bool subtractFromSelf = true) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const T* value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <typename T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const std::basic_string<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <V8Value T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const v8::Eternal<T>& value,
      kj::StringPtr nodeName) KJ_LIFETIMEBOUND;

  template <V8Value T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const v8::PersistentBase<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <V8Value T>
  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const v8::Local<T>& value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  inline MemoryTracker& trackField(
      kj::StringPtr edgeName,
      const v8::BackingStore* value,
      kj::Maybe<kj::StringPtr> nodeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& track(
      const T* retainer,
      kj::Maybe<kj::StringPtr> edgeName = kj::none) KJ_LIFETIMEBOUND;

  template <MemoryRetainer T>
  inline MemoryTracker& trackInlineField(
      const T* retainer,
      kj::Maybe<kj::StringPtr> edgeName = kj::none) KJ_LIFETIMEBOUND;

  inline v8::EmbedderGraph* graph() { return graph_; }
  inline v8::Isolate* isolate() { return isolate_; }

  KJ_DISALLOW_COPY_AND_MOVE(MemoryTracker);

private:
  v8::Isolate* isolate_;
  v8::EmbedderGraph* graph_;
  std::stack<MemoryRetainerNode*> nodeStack_;
  kj::HashMap<const void*, MemoryRetainerNode*> seen_;
  KJ_DISALLOW_AS_COROUTINE_PARAM;

  inline explicit MemoryTracker(v8::Isolate* isolate,
                                v8::EmbedderGraph* graph)
    : isolate_(isolate),
      graph_(graph) {}

  inline kj::Maybe<MemoryRetainerNode&> getCurrentNode() const;

  template <MemoryRetainer T>
  inline MemoryRetainerNode* addNode(const T* retainer,
                                     kj::Maybe<kj::StringPtr> edgeName = kj::none);

  template <MemoryRetainer T>
  inline MemoryRetainerNode* pushNode(const T* retainer,
                                      kj::Maybe<kj::StringPtr> edgeName = kj::none);

  inline MemoryRetainerNode* addNode(kj::StringPtr node_name,
                                     size_t size,
                                     kj::Maybe<kj::StringPtr> edgeName = kj::none);
  inline MemoryRetainerNode* pushNode(kj::StringPtr node_name,
                                      size_t size,
                                      kj::Maybe<kj::StringPtr> edgeName = kj::none);
  inline void popNode();

  inline static kj::StringPtr getNodeName(kj::Maybe<kj::StringPtr> nodeName,
                                          kj::StringPtr edgeName) {
    KJ_IF_SOME(name, nodeName) { return name; }
    return edgeName;
  }

  friend class IsolateBase;
};

class MemoryRetainerNode final : public v8::EmbedderGraph::Node {
public:
  static constexpr auto PREFIX = "workerd /";

  const char* Name() override { return name_.cStr(); }

  const char* NamePrefix() override { return PREFIX; }

  size_t SizeInBytes() override {
    return size_;
  }

  bool IsRootNode() override {
    KJ_IF_SOME(check, checkIsRootNode) {
      return check();
    }
    return isRootNode_;
  }

  v8::EmbedderGraph::Node::Detachedness GetDetachedness() override {
    return detachedness_;
  }

  inline Node* JSWrapperNode() { return wrapper_node_; }

  KJ_DISALLOW_COPY_AND_MOVE(MemoryRetainerNode);
  ~MemoryRetainerNode() noexcept(true) {}

private:
  static inline v8::EmbedderGraph::Node::Detachedness fromDetachedState(
      MemoryInfoDetachedState state) {
    switch (state) {
      case MemoryInfoDetachedState::UNKNOWN:
        return v8::EmbedderGraph::Node::Detachedness::kUnknown;
      case MemoryInfoDetachedState::ATTACHED:
        return v8::EmbedderGraph::Node::Detachedness::kAttached;
      case MemoryInfoDetachedState::DETACHED:
        return v8::EmbedderGraph::Node::Detachedness::kDetached;
    }
    KJ_UNREACHABLE;
  }

  template <MemoryRetainer T>
  inline MemoryRetainerNode(MemoryTracker* tracker,
                            const T* retainer)
      : name_(retainer->jsgGetMemoryName()),
        size_(retainer->jsgGetMemorySelfSize()) {
    v8::HandleScope handle_scope(tracker->isolate());
    if constexpr (MemoryRetainerObject<T>) {
      v8::Local<v8::Object> obj =
          const_cast<T*>(retainer)->jsgGetMemoryInfoWrapperObject(tracker->isolate());
      if (!obj.IsEmpty()) wrapper_node_ = tracker->graph()->V8Node(obj);
    }
    if constexpr (MemoryRetainerIsRootNode<T>) {
      checkIsRootNode = [retainer]() { return retainer->jsgGetMemoryInfoIsRootNode(); };
    }

    if constexpr (MemoryRetainerDetachedState<T>) {
      detachedness_ = fromDetachedState(retainer->jsgGetMemoryInfoDetachedState());
    }
  }

  inline MemoryRetainerNode(MemoryTracker* tracker,
                            kj::StringPtr name,
                            size_t size,
                            bool isRootNode = false)
      : name_(name),
        size_(size),
        isRootNode_(isRootNode) {}

  kj::StringPtr name_;
  size_t size_ = 0;
  v8::EmbedderGraph::Node* wrapper_node_ = nullptr;

  kj::Maybe<kj::Function<bool()>> checkIsRootNode = kj::none;

  bool isRootNode_ = false;
  v8::EmbedderGraph::Node::Detachedness detachedness_ =
      v8::EmbedderGraph::Node::Detachedness::kUnknown;

  friend class MemoryTracker;
};

// ======================================================================================

kj::Maybe<MemoryRetainerNode&> MemoryTracker::getCurrentNode() const {
  if (nodeStack_.empty()) return kj::none;
  return *nodeStack_.top();
}

MemoryTracker& MemoryTracker::trackFieldWithSize(
    kj::StringPtr edgeName, size_t size,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (size > 0) addNode(getNodeName(nodeName, edgeName), size, edgeName);
  return *this;
}

MemoryTracker& MemoryTracker::trackInlineFieldWithSize(
    kj::StringPtr edgeName, size_t size,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (size > 0) addNode(getNodeName(nodeName, edgeName), size, edgeName);
  KJ_ASSERT_NONNULL(getCurrentNode()).size_ -= size;
  return *this;
}

// ======================================================================================

template <MemoryRetainer T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Own<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.get() == nullptr) return *this;
  return trackField(edgeName, value.get(), nodeName);
}

template <MemoryRetainer T, typename D>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const std::unique_ptr<T, D>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.get() == nullptr) return *this;
  return trackField(edgeName, value.get(), nodeName);
}

template <MemoryRetainer T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const std::shared_ptr<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.get() == nullptr) return *this;
  return trackField(edgeName, value.get(), nodeName);
}

template <typename T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Maybe<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  KJ_IF_SOME(v, value) {
    trackField(edgeName, v, nodeName);
  }
  return *this;
}

template <typename T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Maybe<T&>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  KJ_IF_SOME(v, value) {
    trackField(edgeName, v, nodeName);
  }
  return *this;
}

MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::String& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  return trackFieldWithSize(edgeName, value.size(), "kj::String"_kjc);
}

template <typename T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const std::basic_string<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  return trackFieldWithSize(edgeName, value.size() * sizeof(T), "std::basic_string"_kjc);
}

template <typename T, typename test, typename dummy>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Array<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  return trackFieldWithSize(edgeName, value.size() * sizeof(T), "kj::Array<T>"_kjc);
}

template <MemoryRetainer T, typename ... Indexes>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::Table<T, Indexes...>& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return *this;
  KJ_IF_SOME(currentNode, getCurrentNode()) {
    if (subtractFromSelf) {
      currentNode.size_ -= sizeof(T);
    }
  }
  pushNode(getNodeName(nodeName, edgeName), sizeof(T), edgeName);
  for (auto it = value.begin(); it != value.end(); ++it) {
    // Use nullptr as edge names so the elements appear as indexed properties
    trackField(nullptr, *it, elementName);
  }
  popNode();
  return *this;
}

template <typename Key, typename Value>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::HashMap<Key, Value>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.size() == 0) return *this;
  pushNode(getNodeName(nodeName, edgeName),
      sizeof(kj::HashMap<Key, Value>), edgeName);

  for (const auto& entry : value) {
    trackField("key", entry.key);
    trackField("value", entry.value);
  }
  popNode();
  return *this;
}

template <MemoryRetainer T, typename Iterator>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const T& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return *this;
  KJ_IF_SOME(currentNode, getCurrentNode()) {
    if (subtractFromSelf) {
      currentNode.size_ -= sizeof(T);
    }
  }
  pushNode(getNodeName(nodeName, edgeName), sizeof(T), edgeName);
  for (Iterator it = value.begin(); it != value.end(); ++it) {
    // Use nullptr as edge names so the elements appear as indexed properties
    trackField(nullptr, *it, elementName);
  }
  popNode();
  return *this;
}

template <MemoryRetainer T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::ArrayPtr<T>& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return *this;
  KJ_IF_SOME(currentNode, getCurrentNode()) {
    if (subtractFromSelf) {
      currentNode.size_ -= sizeof(T);
    }
  }
  pushNode(getNodeName(nodeName, edgeName), sizeof(T), edgeName);
  for (const auto item : value) {
    // Use nullptr as edge names so the elements appear as indexed properties
    trackField(nullptr, item, elementName);
  }
  popNode();
  return *this;
}

template <MemoryRetainer T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const kj::ArrayPtr<T* const>& value,
    kj::Maybe<kj::StringPtr> nodeName,
    kj::Maybe<kj::StringPtr> elementName,
    bool subtractFromSelf) {
  if (value.begin() == value.end()) return *this;
  KJ_IF_SOME(currentNode, getCurrentNode()) {
    if (subtractFromSelf) {
      currentNode.size_ -= sizeof(T);
    }
  }
  pushNode(getNodeName(nodeName, edgeName), sizeof(T), edgeName);
  for (const auto item : value) {
    // Use nullptr as edge names so the elements appear as indexed properties
    trackField(nullptr, item, elementName);
  }
  popNode();
  return *this;
}

template <MemoryRetainer T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const T& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  return trackField(edgeName, &value, nodeName);
}

template <MemoryRetainer T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const T* value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value == nullptr) return *this;
  KJ_IF_SOME(found, seen_.find(value)) {
    KJ_IF_SOME(currentNode, getCurrentNode()) {
      graph_->AddEdge(&currentNode, found, edgeName.cStr());
    } else {
      graph_->AddEdge(nullptr, found, edgeName.cStr());
    }
    return *this;
  }

  return track(value, edgeName);
}

template <V8Value T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::Eternal<T>& value,
    kj::StringPtr nodeName) {
  return trackField(edgeName, value.Get(isolate_));
}

template <V8Value T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::PersistentBase<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (value.IsWeak()) return *this;
  return trackField(edgeName, value.Get(isolate_));
}

template <V8Value T>
MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::Local<T>& value,
    kj::Maybe<kj::StringPtr> nodeName) {
  if (!value.IsEmpty()) {
    KJ_IF_SOME(currentNode, getCurrentNode()) {
      graph_->AddEdge(&currentNode, graph_->V8Node(value), edgeName.cStr());
    } else {
      graph_->AddEdge(nullptr, graph_->V8Node(value), edgeName.cStr());
    }
  }
  return *this;
}

MemoryTracker& MemoryTracker::trackField(
    kj::StringPtr edgeName,
    const v8::BackingStore* value,
    kj::Maybe<kj::StringPtr> nodeName) {
  return trackFieldWithSize(edgeName, value->ByteLength(), "BackingStore"_kjc);
}

// Put a memory container into the graph, create an edge from
// the current node if there is one on the stack.
template <MemoryRetainer T>
MemoryTracker& MemoryTracker::track(const T* retainer, kj::Maybe<kj::StringPtr> edgeName) {
  v8::HandleScope handle_scope(isolate_);
  KJ_IF_SOME(found, seen_.find(retainer)) {
    KJ_IF_SOME(currentNode, getCurrentNode()) {
      KJ_IF_SOME(name, edgeName) {
        graph_->AddEdge(&currentNode, found, name.cStr());
      } else {
        graph_->AddEdge(&currentNode, found, nullptr);
      }
    }
    return *this;
  }

  MemoryRetainerNode* n = pushNode(retainer, edgeName);
  retainer->jsgGetMemoryInfo(*this);
  KJ_ASSERT(&KJ_ASSERT_NONNULL(getCurrentNode()) == n);
  KJ_ASSERT(n->size_ != 0);
  popNode();
  return *this;
}

// Useful for parents that do not wish to perform manual
// adjustments to its `SelfSize()` when embedding retainer
// objects inline.
// Put a memory container into the graph, create an edge from
// the current node if there is one on the stack - there should
// be one, of the container object which the current field is part of.
// Reduce the size of memory from the container so as to avoid
// duplication in accounting.
template <MemoryRetainer T>
MemoryTracker& MemoryTracker::trackInlineField(
    const T* retainer,
    kj::Maybe<kj::StringPtr> edgeName) {
  track(retainer, edgeName);
  KJ_ASSERT_NONNULL(getCurrentNode()).size_ -= retainer->getMemorySelfSize();
  return *this;
}

template <MemoryRetainer T>
MemoryRetainerNode* MemoryTracker::addNode(const T* retainer,
                                           kj::Maybe<kj::StringPtr> edgeName) {
  KJ_IF_SOME(found, seen_.find(retainer)) { return found; }

  MemoryRetainerNode* n = new MemoryRetainerNode(this, retainer);
  graph_->AddNode(std::unique_ptr<v8::EmbedderGraph::Node>(n));
  seen_.insert(retainer, n);

  KJ_IF_SOME(currentNode, getCurrentNode()) {
    KJ_IF_SOME(name, edgeName) {
      graph_->AddEdge(&currentNode, n, name.cStr());
    } else {
      graph_->AddEdge(&currentNode, n, nullptr);
    }
  }

  if (n->JSWrapperNode() != nullptr) {
    graph_->AddEdge(n, n->JSWrapperNode(), "native_to_javascript");
    graph_->AddEdge(n->JSWrapperNode(), n, "javascript_to_native");
  }

  return n;
}

MemoryRetainerNode* MemoryTracker::addNode(kj::StringPtr nodeName,
                                           size_t size,
                                           kj::Maybe<kj::StringPtr> edgeName) {
  MemoryRetainerNode* n = new MemoryRetainerNode(this, nodeName, size);
  graph_->AddNode(std::unique_ptr<v8::EmbedderGraph::Node>(n));

  KJ_IF_SOME(currentNode, getCurrentNode()) {
    KJ_IF_SOME(name, edgeName) {
      graph_->AddEdge(&currentNode, n, name.cStr());
    } else {
      graph_->AddEdge(&currentNode, n, nullptr);
    }
  }

  return n;
}

template <MemoryRetainer T>
MemoryRetainerNode* MemoryTracker::pushNode(const T* retainer,
                                            kj::Maybe<kj::StringPtr> edgeName) {
  MemoryRetainerNode* n = addNode(retainer, edgeName);
  nodeStack_.push(n);
  return n;
}

MemoryRetainerNode* MemoryTracker::pushNode(kj::StringPtr nodeName,
                                            size_t size,
                                            kj::Maybe<kj::StringPtr> edgeName) {
  MemoryRetainerNode* n = addNode(nodeName, size, edgeName);
  nodeStack_.push(n);
  return n;
}

void MemoryTracker::popNode() {
  nodeStack_.pop();
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

  inline HeapSnapshotActivity(Callback callback): callback(kj::mv(callback)) {}
  ~HeapSnapshotActivity() noexcept(true) = default;

  inline ControlOption ReportProgressValue(uint32_t done, uint32_t total) override {
    return callback(done, total) ?
        ControlOption::kContinue :
        ControlOption::kAbort;
  }

private:
  Callback callback;
};

class HeapSnapshotWriter final: public v8::OutputStream {
public:
  using Callback = kj::Function<bool(kj::Maybe<kj::ArrayPtr<char>>)>;

  inline HeapSnapshotWriter(Callback callback, size_t chunkSize = 65536)
      : callback(kj::mv(callback)),
        chunkSize(chunkSize) {}
  inline ~HeapSnapshotWriter() noexcept(true) {}

  inline void EndOfStream() override {
    callback(kj::none);
  }

  inline int GetChunkSize() override { return chunkSize; }

  inline v8::OutputStream::WriteResult WriteAsciiChunk(char* data, int size) override {
    return callback(kj::ArrayPtr<char>(data, size)) ?
        v8::OutputStream::WriteResult::kContinue :
        v8::OutputStream::WriteResult::kAbort;
  }

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
