// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8.h>
#include <workerd/api/http.h>
#include <workerd/jsg/jsg.h>

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

KJ_DECLARE_NON_POLYMORPHIC(lol_html_AttributesIterator);

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
  KJ_DISALLOW_COPY_AND_MOVE(HTMLRewriter);

  static jsg::Ref<HTMLRewriter> constructor();

  using ElementCallback = kj::Promise<void>(jsg::Ref<jsg::Object> element);
  using ElementCallbackFunction = jsg::Function<ElementCallback>;

  // A struct-like wrapper around element content handlers. I say struct-like, because we only use
  // this wrapper as a convenience to help us access the three function properties that we expect
  // to find. In reality, this is more like a "callback interface" in Web IDL terms, since we hang
  // onto the original object so that we can use it as the `this` argument.
  struct ElementContentHandlers {
    jsg::Optional<ElementCallbackFunction> element;
    jsg::Optional<ElementCallbackFunction> comments;
    jsg::Optional<ElementCallbackFunction> text;

    JSG_STRUCT(element, comments, text);

    JSG_STRUCT_TS_OVERRIDE({
      element?(element: Element): void | Promise<void>;
      comments?(comment: Comment): void | Promise<void>;
      text?(element: Text): void | Promise<void>;
    });
    // Specify parameter types for callback functions
  };

  // A struct-like wrapper around document content handlers. See the doc comment on
  // ElementContentHandlers for more information on its idiosyncrasies.
  struct DocumentContentHandlers {
    jsg::Optional<ElementCallbackFunction> doctype;
    jsg::Optional<ElementCallbackFunction> comments;
    jsg::Optional<ElementCallbackFunction> text;
    jsg::Optional<ElementCallbackFunction> end;

    JSG_STRUCT(doctype, comments, text, end);

    JSG_STRUCT_TS_OVERRIDE({
      doctype?(doctype: Doctype): void | Promise<void>;
      comments?(comment: Comment): void | Promise<void>;
      text?(text: Text): void | Promise<void>;
      end?(end: DocumentEnd): void | Promise<void>;
    });
    // Specify parameter types for callback functions
  };

  jsg::Ref<HTMLRewriter> on(kj::String selector, ElementContentHandlers&& handlers);
  jsg::Ref<HTMLRewriter> onDocument(DocumentContentHandlers&& handlers);

  // Register element or doctype content handlers. `handlers` must be unwrappable into a
  // ElementContentHandlers or DocumentContentHandlers struct, respectively. We take it as a
  // v8::Object so that we can use it as the `this` argument for the function calls.

  // Create a new Response object that is identical to the input response except that its body is
  // the result of running the original body through this HTMLRewriter's rewriter. This
  // function does not run the parser itself -- to drive the parser, you must read the transformed
  // response body.
  //
  // Pre-condition: the input response body is not disturbed.
  // Post-condition: the input response body is disturbed.
  jsg::Ref<Response> transform(jsg::Lock& js, jsg::Ref<Response> response);

  JSG_RESOURCE_TYPE(HTMLRewriter) {
    JSG_METHOD(on);
    JSG_METHOD(onDocument);
    JSG_METHOD(transform);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

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
//
// Note, when generating TypeScript types, definitions to include are collected before overrides are
// applied. Because ElementCallbackFunction's parameter is always jsg::Ref<jsg::Object> and not the
// token type, we would not include token types by default as these are only defined in overrides.
// Therefore, we manually define each token type as a JSG_TS_ROOT(), so it gets visited when
// collecting definitions.

class HTMLRewriter::Token: public jsg::Object {
public:
  virtual void htmlContentScopeEnd() = 0;
};

// A chunk of text or HTML which can be passed to content token mutation functions.
using Content = kj::OneOf<kj::String, jsg::Ref<ReadableStream>, jsg::Ref<Response>>;
// TODO(soon): Support ReadableStream/Response types. Requires fibers or lol-html saveable state.

// Options bag which can be passed to content token mutation functions.
struct ContentOptions {
  // True if the Content being passed to the mutation function is HTML. If false, the content will
  // be escaped (HTML entity-encoded).
  jsg::Optional<bool> html;

  JSG_STRUCT(html);
};

class Rewriter;

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

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      before(content: string, options?: ContentOptions): Element;
      after(content: string, options?: ContentOptions): Element;
      prepend(content: string, options?: ContentOptions): Element;
      append(content: string, options?: ContentOptions): Element;
      replace(content: string, options?: ContentOptions): Element;
      setInnerContent(content: string, options?: ContentOptions): Element;

      onEndTag(handler: (tag: EndTag) => void | Promise<void>): void;
    });
    // Require content to be a string, and specify parameter type for onEndTag
    // callback function
  }

private:
  struct Impl {
    Impl(CType& element, Rewriter&);
    KJ_DISALLOW_COPY_AND_MOVE(Impl);
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

  // lol_html_AttributesIterator has the distinction of being valid only during a content handler
  // execution scope AND also requiring manual deallocation, so this takes an Own<T> rather than T&.
  explicit AttributesIterator(kj::Own<CType> iter);

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

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      before(content: string, options?: ContentOptions): EndTag;
      after(content: string, options?: ContentOptions): EndTag;
    });
    // Require content to be a string
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

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      before(content: string, options?: ContentOptions): Comment;
      after(content: string, options?: ContentOptions): Comment;
      replace(content: string, options?: ContentOptions): Comment;
    });
    // Require content to be a string
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

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      before(content: string, options?: ContentOptions): Text;
      after(content: string, options?: ContentOptions): Text;
      replace(content: string, options?: ContentOptions): Text;
    });
    // Require content to be a string
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

    JSG_TS_ROOT();
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

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      append(content: string, options?: ContentOptions): DocumentEnd;
    });
    // Require content to be a string
  }

private:
  kj::Maybe<CType&> impl;

  void htmlContentScopeEnd() override;
};

#define EW_HTML_REWRITER_ISOLATE_TYPES                                                             \
  api::ContentOptions, api::HTMLRewriter, api::HTMLRewriter::ElementContentHandlers,               \
      api::HTMLRewriter::DocumentContentHandlers, api::Doctype, api::Element, api::EndTag,         \
      api::Comment, api::Text, api::DocumentEnd, api::Element::AttributesIterator,                 \
      api::Element::AttributesIterator::Next

}  // namespace workerd::api
