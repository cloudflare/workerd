#include "memory.h"
#include <kj/one-of.h>

namespace workerd::jsg {

class MemoryRetainerNode final: public v8::EmbedderGraph::Node {
public:
  static constexpr auto PREFIX = "workerd /";

  const char* Name() override {
    return name_.cStr();
  }

  const char* NamePrefix() override {
    return PREFIX;
  }

  size_t SizeInBytes() override {
    return size_;
  }

  bool IsRootNode() override {
    KJ_SWITCH_ONEOF(isRootNode) {
      KJ_CASE_ONEOF(b, bool) {
        return b;
      }
      KJ_CASE_ONEOF(fn, kj::Function<bool()>) {
        return fn();
      }
    }
    KJ_UNREACHABLE;
  }

  v8::EmbedderGraph::Node::Detachedness GetDetachedness() override {
    return detachedness_;
  }

  inline Node* JSWrapperNode() {
    return wrapper_node_;
  }

  KJ_DISALLOW_COPY_AND_MOVE(MemoryRetainerNode);
  ~MemoryRetainerNode() noexcept(true) {}

private:
  static v8::EmbedderGraph::Node::Detachedness fromDetachedState(MemoryInfoDetachedState state) {
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

  static v8::EmbedderGraph::Node* maybeWrapperNode(
      MemoryTracker* tracker, v8::Local<v8::Object> obj) {
    if (!obj.IsEmpty()) return tracker->graph_->V8Node(obj.As<v8::Value>());
    return nullptr;
  }

  MemoryRetainerNode(MemoryTracker* tracker,
      const void* retainer,
      const kj::StringPtr name,
      const size_t size,
      v8::Local<v8::Object> obj,
      kj::Maybe<kj::Function<bool()>> checkIsRootNode,
      MemoryInfoDetachedState detachedness)
      : name_(name),
        size_(size),
        wrapper_node_(maybeWrapperNode(tracker, obj)),
        detachedness_(fromDetachedState(detachedness)) {
    KJ_IF_SOME(fn, checkIsRootNode) {
      isRootNode = kj::mv(fn);
    }
  }

  MemoryRetainerNode(
      MemoryTracker* tracker, kj::StringPtr name, size_t size, bool isRootNode = false)
      : name_(name),
        size_(size),
        isRootNode(isRootNode) {}

  kj::StringPtr name_;
  size_t size_ = 0;
  v8::EmbedderGraph::Node* wrapper_node_ = nullptr;

  kj::OneOf<bool, kj::Function<bool()>> isRootNode = false;

  v8::EmbedderGraph::Node::Detachedness detachedness_ =
      v8::EmbedderGraph::Node::Detachedness::kUnknown;

  friend class MemoryTracker;
};

namespace {
kj::Maybe<MemoryRetainerNode&> getCurrentNode(const std::stack<MemoryRetainerNode*> stack) {
  if (stack.empty()) return kj::none;
  return *stack.top();
}
}  // namespace

MemoryTracker::MemoryTracker(v8::Isolate* isolate, v8::EmbedderGraph* graph)
    : isolate_(isolate),
      graph_(graph) {}

MemoryRetainerNode* MemoryTracker::addNode(const void* retainer,
    const kj::StringPtr name,
    const size_t size,
    v8::Local<v8::Object> obj,
    kj::Maybe<kj::Function<bool()>> checkIsRootNode,
    MemoryInfoDetachedState detachedness,
    kj::Maybe<kj::StringPtr> edgeName) {
  KJ_IF_SOME(found, seen_.find(retainer)) {
    return found;
  }

  MemoryRetainerNode* n = new MemoryRetainerNode(
      this, retainer, name, size, obj, kj::mv(checkIsRootNode), detachedness);
  graph_->AddNode(std::unique_ptr<v8::EmbedderGraph::Node>(n));
  seen_.insert(retainer, n);

  KJ_IF_SOME(currentNode, getCurrentNode(nodeStack_)) {
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

MemoryRetainerNode* MemoryTracker::addNode(
    kj::StringPtr nodeName, size_t size, kj::Maybe<kj::StringPtr> edgeName) {
  MemoryRetainerNode* n = new MemoryRetainerNode(this, nodeName, size);
  graph_->AddNode(std::unique_ptr<v8::EmbedderGraph::Node>(n));

  KJ_IF_SOME(currentNode, getCurrentNode(nodeStack_)) {
    KJ_IF_SOME(name, edgeName) {
      graph_->AddEdge(&currentNode, n, name.cStr());
    } else {
      graph_->AddEdge(&currentNode, n, nullptr);
    }
  }

  return n;
}

MemoryRetainerNode* MemoryTracker::pushNode(
    kj::StringPtr nodeName, size_t size, kj::Maybe<kj::StringPtr> edgeName) {
  MemoryRetainerNode* n = addNode(nodeName, size, edgeName);
  nodeStack_.push(n);
  return n;
}

void MemoryTracker::decCurrentNodeSize(size_t size) {
  KJ_IF_SOME(currentNode, getCurrentNode(nodeStack_)) {
    currentNode.size_ -= size;
  }
}

void MemoryTracker::addEdge(v8::EmbedderGraph::Node* node, kj::StringPtr edgeName) {
  KJ_IF_SOME(currentNode, getCurrentNode(nodeStack_)) {
    graph_->AddEdge(&currentNode, node, edgeName.cStr());
  } else {
    graph_->AddEdge(nullptr, node, edgeName.cStr());
  }
}

void MemoryTracker::addEdge(MemoryRetainerNode* node, kj::StringPtr edgeName) {
  KJ_IF_SOME(currentNode, getCurrentNode(nodeStack_)) {
    graph_->AddEdge(&currentNode, node, edgeName.cStr());
  } else {
    graph_->AddEdge(nullptr, node, edgeName.cStr());
  }
}

// ======================================================================================

HeapSnapshotActivity::HeapSnapshotActivity(Callback callback): callback(kj::mv(callback)) {}

v8::ActivityControl::ControlOption HeapSnapshotActivity::ReportProgressValue(
    uint32_t done, uint32_t total) {
  return callback(done, total) ? ControlOption::kContinue : ControlOption::kAbort;
}

HeapSnapshotWriter::HeapSnapshotWriter(Callback callback, size_t chunkSize)
    : callback(kj::mv(callback)),
      chunkSize(chunkSize) {}

void HeapSnapshotWriter::EndOfStream() {
  callback(kj::none);
}

int HeapSnapshotWriter::GetChunkSize() {
  return chunkSize;
}

v8::OutputStream::WriteResult HeapSnapshotWriter::WriteAsciiChunk(char* data, int size) {
  return callback(kj::ArrayPtr<char>(data, size)) ? v8::OutputStream::WriteResult::kContinue
                                                  : v8::OutputStream::WriteResult::kAbort;
}

}  // namespace workerd::jsg
