// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <v8.h>
#include "http.h"
#include "c-api/include/lol_html.h"

struct lol_html_HtmlRewriterBuilder;
struct lol_html_HtmlRewriter;
struct lol_html_Doctype;
struct lol_html_DocumentEnd;
struct lol_html_Comment;
struct lol_html_TextChunk;
struct lol_html_Element;
struct lol_html_EndTag;
struct lol_html_AttributesIterator;
struct lol_html_Attribute;
// Defined in lol_html.h, forward declarations mirrored here so we don't need the header.

struct lol_html_HtmlRewriter {};
struct lol_html_HtmlRewriterBuilder {};
struct lol_html_AttributesIterator {};
struct lol_html_Selector {};
// TODO(cleanup): These are defined internally in lol-html, but kj::Own<T> needs to check whether
//   or not T is polymorphic, so here's a dummy definition.

namespace workerd::api {

class Element;
class EndTag;
class Comment;
class Text;
class Doctype;
class DocumentEnd;

// =======================================================================================
// HTMLRewriter

class HTMLRewriter: public jsg::Object {
public:
  class Token;
  class TokenScope;

  explicit HTMLRewriter();
  ~HTMLRewriter() noexcept(false);
  KJ_DISALLOW_COPY(HTMLRewriter);

  static jsg::Ref<HTMLRewriter> constructor();

  using ElementCallback = kj::Promise<void>(jsg::Ref<jsg::Object> element);
  using ElementCallbackFunction = jsg::Function<ElementCallback>;

  struct ElementContentHandlers {
    // A struct-like wrapper around element content handlers. I say struct-like, because we only use
    // this wrapper as a convenience to help us access the three function properties that we expect
    // to find. In reality, this is more like a "callback interface" in Web IDL terms, since we hang
    // onto the original object so that we can use it as the `this` argument.

    jsg::Optional<ElementCallbackFunction> element;
    jsg::Optional<ElementCallbackFunction> comments;
    jsg::Optional<ElementCallbackFunction> text;

    JSG_STRUCT(element, comments, text);
  };

  struct DocumentContentHandlers {
    // A struct-like wrapper around document content handlers. See the doc comment on
    // ElementContentHandlers for more information on its idiosyncrasies.

    jsg::Optional<ElementCallbackFunction> doctype;
    jsg::Optional<ElementCallbackFunction> comments;
    jsg::Optional<ElementCallbackFunction> text;
    jsg::Optional<ElementCallbackFunction> end;

    JSG_STRUCT(doctype, comments, text, end);
  };

  jsg::Ref<HTMLRewriter> on(kj::String selector, ElementContentHandlers&& handlers);
  jsg::Ref<HTMLRewriter> onDocument(DocumentContentHandlers&& handlers);

  // Register element or doctype content handlers. `handlers` must be unwrappable into a
  // ElementContentHandlers or DocumentContentHandlers struct, respectively. We take it as a
  // v8::Object so that we can use it as the `this` argument for the function calls.

  jsg::Ref<Response> transform(jsg::Lock& js, jsg::Ref<Response> response,
      CompatibilityFlags::Reader featureFlags);
  // Create a new Response object that is identical to the input response except that its body is
  // the result of running the original body through this HTMLRewriter's rewriter. This
  // function does not run the parser itself -- to drive the parser, you must read the transformed
  // response body.
  //
  // Pre-condition: the input response body is not disturbed.
  // Post-condition: the input response body is disturbed.

  JSG_RESOURCE_TYPE(HTMLRewriter) {
    JSG_METHOD(on);
    JSG_METHOD(onDocument);
    JSG_METHOD(transform);
  }

private:
  void visitForGc(jsg::GcVisitor& visitor);

  struct Impl;
  kj::Own<Impl> impl;
};

// =======================================================================================
// HTML Content Tokens
//
// HTML content tokens represent individual chunks of HTML that scripts can manipulate. There are
// four types: Element, Comment, Text, and Doctype. Each one wraps a corresponding lower-level
// handle that lol-html passes to our callbacks. These lower-level handles are only valid during
// the execution (scope) of the callback, which the JS wrapper objects can obviously outlive. To
// cope with that, we have some machinery (HTMLRewriter::TokenScope/Token) which enforces
// in-scope access.
//
// The Element content token also exposes an AttributesIterator type. This is not a content token
// per se, but follows the same scoping rule.

class HTMLRewriter::Token: public jsg::Object {
public:
  virtual void htmlContentScopeEnd() = 0;
};

using Content = kj::OneOf<kj::String, jsg::Ref<ReadableStream>, jsg::Ref<Response>>;
// A chunk of text or HTML which can be passed to content token mutation functions.
//
// TODO(soon): Support ReadableStream/Response types. Requires fibers or lol-html saveable state.

struct ContentOptions {
  // Options bag which can be passed to content token mutation functions.

  jsg::Optional<bool> html;
  // True if the Content being passed to the mutation function is HTML. If false, the content will
  // be escaped (HTML entity-encoded).

  JSG_STRUCT(html);
};

class LolString {
  // RAII helper for lol_html_str_t.
  //
  // We cannot use a kj::Own<T> because lol_html_str_t is a struct, not a pointer, so instead we
  // have this LolString RAII wrapper.
  //
  // Use `kj::str(LolString.asChars())` to allocate your own copy of a LolString.

public:
  explicit LolString(lol_html_str_t s): chars(s.data, s.len) {}
  ~LolString() noexcept(false) {
    lol_html_str_free({ chars.begin(), chars.size() });
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

  kj::ArrayPtr<const char> asChars() const { return chars; }

  kj::Maybe<kj::String> asKjString() {
    if (chars.begin() != nullptr) {
      return kj::str(chars);
    } else {
      return nullptr;
    }
  }

private:
  kj::ArrayPtr<const char> chars;
};

inline void discardLastError() {
  auto drop = LolString(lol_html_take_last_error());
}

using ElementCallbackFunction = HTMLRewriter::ElementCallbackFunction;

struct UnregisteredElementHandlers {
  kj::Own<lol_html_Selector> selector;

  jsg::Optional<ElementCallbackFunction> element;
  jsg::Optional<ElementCallbackFunction> comments;
  jsg::Optional<ElementCallbackFunction> text;
  // The actual handler functions. We store them as jsg::Values for compatibility with GcVisitor.

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(element, comments, text);
  }
};

struct UnregisteredDocumentHandlers {
  jsg::Optional<ElementCallbackFunction> doctype;
  jsg::Optional<ElementCallbackFunction> comments;
  jsg::Optional<ElementCallbackFunction> text;
  jsg::Optional<ElementCallbackFunction> end;
  // The actual handler functions. We store them as jsg::Values for compatibility with GcVisitor.

  // The `this` object used to call the handler functions.

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(doctype, comments, text, end);
  }
};


using UnregisteredElementOrDocumentHandlers =
    kj::OneOf<UnregisteredElementHandlers, UnregisteredDocumentHandlers>;

class Rewriter final: public WritableStreamSink {
  // Wrapper around an actual rewriter (streaming parser).

public:
  explicit Rewriter(
      jsg::Lock& js,
      kj::ArrayPtr<UnregisteredElementOrDocumentHandlers> unregisteredHandlers,
      kj::ArrayPtr<const char> encoding,
      kj::Own<WritableStreamSink> inner,
      CompatibilityFlags::Reader featureFlags);
  KJ_DISALLOW_COPY(Rewriter);

  // WritableStreamSink implementation. The input body pumpTo() operation calls these.
  kj::Promise<void> write(const void* buffer, size_t size) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override;
  kj::Promise<void> end() override;
  void abort(kj::Exception reason) override;

  // Implementation for `Element::onEndTag` to avoid exposing private details of Rewriter.
  void onEndTag(lol_html_element_t *element, ElementCallbackFunction&& callback);

  // Exposed so we can use it in the stack overflow html-rewriter-test.
  void outputImpl(const char* buffer, size_t size);

private:
  kj::Promise<void> finishWrite();
  // Wait for the write promise (if any) produced by our `output()` callback, then, if there is a
  // stored exception, abort the wrapped WritableStreamSink with it, then return the exception.
  // Otherwise, just return.

  static kj::Own<lol_html_HtmlRewriter> buildRewriter(jsg::Lock& js,
      kj::ArrayPtr<UnregisteredElementOrDocumentHandlers> unregisteredHandlers,
      kj::ArrayPtr<const char> encoding, Rewriter& rewriterWrapper,
      CompatibilityFlags::Reader featureFlags);

  static void output(const char* buffer, size_t size, void* userdata);


  void tryHandleCancellation(int rc) {
    if (canceled) {
      canceled = false;

      // We canceled this, which means we used LOL_HTML_STOP. That means we have an error sitting
      // in the error buffer in the lol-html C API. Let's make sure our return code is -1 and get
      // rid of that error value to make sure nobody picks it up later on accident and thinks an
      // error occured.
      KJ_ASSERT(rc == -1);
      discardLastError();

      throw kj::CanceledException { };
    }
  }

  friend class ::workerd::api::HTMLRewriter;

  struct RegisteredHandler {
    Rewriter& rewriter;
    // A back-reference to the rewriter which owns this particular registered handler.

    ElementCallbackFunction callback;
  };

  kj::Vector<kj::Own<RegisteredHandler>> registeredHandlers;
  // TODO(perf): Don't store Owns. We need to pass stable pointers as the userdata parameter to
  //   lol_html_rewriter_builder_add_*_content_handlers(), but don't have a really easy way to
  //   know precisely how many handlers we're going to register beforehand, so we need a vector. But
  //   vectors can grow, moving their objects around, invalidating pointers into their storage.

  kj::Vector<kj::Own<RegisteredHandler>> registeredEndTagHandlers;
  // This is separate from `registeredHandlers` so we can delete them more eagerly when EndTags are
  // destroyed, and not have to look through all other handlers.
  // TODO(perf) Don't store Owns, same as `registeredHandlers` above.

  template <typename T, typename CType = typename T::CType>
  static lol_html_rewriter_directive_t thunk(CType* content, void* userdata);
  template <typename T, typename CType = typename T::CType>
  lol_html_rewriter_directive_t thunkImpl( CType* content, RegisteredHandler& registration);
  template <typename T, typename CType = typename T::CType>
  kj::Promise<void> thunkPromise( CType* content, RegisteredHandler& registration);

  void removeEndTagHandler(RegisteredHandler& registration);
  // Eagerly free this handler. Should only be called if we're confident the handler will never be
  // used again.

  kj::Own<lol_html_HtmlRewriter> rewriter;
  // Must be constructed AFTER the registered handler vector, since the function which constructs
  // this (buildRewriter()) modifies that vector.

  kj::Own<WritableStreamSink> inner;

  kj::Maybe<kj::Promise<void>> writePromise;

  kj::Maybe<kj::Exception> maybeException;

  IoContext& ioContext;

  kj::Maybe<kj::WaitScope&> maybeWaitScope;

  bool canceled = false;

  bool isPoisoned() {
    // If a call to `lol-html` returned an error or propagated a user error from a handler
    // (LOL_HTML_STOP for instance); we consider its instance as poisoned. Future calls to
    // `lol_html_rewriter_write` and `lol_html_rewriter_end` will probably throw.
    return maybeException != nullptr;
  }

  void maybePoison(kj::Exception exception) {
    // Ignore this error if maybeException is already populated -- this error is probably just a
    // secondary effect.
    if (maybeException == nullptr) {
      maybeException = kj::mv(exception);
    }
  }
};

class Element final: public HTMLRewriter::Token {
public:
  using CType = lol_html_Element;

  explicit Element(CType& element, Rewriter& wrapper);

  kj::String getTagName();
  void setTagName(kj::String tagName);

  class AttributesIterator;
  jsg::Ref<AttributesIterator> getAttributes();

  bool getRemoved();

  kj::StringPtr getNamespaceURI();

  kj::Maybe<kj::String> getAttribute(kj::String name);
  bool hasAttribute(kj::String name);
  jsg::Ref<Element> setAttribute(kj::String name, kj::String value);
  jsg::Ref<Element> removeAttribute(kj::String name);

  jsg::Ref<Element> before(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Element> after(Content content, jsg::Optional<ContentOptions> options);

  jsg::Ref<Element> prepend(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Element> append(Content content, jsg::Optional<ContentOptions> options);

  jsg::Ref<Element> replace(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Element> setInnerContent(Content content, jsg::Optional<ContentOptions> options);

  jsg::Ref<Element> remove();
  jsg::Ref<Element> removeAndKeepContent();

  void onEndTag(HTMLRewriter::ElementCallbackFunction&& callback);

  JSG_RESOURCE_TYPE(Element) {
    JSG_INSTANCE_PROPERTY(tagName, getTagName, setTagName);
    JSG_READONLY_INSTANCE_PROPERTY(attributes, getAttributes);
    JSG_READONLY_INSTANCE_PROPERTY(removed, getRemoved);
    JSG_READONLY_INSTANCE_PROPERTY(namespaceURI, getNamespaceURI);

    JSG_METHOD(getAttribute);
    JSG_METHOD(hasAttribute);
    JSG_METHOD(setAttribute);
    JSG_METHOD(removeAttribute);
    JSG_METHOD(before);
    JSG_METHOD(after);
    JSG_METHOD(prepend);
    JSG_METHOD(append);
    JSG_METHOD(replace);
    JSG_METHOD(remove);
    JSG_METHOD(removeAndKeepContent);
    JSG_METHOD(setInnerContent);
    JSG_METHOD(onEndTag);
  }

private:
  struct Impl {
    Impl(CType& element, Rewriter&);
    KJ_DISALLOW_COPY(Impl);
    ~Impl() noexcept(false);

    CType& element;
    kj::Vector<jsg::Ref<AttributesIterator>> attributesIterators;
    Rewriter& rewriter;
  };

  kj::Maybe<Impl> impl;

  void htmlContentScopeEnd() override;
};

class Element::AttributesIterator final: public HTMLRewriter::Token {
public:
  using CType = lol_html_AttributesIterator;

  explicit AttributesIterator(kj::Own<CType> iter);
  // lol_html_AttributesIterator has the distinction of being valid only during a content handler
  // execution scope AND also requiring manual deallocation, so this takes an Own<T> rather than T&.

  struct Next {
    bool done;
    jsg::Optional<kj::Array<kj::String>> value;

    JSG_STRUCT(done, value);
  };

  Next next();

  jsg::Ref<AttributesIterator> self();

  JSG_RESOURCE_TYPE(AttributesIterator) {
    JSG_INHERIT_INTRINSIC(v8::kIteratorPrototype);
    JSG_METHOD(next);
    JSG_ITERABLE(self);
  }

private:
  kj::Maybe<kj::Own<CType>> impl;

  void htmlContentScopeEnd() override;
};

class EndTag final: public HTMLRewriter::Token {
public:
  using CType = lol_html_EndTag;

  explicit EndTag(CType& tag, Rewriter&);

  kj::String getName();
  void setName(kj::String);

  jsg::Ref<EndTag> before(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<EndTag> after(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<EndTag> remove();

  JSG_RESOURCE_TYPE(EndTag) {
    JSG_INSTANCE_PROPERTY(name, getName, setName);

    JSG_METHOD(before);
    JSG_METHOD(after);
    JSG_METHOD(remove);
  }

private:
  kj::Maybe<CType&> impl;

  void htmlContentScopeEnd() override;
};

class Comment final: public HTMLRewriter::Token {
public:
  using CType = lol_html_Comment;

  explicit Comment(CType& comment, Rewriter&);

  kj::String getText();
  void setText(kj::String);

  bool getRemoved();

  jsg::Ref<Comment> before(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Comment> after(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Comment> replace(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Comment> remove();

  JSG_RESOURCE_TYPE(Comment) {
    JSG_INSTANCE_PROPERTY(text, getText, setText);
    JSG_READONLY_INSTANCE_PROPERTY(removed, getRemoved);

    JSG_METHOD(before);
    JSG_METHOD(after);
    JSG_METHOD(replace);
    JSG_METHOD(remove);
  }

private:
  kj::Maybe<CType&> impl;

  void htmlContentScopeEnd() override;
};

class Text final: public HTMLRewriter::Token {
public:
  using CType = lol_html_TextChunk;

  explicit Text(CType& text, Rewriter&);

  kj::String getText();

  bool getLastInTextNode();

  bool getRemoved();

  jsg::Ref<Text> before(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Text> after(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Text> replace(Content content, jsg::Optional<ContentOptions> options);
  jsg::Ref<Text> remove();

  JSG_RESOURCE_TYPE(Text) {
    JSG_READONLY_INSTANCE_PROPERTY(text, getText);
    JSG_READONLY_INSTANCE_PROPERTY(lastInTextNode, getLastInTextNode);
    JSG_READONLY_INSTANCE_PROPERTY(removed, getRemoved);

    JSG_METHOD(before);
    JSG_METHOD(after);
    JSG_METHOD(replace);
    JSG_METHOD(remove);
  }

private:
  kj::Maybe<CType&> impl;

  void htmlContentScopeEnd() override;
};

class Doctype final: public HTMLRewriter::Token {
public:
  using CType = lol_html_Doctype;

  explicit Doctype(CType& doctype, Rewriter&);

  kj::Maybe<kj::String> getName();
  kj::Maybe<kj::String> getPublicId();
  kj::Maybe<kj::String> getSystemId();

  JSG_RESOURCE_TYPE(Doctype) {
    JSG_READONLY_INSTANCE_PROPERTY(name, getName);
    JSG_READONLY_INSTANCE_PROPERTY(publicId, getPublicId);
    JSG_READONLY_INSTANCE_PROPERTY(systemId, getSystemId);
  }

private:
  kj::Maybe<CType&> impl;

  void htmlContentScopeEnd() override;
};

class DocumentEnd final: public HTMLRewriter::Token {
public:
  using CType = lol_html_DocumentEnd;

  explicit DocumentEnd(CType& documentEnd, Rewriter&);

  jsg::Ref<DocumentEnd> append(Content content, jsg::Optional<ContentOptions> options);

  JSG_RESOURCE_TYPE(DocumentEnd) {
    JSG_METHOD(append);
  }

private:
  kj::Maybe<CType&> impl;

  void htmlContentScopeEnd() override;
};

#define EW_HTML_REWRITER_ISOLATE_TYPES          \
  api::ContentOptions,                          \
  api::HTMLRewriter,                            \
  api::HTMLRewriter::ElementContentHandlers,    \
  api::HTMLRewriter::DocumentContentHandlers,   \
  api::Doctype,                                 \
  api::Element,                                 \
  api::EndTag,                                  \
  api::Comment,                                 \
  api::Text,                                    \
  api::DocumentEnd,                             \
  api::Element::AttributesIterator,             \
  api::Element::AttributesIterator::Next

}  // namespace workerd::api
