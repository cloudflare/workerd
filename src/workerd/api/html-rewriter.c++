// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "html-rewriter.h"

#include "streams.h"
#include "util.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

#include <lol_html.h>

struct lol_html_HtmlRewriter {};
struct lol_html_HtmlRewriterBuilder {};
struct lol_html_AttributesIterator {};
struct lol_html_Selector {};
// TODO(cleanup): These are defined internally in lol-html, but kj::Own<T> needs to check whether
//   or not T is polymorphic, so here's a dummy definition.

namespace workerd::api {

namespace {

// =======================================================================================
// RAII helpers for lol-html

// RAII helper for lol-html types which are managed by pointers and have straightforward _free()
// functions.
template <typename T, void (*lolhtmlFree)(T*)>
class LolHtmlDisposer: public kj::Disposer {
 public:
  static const LolHtmlDisposer INSTANCE;

 protected:
  void disposeImpl(void* pointer) const override {
    lolhtmlFree(reinterpret_cast<T*>(pointer));
  }
};

template <typename T, void (*lolhtmlFree)(T*)>
const LolHtmlDisposer<T, lolhtmlFree> LolHtmlDisposer<T, lolhtmlFree>::INSTANCE;

#define LOL_HTML_OWN(name, ...)                                                                    \
  ({                                                                                               \
    using T = lol_html_##name##_t;                                                                 \
    constexpr auto* lolhtmlFree = lol_html_##name##_free;                                          \
    kj::Own<T>(&check(__VA_ARGS__), LolHtmlDisposer<T, lolhtmlFree>::INSTANCE);                    \
  })

// RAII helper for lol_html_str_t.
//
// We cannot use a kj::Own<T> because lol_html_str_t is a struct, not a pointer, so instead we
// have this LolString RAII wrapper.
//
// Use `kj::str(LolString.asChars())` to allocate your own copy of a LolString.
class LolString {
 public:
  explicit LolString(lol_html_str_t s): chars(s.data, s.len) {}
  ~LolString() noexcept(false) {
    lol_html_str_free({chars.begin(), chars.size()});
  }
  KJ_DISALLOW_COPY(LolString);
  LolString(LolString&& other): chars(other.chars) {
    other.chars = nullptr;
  }
  LolString& operator=(LolString&& other) {
    LolString old(kj::mv(*this));
    chars = other.chars;
    other.chars = nullptr;
    return *this;
  }

  kj::ArrayPtr<const char> asChars() const {
    return chars;
  }

  kj::Maybe<kj::String> asKjString() {
    if (chars.begin() != nullptr) {
      return kj::str(chars);
    } else {
      return kj::none;
    }
  }

 private:
  kj::ArrayPtr<const char> chars;
};

// =======================================================================================
// Error checking for lol-html

kj::Maybe<kj::Exception> tryGetLastError() {
  auto maybeErrorString = lol_html_take_last_error();
  if (maybeErrorString.data == nullptr) {
    return kj::none;
  }
  auto errorString = LolString(maybeErrorString);
  return kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
      kj::str(JSG_EXCEPTION(TypeError) ": Parser error: ", errorString.asChars()));
}

void discardLastError() {
  auto drop = LolString(lol_html_take_last_error());
}

kj::Exception getLastError() {
  return KJ_REQUIRE_NONNULL(tryGetLastError(),
      "lol-html reported error through return value, but lol_html_take_last_error() is null");
}

int check(int rc) {
  if (rc == -1) {
    kj::throwFatalException(getLastError());
  }
  return rc;
}

template <typename T>
[[nodiscard]] T& check(T* ptr) {
  // Nodiscard, because this function typically checks references that must later be freed.
  if (ptr == nullptr) {
    kj::throwFatalException(getLastError());
  }
  return *ptr;
}

// Helper function to determine if a content token is still valid. Each content token has an
// implementation object inside a Maybe -- when HTMLRewriter::TokenScope (defined below)
// gets destroyed, that Maybe gets nullified, and the content token becomes a dead, useless,
// JavaScript object occupying space, waiting to get garbage collected.
//
// In other words, if you try to access a content token (Element, Text, etc.) outside of a
// content handler, you're gonna get this exception.
template <typename T>
decltype(auto) checkToken(kj::Maybe<T>& impl) {
  return JSG_REQUIRE_NONNULL(impl, TypeError,
      "This content token is no longer valid. Content tokens are only valid "
      "during the execution of the relevant content handler.");
}

}  // namespace

// =======================================================================================
// HTMLRewriter::TokenScope

class HTMLRewriter::TokenScope {
 public:
  template <typename T>
  explicit TokenScope(jsg::Ref<T>& value): contentToken(value.addRef()) {}
  ~TokenScope() noexcept(false) {
    KJ_IF_SOME(token, contentToken) {
      token->htmlContentScopeEnd();
    }
  }
  TokenScope(TokenScope&& o): contentToken(kj::mv(o.contentToken)) {
    o.contentToken = kj::none;
  }
  KJ_DISALLOW_COPY(TokenScope);

 private:
  kj::Maybe<jsg::Ref<HTMLRewriter::Token>> contentToken;
};

namespace {

// =======================================================================================
// Rewriter
using ElementCallbackFunction = HTMLRewriter::ElementCallbackFunction;

struct UnregisteredElementHandlers {
  kj::Own<lol_html_Selector> selector;

  // The actual handler functions. We store them as jsg::Values for compatibility with GcVisitor.

  jsg::Optional<ElementCallbackFunction> element;
  jsg::Optional<ElementCallbackFunction> comments;
  jsg::Optional<ElementCallbackFunction> text;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(element, comments, text);
  }

  JSG_MEMORY_INFO(UnregisteredElementHandlers) {
    tracker.trackField("element", element);
    tracker.trackField("comments", comments);
    tracker.trackField("text", text);
  }
};

struct UnregisteredDocumentHandlers {

  // The actual handler functions. We store them as jsg::Values for compatibility with GcVisitor.

  jsg::Optional<ElementCallbackFunction> doctype;
  jsg::Optional<ElementCallbackFunction> comments;
  jsg::Optional<ElementCallbackFunction> text;
  jsg::Optional<ElementCallbackFunction> end;

  // The `this` object used to call the handler functions.

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(doctype, comments, text, end);
  }

  JSG_MEMORY_INFO(UnregisteredDocumentHandlers) {
    tracker.trackField("doctype", doctype);
    tracker.trackField("comments", comments);
    tracker.trackField("text", text);
    tracker.trackField("end", end);
  }
};

using UnregisteredElementOrDocumentHandlers =
    kj::OneOf<UnregisteredElementHandlers, UnregisteredDocumentHandlers>;

}  // namespace

// Wrapper around an actual rewriter (streaming parser).
class Rewriter final: public WritableStreamSink {
 public:
  explicit Rewriter(jsg::Lock& js,
      kj::ArrayPtr<UnregisteredElementOrDocumentHandlers> unregisteredHandlers,
      kj::ArrayPtr<const char> encoding,
      kj::Own<WritableStreamSink> inner);
  KJ_DISALLOW_COPY_AND_MOVE(Rewriter);

  // WritableStreamSink implementation. The input body pumpTo() operation calls these.
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override;
  kj::Promise<void> end() override;
  void abort(kj::Exception reason) override;

  // Implementation for `Element::onEndTag` to avoid exposing private details of Rewriter.
  void onEndTag(lol_html_element_t* element, ElementCallbackFunction&& callback);

 private:
  // Wait for the write promise (if any) produced by our `output()` callback, then, if there is a
  // stored exception, abort the wrapped WritableStreamSink with it, then return the exception.
  // Otherwise, just return.
  kj::Promise<void> finishWrite();

  static kj::Own<lol_html_HtmlRewriter> buildRewriter(jsg::Lock& js,
      kj::ArrayPtr<UnregisteredElementOrDocumentHandlers> unregisteredHandlers,
      kj::ArrayPtr<const char> encoding,
      Rewriter& rewriterWrapper);

  static void output(const char* buffer, size_t size, void* userdata);
  void outputImpl(kj::ArrayPtr<const byte> buffer);

  void tryHandleCancellation(int rc) {
    if (canceled) {
      canceled = false;

      // We canceled this, which means we used LOL_HTML_STOP. That means we have an error sitting
      // in the error buffer in the lol-html C API. Let's make sure our return code is -1 and get
      // rid of that error value to make sure nobody picks it up later on accident and thinks an
      // error occured.
      KJ_ASSERT(rc == -1);
      discardLastError();

      throw kj::CanceledException{};
    }
  }

  friend class ::workerd::api::HTMLRewriter;

  struct RegisteredHandler {
    // A back-reference to the rewriter which owns this particular registered handler.
    Rewriter& rewriter;

    ElementCallbackFunction callback;
  };

  kj::Vector<kj::Own<RegisteredHandler>> registeredHandlers;
  // TODO(perf): Don't store Owns. We need to pass stable pointers as the userdata parameter to
  //   lol_html_rewriter_builder_add_*_content_handlers(), but don't have a really easy way to
  //   know precisely how many handlers we're going to register beforehand, so we need a vector. But
  //   vectors can grow, moving their objects around, invalidating pointers into their storage.

  // This is separate from `registeredHandlers` so we can delete them more eagerly when EndTags are
  // destroyed, and not have to look through all other handlers.
  kj::Vector<kj::Own<RegisteredHandler>> registeredEndTagHandlers;
  // TODO(perf) Don't store Owns, same as `registeredHandlers` above.

  template <typename T, typename CType = typename T::CType>
  static lol_html_rewriter_directive_t thunk(CType* content, void* userdata);
  template <typename T, typename CType = typename T::CType>
  lol_html_rewriter_directive_t thunkImpl(CType* content, RegisteredHandler& registration);
  template <typename T, typename CType = typename T::CType>
  kj::Promise<void> thunkPromise(CType* content, RegisteredHandler& registration);

  // Eagerly free this handler. Should only be called if we're confident the handler will never be
  // used again.
  void removeEndTagHandler(RegisteredHandler& registration);

  // Must be constructed AFTER the registered handler vector, since the function which constructs
  // this (buildRewriter()) modifies that vector.
  kj::Own<lol_html_HtmlRewriter> rewriter;

  kj::Own<WritableStreamSink> inner;

  kj::Maybe<kj::Promise<void>> writePromise;

  kj::Maybe<kj::Exception> maybeException;

  IoContext& ioContext;

  kj::Maybe<kj::WaitScope&> maybeWaitScope;

  bool canceled = false;

  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> maybeAsyncContext;

  bool isPoisoned() {
    // If a call to `lol-html` returned an error or propagated a user error from a handler
    // (LOL_HTML_STOP for instance); we consider its instance as poisoned. Future calls to
    // `lol_html_rewriter_write` and `lol_html_rewriter_end` will probably throw.
    return maybeException != kj::none;
  }

  void maybePoison(kj::Exception exception) {
    // Ignore this error if maybeException is already populated -- this error is probably just a
    // secondary effect.
    if (maybeException == kj::none) {
      maybeException = kj::mv(exception);
    }
  }
};

kj::Own<lol_html_HtmlRewriter> Rewriter::buildRewriter(jsg::Lock& js,
    kj::ArrayPtr<UnregisteredElementOrDocumentHandlers> unregisteredHandlers,
    kj::ArrayPtr<const char> encoding,
    Rewriter& rewriter) {
  auto builder = LOL_HTML_OWN(rewriter_builder, lol_html_rewriter_builder_new());

  auto registerCallback = [&](ElementCallbackFunction& callback) {
    auto registeredHandler = RegisteredHandler{rewriter, callback.addRef(js)};
    return rewriter.registeredHandlers.add(kj::heap(kj::mv(registeredHandler))).get();
  };

  for (auto& handlers: unregisteredHandlers) {
    KJ_SWITCH_ONEOF(handlers) {
      KJ_CASE_ONEOF(elementHandlers, UnregisteredElementHandlers) {
        auto element = elementHandlers.element.map(registerCallback);
        auto comments = elementHandlers.comments.map(registerCallback);
        auto text = elementHandlers.text.map(registerCallback);

        check(lol_html_rewriter_builder_add_element_content_handlers(builder,
            elementHandlers.selector, element == kj::none ? nullptr : &Rewriter::thunk<Element>,
            element.orDefault(nullptr), comments == kj::none ? nullptr : &Rewriter::thunk<Comment>,
            comments.orDefault(nullptr), text == kj::none ? nullptr : &Rewriter::thunk<Text>,
            text.orDefault(nullptr)));
      }
      KJ_CASE_ONEOF(documentHandlers, UnregisteredDocumentHandlers) {
        auto doctype = documentHandlers.doctype.map(registerCallback);
        auto comments = documentHandlers.comments.map(registerCallback);
        auto text = documentHandlers.text.map(registerCallback);
        auto end = documentHandlers.end.map(registerCallback);

        // Adding document content handlers cannot fail, so no need for check().
        lol_html_rewriter_builder_add_document_content_handlers(builder,
            doctype == kj::none ? nullptr : &Rewriter::thunk<Doctype>, doctype.orDefault(nullptr),
            comments == kj::none ? nullptr : &Rewriter::thunk<Comment>, comments.orDefault(nullptr),
            text == kj::none ? nullptr : &Rewriter::thunk<Text>, text.orDefault(nullptr),
            end == kj::none ? nullptr : &Rewriter::thunk<DocumentEnd>, end.orDefault(nullptr));
      }
    }
  }

  // `strict` mode will bail out from tokenization process in cases when
  // there is no way to determine correct parsing context. Recommended
  // setting for safety reasons.
  bool isStrict = true;

  // Configure a maximum memory limit that `lol-html` is allowed to use and
  // preallocate some memory for its internal buffer.
  lol_html_memory_settings_t memorySettings = {
    .preallocated_parsing_buffer_size = 1024, .max_allowed_memory_usage = 3 * 1024 * 1024};

  if (FeatureFlags::get(js).getEsiIncludeIsVoidTag()) {
    return LOL_HTML_OWN(rewriter,
        unstable_lol_html_rewriter_build_with_esi_tags(builder, encoding.begin(), encoding.size(),
            memorySettings, &Rewriter::output, &rewriter, isStrict));

  } else {
    return LOL_HTML_OWN(rewriter,
        lol_html_rewriter_build(builder, encoding.begin(), encoding.size(), memorySettings,
            &Rewriter::output, &rewriter, isStrict));
  }
}

Rewriter::Rewriter(jsg::Lock& js,
    kj::ArrayPtr<UnregisteredElementOrDocumentHandlers> unregisteredHandlers,
    kj::ArrayPtr<const char> encoding,
    kj::Own<WritableStreamSink> inner)
    : rewriter(buildRewriter(js, unregisteredHandlers, encoding, *this)),
      inner(kj::mv(inner)),
      ioContext(IoContext::current()),
      maybeAsyncContext(jsg::AsyncContextFrame::currentRef(js)) {}

namespace {

// The stack size floor enforced by kj. We could go lower,
// but it'd always be increased to this anyway.
const size_t FIBER_STACK_SIZE = 1024 * 64;

const kj::FiberPool& getFiberPool() {
  const static kj::FiberPool FIBER_POOL(FIBER_STACK_SIZE);
  return FIBER_POOL;
}

}  // namespace

kj::Promise<void> Rewriter::write(kj::ArrayPtr<const byte> buffer) {
  KJ_ASSERT(maybeWaitScope == kj::none);
  return getFiberPool().startFiber([this, buffer](kj::WaitScope& scope) {
    maybeWaitScope = scope;
    if (!isPoisoned()) {
      // Cannot use `check()` because `finishWrite()` implements the error path.
      auto rc = lol_html_rewriter_write(rewriter, buffer.asChars().begin(), buffer.size());
      tryHandleCancellation(rc);
      if (rc == -1) {
        maybePoison(getLastError());
      }
    }
    return finishWrite();
  });
}

kj::Promise<void> Rewriter::write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  KJ_ASSERT(maybeWaitScope == kj::none);
  return getFiberPool().startFiber([this, pieces](kj::WaitScope& scope) {
    maybeWaitScope = scope;
    if (!isPoisoned()) {
      for (auto bytes: pieces) {
        auto chars = bytes.asChars();
        // Cannot use `check()` because `finishWrite()` implements the error path.
        auto rc = lol_html_rewriter_write(rewriter, chars.begin(), chars.size());
        tryHandleCancellation(rc);
        if (rc == -1) {
          maybePoison(getLastError());
          // A handler threw an exception; stop calling `lol_html_rewriter_write()`.
          break;
        }
      }
    }
    return finishWrite();
  });
}

kj::Promise<void> Rewriter::end() {
  KJ_ASSERT(maybeWaitScope == kj::none);
  return getFiberPool().startFiber([this](kj::WaitScope& scope) {
    maybeWaitScope = scope;
    if (!isPoisoned()) {
      // Cannot use `check()` because `finishWrite()` implements the error path.
      auto rc = lol_html_rewriter_end(rewriter);
      tryHandleCancellation(rc);
      if (rc == -1) {
        maybePoison(getLastError());
      }
    }
    return finishWrite().then([this]() { return inner->end(); });
  });
}

void Rewriter::abort(kj::Exception reason) {
  // End the rewriter and forward the error to the wrapped output stream.
  maybeException = kj::cp(reason);

  inner->abort(kj::mv(reason));
}

kj::Promise<void> Rewriter::finishWrite() {
  maybeWaitScope = kj::none;
  auto checkException = [this]() -> kj::Promise<void> {
    KJ_ASSERT(writePromise == kj::none);

    KJ_IF_SOME(exception, maybeException) {
      inner->abort(kj::cp(exception));
      return kj::cp(exception);
    }

    return kj::READY_NOW;
  };

  KJ_IF_SOME(wp, writePromise) {
    KJ_DEFER(writePromise = kj::none);
    return wp.then([checkException]() { return checkException(); });
  }

  return checkException();
}

template <typename T, typename CType>
lol_html_rewriter_directive_t Rewriter::thunk(CType* content, void* userdata) {
  auto& registration = *reinterpret_cast<RegisteredHandler*>(userdata);
  return registration.rewriter.thunkImpl<T>(content, registration);
}

template <typename T, typename CType>
lol_html_rewriter_directive_t Rewriter::thunkImpl(
    CType* content, RegisteredHandler& registeredHandler) {
  if (isPoisoned()) {
    // Handlers disabled due to exception.
    KJ_LOG(ERROR, "poisoned rewriter should not be able to call handlers");
    return LOL_HTML_STOP;
  }

  try {
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&] {
      // V8 has a thread local pointer that points to where the stack limit is on this thread which
      // is tested for overflows when we enter any JS code. However since we're running in a fiber
      // here, we're in an entirely different stack that V8 doesn't know about, so it gets confused
      // and may think we've overflowed our stack. evalLater will run thunkPromise on the main stack
      // to keep V8 from getting confused.
      auto promise = kj::evalLater([&]() { return thunkPromise<T>(content, registeredHandler); });
      promise.wait(KJ_ASSERT_NONNULL(maybeWaitScope));
    })) {
      // Exception in handler. We need to abort the streaming parser, but can't do so just yet: we
      // need to unwind the stack because we're probably still inside a cool_thing_rewriter_write().
      // We can't unwind with an exception across the Rust/C++ boundary, so instead we'll keep this
      // exception around and disable all later handlers.
      maybePoison(kj::mv(exception));
      return LOL_HTML_STOP;
    }
  } catch (kj::CanceledException) {
    // The fiber is being canceled. Same as runCatchingExceptions, we need to abort the parser,
    // but can't since we're still inside cool_thing_rewriter_write(). This isn't handled by
    // runCatchingExceptions since CanceledException isn't a kj exception, and we wouldn't want
    // runCatchingExceptions to handle it anyway. We set canceled to true and once we leave Rust,
    // we rethrow it to properly cancel the fiber.
    canceled = true;
    return LOL_HTML_STOP;
  }
  return LOL_HTML_CONTINUE;
}

void Rewriter::removeEndTagHandler(RegisteredHandler& handler) {
  auto size = registeredEndTagHandlers.size();
  for (auto counter = size; counter != 0; --counter) {
    auto idx = counter - 1;
    if (registeredEndTagHandlers[idx].get() == &handler) {
      // equivalent of `Vec::swap_remove` in Rust
      if (counter != size) {
        registeredEndTagHandlers[idx] = kj::mv(registeredEndTagHandlers[size - 1]);
      }
      registeredEndTagHandlers.removeLast();
      break;
    }
  }
}

template <typename T, typename CType>
kj::Promise<void> Rewriter::thunkPromise(CType* content, RegisteredHandler& registeredHandler) {
  return ioContext.run(
      [this, content, &registeredHandler](Worker::Lock& lock) -> kj::Promise<void> {
    // We enter the AsyncContextFrame that was current when the Rewriter was created
    // (when transform() was called). If someone wants, instead, to use the context
    // that was current when on(...) is called, the ElementHandler can use AsyncResource
    // (or eventually the standard AsyncContext once that lands).
    jsg::AsyncContextFrame::Scope asyncContextScope(lock, maybeAsyncContext);
    auto jsContent = jsg::alloc<T>(*content, *this);
    auto scope = HTMLRewriter::TokenScope(jsContent);
    auto value = registeredHandler.callback(lock, kj::mv(jsContent));

    if constexpr (kj::isSameType<T, EndTag>()) {
      // TODO(someday): We can't unconditionally pop the top of `registeredEndTagHandlers`,
      //   because that depends on https://github.com/cloudflare/lol-html/issues/110
      //   being resolved. For now we let handles to end tag handlers tags live for the duration of
      //   the response transformation, but eagerly release ones that we can.
      //   In particular, note that `thunkPromise` is never called for implied end tags.
      removeEndTagHandler(registeredHandler);
    }

    return value.attach(kj::mv(scope));
  });
}

void Rewriter::onEndTag(lol_html_element_t* element, ElementCallbackFunction&& callback) {
  auto registeredHandler = Rewriter::RegisteredHandler{*this, kj::mv(callback)};
  // NOTE: this gets freed in `thunkPromise` above.
  // TODO(someday): this uses more memory than necessary for implied end tags, which lol-html
  // doesn't actually call `thunk` on.  LOL HTML drops the handler after it finishes transforming
  // the current element, but this code will keep it around until the entire HTML document is
  // transformed. It would be nice to free it directly after the handler is used; unfortunately,
  // this isn't trivial to do since we have no idea whether there's an end tag or not. The fix for
  // this probably needs to happen in lol-html; see #110.
  // WARNING: if we ever start reusing the same Rewriter for multiple documents,
  // this will cause a memory leak!
  auto& registeredHandlerPtr = registeredEndTagHandlers.add(kj::heap(kj::mv(registeredHandler)));
  lol_html_element_clear_end_tag_handlers(element);
  check(lol_html_element_add_end_tag_handler(
      element, Rewriter::thunk<EndTag>, registeredHandlerPtr.get()));
}

void Rewriter::output(const char* buffer, size_t size, void* userdata) {
  auto& rewriter = *reinterpret_cast<Rewriter*>(userdata);
  rewriter.outputImpl(kj::arrayPtr(buffer, size).asBytes());
}

void Rewriter::outputImpl(kj::ArrayPtr<const byte> buffer) {
  if (isPoisoned()) {
    // Handlers disabled due to exception or running in a destructor.
    return;
  }

  auto bufferCopy = kj::heapArray(buffer);
  KJ_IF_SOME(wp, writePromise) {
    writePromise = wp.then([this, bufferCopy = kj::mv(bufferCopy)]() mutable {
      return inner->write(bufferCopy.asPtr()).attach(kj::mv(bufferCopy));
    });
  } else {
    writePromise = inner->write(bufferCopy.asPtr()).attach(kj::mv(bufferCopy));
  }
}

// =======================================================================================
// Element

Element::Element(CType& element, Rewriter& rewriter) {
  impl.emplace(element, rewriter);
}

kj::String Element::getTagName() {
  auto tagName = LolString(lol_html_element_tag_name_get(&checkToken(impl).element));
  return kj::str(tagName.asChars());
}

void Element::setTagName(kj::String name) {
  check(lol_html_element_tag_name_set(&checkToken(impl).element, name.cStr(), name.size()));
}

bool Element::getRemoved() {
  return lol_html_element_is_removed(&checkToken(impl).element);
}

kj::StringPtr Element::getNamespaceURI() {
  // lol-html returns a static C string, no need to handle its lifetime.
  return lol_html_element_namespace_uri_get(&checkToken(impl).element);
}

jsg::Ref<Element::AttributesIterator> Element::getAttributes() {
  auto& implRef = checkToken(impl);

  auto iter = LOL_HTML_OWN(attributes_iterator, lol_html_attributes_iterator_get(&implRef.element));

  auto jsIter = jsg::alloc<Element::AttributesIterator>(kj::mv(iter));
  implRef.attributesIterators.add(jsIter.addRef());
  return kj::mv(jsIter);
}

kj::Maybe<kj::String> Element::getAttribute(kj::String name) {
  // NOTE: lol_html_element_get_attribute() returns NULL for both nonexistent attributes and for
  //   errors, so we can't use check() here.
  LolString attr(
      lol_html_element_get_attribute(&checkToken(impl).element, name.cStr(), name.size()));
  // TODO(perf): We could construct a v8::String directly here, saving a copy.
  kj::Maybe kjAttr = attr.asKjString();
  if (kjAttr != kj::none) {
    return kj::mv(kjAttr);
  }

  KJ_IF_SOME(exception, tryGetLastError()) {
    kj::throwFatalException(kj::mv(exception));
  }

  // No error, just doesn't exist.
  return kj::none;
}

bool Element::hasAttribute(kj::String name) {
  return !!check(
      lol_html_element_has_attribute(&checkToken(impl).element, name.cStr(), name.size()));
}

jsg::Ref<Element> Element::setAttribute(kj::String name, kj::String value) {
  check(lol_html_element_set_attribute(
      &checkToken(impl).element, name.cStr(), name.size(), value.cStr(), value.size()));

  return JSG_THIS;
}

jsg::Ref<Element> Element::removeAttribute(kj::String name) {
  check(lol_html_element_remove_attribute(&checkToken(impl).element, name.cStr(), name.size()));

  return JSG_THIS;
}

namespace {

kj::String unwrapContent(Content content) {
  return kj::mv(JSG_REQUIRE_NONNULL(content.tryGet<kj::String>(), TypeError,
      "Replacing HTML content using a ReadableStream or Response object is not "
      "implemented. You must provide a string."));
}

}  // namespace

#define DEFINE_CONTENT_REWRITER_FUNCTION(camel, snake)                                             \
  jsg::Ref<Element> Element::camel(Content content, jsg::Optional<ContentOptions> options) {       \
    auto stringContent = unwrapContent(kj::mv(content));                                           \
    check(lol_html_element_##snake(&checkToken(impl).element, stringContent.cStr(),                \
        stringContent.size(), options.orDefault({}).html.orDefault(false)));                       \
    return JSG_THIS;                                                                               \
  }

DEFINE_CONTENT_REWRITER_FUNCTION(before, before)
DEFINE_CONTENT_REWRITER_FUNCTION(after, after)
DEFINE_CONTENT_REWRITER_FUNCTION(prepend, prepend)
DEFINE_CONTENT_REWRITER_FUNCTION(append, append)
DEFINE_CONTENT_REWRITER_FUNCTION(replace, replace)
DEFINE_CONTENT_REWRITER_FUNCTION(setInnerContent, set_inner_content)

#undef DEFINE_CONTENT_REWRITER_FUNCTION

jsg::Ref<Element> Element::remove() {
  lol_html_element_remove(&checkToken(impl).element);
  return JSG_THIS;
}

jsg::Ref<Element> Element::removeAndKeepContent() {
  lol_html_element_remove_and_keep_content(&checkToken(impl).element);
  return JSG_THIS;
}

void Element::onEndTag(ElementCallbackFunction&& callback) {
  auto& knownImpl = checkToken(impl);
  knownImpl.rewriter.onEndTag(&knownImpl.element, kj::mv(callback));
}

EndTag::EndTag(CType& endTag, Rewriter&): impl(endTag) {}

void EndTag::htmlContentScopeEnd() {
  impl = kj::none;
}

kj::String EndTag::getName() {
  auto text = LolString(lol_html_end_tag_name_get(&checkToken(impl)));
  return kj::str(text.asChars());
}

void EndTag::setName(kj::String text) {
  check(lol_html_end_tag_name_set(&checkToken(impl), text.cStr(), text.size()));
}

jsg::Ref<EndTag> EndTag::before(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_end_tag_before(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<EndTag> EndTag::after(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_end_tag_after(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<EndTag> EndTag::remove() {
  lol_html_end_tag_remove(&checkToken(impl));
  return JSG_THIS;
}

void Element::htmlContentScopeEnd() {
  impl = kj::none;
}

Element::Impl::Impl(CType& element, Rewriter& rewriter): element(element), rewriter(rewriter) {}

Element::Impl::~Impl() noexcept(false) {
  for (auto& jsIter: attributesIterators) {
    static_cast<HTMLRewriter::Token&>(*jsIter).htmlContentScopeEnd();
  }
}

// =======================================================================================
// Element::AttributesIterator

Element::AttributesIterator::AttributesIterator(kj::Own<CType> iter): impl(kj::mv(iter)) {}

jsg::Ref<Element::AttributesIterator> Element::AttributesIterator::self() {
  return JSG_THIS;
}

Element::AttributesIterator::Next Element::AttributesIterator::next() {
  // NOTE: lol_html_attribute_t doesn't need to be freed.
  auto* attribute = lol_html_attributes_iterator_next(checkToken(impl));
  if (attribute == nullptr) {
    // End of iteration.
    // TODO(someday): Eagerly deallocate. Can't seem to nullify the Own without also nullifying the
    //   enclosing Maybe, however.
    return {true, kj::none};
  }

  auto name = LolString(lol_html_attribute_name_get(attribute));
  auto value = LolString(lol_html_attribute_value_get(attribute));

  return {false, kj::arr(kj::str(name.asChars()), kj::str(value.asChars()))};
}

void Element::AttributesIterator::htmlContentScopeEnd() {
  impl = kj::none;
}

// =======================================================================================
// Comment

Comment::Comment(CType& comment, Rewriter&): impl(comment) {}

kj::String Comment::getText() {
  auto text = LolString(lol_html_comment_text_get(&checkToken(impl)));
  return kj::str(text.asChars());
}

void Comment::setText(kj::String text) {
  check(lol_html_comment_text_set(&checkToken(impl), text.cStr(), text.size()));
}

bool Comment::getRemoved() {
  // NOTE: No error checking seems required by this function -- it returns a bool directly.
  return lol_html_comment_is_removed(&checkToken(impl));
}

jsg::Ref<Comment> Comment::before(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_comment_before(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<Comment> Comment::after(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_comment_after(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<Comment> Comment::replace(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_comment_replace(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<Comment> Comment::remove() {
  lol_html_comment_remove(&checkToken(impl));

  return JSG_THIS;
}

void Comment::htmlContentScopeEnd() {
  impl = kj::none;
}

// =======================================================================================
// Text

Text::Text(CType& text, Rewriter&): impl(text) {}

kj::String Text::getText() {
  auto content = lol_html_text_chunk_content_get(&checkToken(impl));
  return kj::heapString(content.data, content.len);
}

bool Text::getLastInTextNode() {
  // NOTE: No error checking seems required by this function -- it returns a bool directly.
  return lol_html_text_chunk_is_last_in_text_node(&checkToken(impl));
}

bool Text::getRemoved() {
  // NOTE: No error checking seems required by this function -- it returns a bool directly.
  return lol_html_text_chunk_is_removed(&checkToken(impl));
}

jsg::Ref<Text> Text::before(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_text_chunk_before(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<Text> Text::after(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_text_chunk_after(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<Text> Text::replace(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_text_chunk_replace(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

jsg::Ref<Text> Text::remove() {
  lol_html_text_chunk_remove(&checkToken(impl));

  return JSG_THIS;
}

void Text::htmlContentScopeEnd() {
  impl = kj::none;
}

// =======================================================================================
// Doctype

Doctype::Doctype(CType& doctype, Rewriter&): impl(doctype) {}

kj::Maybe<kj::String> Doctype::getName() {
  LolString name(lol_html_doctype_name_get(&checkToken(impl)));
  return name.asKjString();
}

kj::Maybe<kj::String> Doctype::getPublicId() {
  LolString publicId(lol_html_doctype_public_id_get(&checkToken(impl)));
  return publicId.asKjString();
}

kj::Maybe<kj::String> Doctype::getSystemId() {
  LolString systemId(lol_html_doctype_system_id_get(&checkToken(impl)));
  return systemId.asKjString();
}

void Doctype::htmlContentScopeEnd() {
  impl = kj::none;
}

// =======================================================================================
// DocumentEnd

DocumentEnd::DocumentEnd(CType& documentEnd, Rewriter&): impl(documentEnd) {}

jsg::Ref<DocumentEnd> DocumentEnd::append(Content content, jsg::Optional<ContentOptions> options) {
  auto stringContent = unwrapContent(kj::mv(content));
  check(lol_html_doc_end_append(&checkToken(impl), stringContent.cStr(), stringContent.size(),
      options.orDefault({}).html.orDefault(false)));

  return JSG_THIS;
}

void DocumentEnd::htmlContentScopeEnd() {
  impl = kj::none;
}

// =======================================================================================
// HTMLRewriter

struct HTMLRewriter::Impl {
  // The list of handlers added to this builder.
  kj::Vector<UnregisteredElementOrDocumentHandlers> unregisteredHandlers;
  // TODO(perf): It'd be nice to eagerly register handlers on the native builder object. However,
  //   currently lol-html rewriters are inextricably linked to the builders which created them,
  //   and this has concurrency and reentrancy ramifications: two rewriters built from the same
  //   builder require synchronization to access safely, and their callbacks must not use the
  //   builder which created them, lest the process deadlock.
  //
  //   In the meantime, we keep this list of handlers around and "replay" their registration, in
  //   order, on the builder object that we create inside of .transform().

  JSG_MEMORY_INFO(HTMLRewriter::Impl) {
    for (const auto& handlers: unregisteredHandlers) {
      KJ_SWITCH_ONEOF(handlers) {
        KJ_CASE_ONEOF(h, UnregisteredElementHandlers) {
          tracker.trackField(nullptr, h);
        }
        KJ_CASE_ONEOF(h, UnregisteredDocumentHandlers) {
          tracker.trackField(nullptr, h);
        }
      }
    }
  }
};

HTMLRewriter::HTMLRewriter(): impl(kj::heap<Impl>()) {}
HTMLRewriter::~HTMLRewriter() noexcept(false) {}

void HTMLRewriter::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("impl", impl);
}

jsg::Ref<HTMLRewriter> HTMLRewriter::constructor() {
  return jsg::alloc<HTMLRewriter>();
}

jsg::Ref<HTMLRewriter> HTMLRewriter::on(
    kj::String stringSelector, ElementContentHandlers&& handlers) {
  kj::Own<lol_html_Selector> selector =
      LOL_HTML_OWN(selector, lol_html_selector_parse(stringSelector.cStr(), stringSelector.size()));

  impl->unregisteredHandlers.add(UnregisteredElementHandlers{
    kj::mv(selector), kj::mv(handlers.element), kj::mv(handlers.comments), kj::mv(handlers.text)});

  return JSG_THIS;
}

jsg::Ref<HTMLRewriter> HTMLRewriter::onDocument(DocumentContentHandlers&& handlers) {
  impl->unregisteredHandlers.add(UnregisteredDocumentHandlers{kj::mv(handlers.doctype),
    kj::mv(handlers.comments), kj::mv(handlers.text), kj::mv(handlers.end)});

  return JSG_THIS;
}

jsg::Ref<Response> HTMLRewriter::transform(jsg::Lock& js, jsg::Ref<Response> response) {
  auto maybeInput = response->getBody();

  if (maybeInput == kj::none) {
    // That was easy!
    return kj::mv(response);
  }

  auto& ioContext = IoContext::current();

  auto pipe = newIdentityPipe();
  response = Response::constructor(
      js, kj::Maybe(jsg::alloc<ReadableStream>(ioContext, kj::mv(pipe.in))), kj::mv(response));

  kj::String ownContentType;
  kj::String encoding = kj::str("utf-8");
  auto contentTypeKey = jsg::ByteString(kj::str("content-type"));
  KJ_IF_SOME(contentType, response->getHeaders(js)->get(kj::mv(contentTypeKey))) {
    // TODO(cleanup): readContentTypeParameter can be replaced with using
    // workerd/util/mimetype.h directly.
    KJ_IF_SOME(charset, readContentTypeParameter(contentType, "charset")) {
      ownContentType = kj::mv(contentType);
      encoding = kj::mv(charset);
    }
  }

  auto rewriter = kj::heap<Rewriter>(js, impl->unregisteredHandlers, encoding, kj::mv(pipe.out));

  // NOTE: Avoid throwing any exceptions after initiating the pump below. This makes
  //   the input response object disturbed (response.bodyUsed === true), which should only happen
  //   after we know that nothing else (like invalid encoding) could cause an exception.

  // Drive and flush the parser asynchronously.
  ioContext.addTask(ioContext.waitForDeferredProxy(
      KJ_ASSERT_NONNULL(maybeInput)->pumpTo(js, kj::mv(rewriter), true)));

  // TODO(soon): EW-2025 Make Rewriter a proper wrapper object and put it in hidden property on the
  //   response so the GC can find the handlers which Rewriter co-owns.
  return kj::mv(response);
}

void HTMLRewriter::visitForGc(jsg::GcVisitor& visitor) {
  for (auto& handlers: impl->unregisteredHandlers) {
    KJ_SWITCH_ONEOF(handlers) {
      KJ_CASE_ONEOF(elementHandlers, UnregisteredElementHandlers) {
        visitor.visit(elementHandlers);
      }
      KJ_CASE_ONEOF(documentHandlers, UnregisteredDocumentHandlers) {
        visitor.visit(documentHandlers);
      }
    }
  }
}

}  // namespace workerd::api
