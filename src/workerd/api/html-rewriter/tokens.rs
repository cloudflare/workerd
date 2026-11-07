// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Token handles: one resource per lol-html token type, each with one method per operation.
//!
//! A handle reaches its token only while lol-html runs the handler it was created for: until the
//! handler returns, or, on a fiber, until the promise it returns settles. Then every method fails
//! with the `TypeError` that C++ `HTMLRewriter` throws for a token used outside its handler,
//! whatever JavaScript does with the handle.

use std::sync::Arc;

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use lol_html::html_content::Comment;
use lol_html::html_content::ContentType;
use lol_html::html_content::Doctype;
use lol_html::html_content::DocumentEnd;
use lol_html::html_content::EndTag;
use lol_html::html_content::StreamingHandler;
use lol_html::html_content::StreamingHandlerSink;
use lol_html::html_content::TextChunk;

use crate::parser_error;
use crate::rewriter::Element;
use crate::rewriter::HandlerKind;
use crate::rewriter::Replacer;
use crate::rewriter::Shared;
use crate::rewriter::Token;
use crate::slot::TokenSlot;

const INVALID_TOKEN: &str = "This content token is no longer valid. Content tokens are only \
                             valid during the execution of the relevant content handler.";

const fn content_type(html: bool) -> ContentType {
    if html {
        ContentType::Html
    } else {
        ContentType::Text
    }
}

/// The streaming content numbered `id`, as HTML or as text.
fn replacer(id: u32, html: bool) -> Box<dyn StreamingHandler + Send> {
    Box::new(Replacer {
        id,
        content_type: content_type(html),
    })
}

/// A slot of any token type, which [`lend_slot`] fills and [`handle`] makes a handle of.
pub enum Slot {
    Element(TokenSlot<Element<'static, 'static>>),
    EndTag(TokenSlot<EndTag<'static>>),
    Text(TokenSlot<TextChunk<'static>>),
    Comment(TokenSlot<Comment<'static>>),
    Doctype(TokenSlot<Doctype<'static>>),
    DocumentEnd(TokenSlot<DocumentEnd<'static>>),
    Sink(TokenSlot<StreamingHandlerSink<'static>>, ContentType),
}

/// Runs `f` with a new slot that holds `token` until `f` returns. Needs no lock, so that it can
/// run on lol-html's fiber.
pub fn lend_slot<R>(token: Token<'_, '_, '_>, f: impl FnOnce(Slot) -> R) -> R {
    macro_rules! lend {
        ($token:expr, $variant:ident) => {{
            let slot = TokenSlot::empty();
            slot.lend($token, || f(Slot::$variant(slot.clone())))
        }};
    }
    match token {
        Token::Element(element) => lend!(element, Element),
        Token::EndTag(end_tag) => lend!(end_tag, EndTag),
        Token::Text(text) => lend!(text, Text),
        Token::Comment(comment) => lend!(comment, Comment),
        Token::Doctype(doctype) => lend!(doctype, Doctype),
        Token::DocumentEnd(end) => lend!(end, DocumentEnd),
        Token::Sink(sink, content_type) => {
            let slot = TokenSlot::empty();
            slot.lend(sink, || f(Slot::Sink(slot.clone(), content_type)))
        }
    }
}

/// Creates the handle of the token in `slot`, which reaches it while the slot holds it.
pub fn handle<'s>(
    lock: &mut jsg::Lock<'s>,
    shared: &Arc<Shared>,
    slot: Slot,
) -> jsg::Result<v8::Local<'s, v8::Value>> {
    match slot {
        Slot::Element(slot) => jsg::Rc::new(
            lock,
            ElementHandle {
                slot,
                shared: Arc::clone(shared),
            },
        )
        .to_js(lock),
        Slot::EndTag(slot) => jsg::Rc::new(lock, EndTagHandle { slot }).to_js(lock),
        Slot::Text(slot) => jsg::Rc::new(lock, TextHandle { slot }).to_js(lock),
        Slot::Comment(slot) => jsg::Rc::new(lock, CommentHandle { slot }).to_js(lock),
        Slot::Doctype(slot) => jsg::Rc::new(lock, DoctypeHandle { slot }).to_js(lock),
        Slot::DocumentEnd(slot) => jsg::Rc::new(lock, DocumentEndHandle { slot }).to_js(lock),
        Slot::Sink(slot, content_type) => {
            jsg::Rc::new(lock, SinkHandle { slot, content_type }).to_js(lock)
        }
    }
}

/// Runs `f` with a new handle of `token`, which reaches the token until `f` returns.
pub fn lend<'s>(
    lock: &mut jsg::Lock<'s>,
    shared: &Arc<Shared>,
    token: Token<'_, '_, '_>,
    f: impl FnOnce(&mut jsg::Lock<'s>, v8::Local<'s, v8::Value>) -> jsg::Result<()>,
) -> jsg::Result<()> {
    lend_slot(token, |slot| {
        let handle = handle(lock, shared, slot)?;
        f(lock, handle)
    })
}

/// Implements `jsg::Traced` for handles, which hold no GC references, and `token()`, which runs a
/// closure with the lent token.
macro_rules! token_handle {
    ($handle:ident, $token:ty) => {
        impl jsg::Traced for $handle {
            fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
        }

        impl $handle {
            fn token<R>(&self, f: impl FnOnce(&mut $token) -> R) -> jsg::Result<R> {
                self.slot
                    .with(f)
                    .ok_or_else(|| jsg::Error::new_type_error(INVALID_TOKEN))
            }
        }
    };
}

/// The handle of an element.
#[jsg_resource(custom_trace)]
pub struct ElementHandle {
    slot: TokenSlot<Element<'static, 'static>>,
    /// For the end tag handlers that `onEndTag()` registers.
    shared: Arc<Shared>,
}

token_handle!(ElementHandle, Element<'static, 'static>);

#[jsg_resource]
impl ElementHandle {
    #[jsg_method]
    pub fn tag_name(&self) -> jsg::Result<String> {
        self.token(|element| element.tag_name())
    }

    #[jsg_method]
    pub fn set_tag_name(&self, name: String) -> jsg::Result<()> {
        self.token(|element| element.set_tag_name(&name))?
            .map_err(parser_error)
    }

    #[jsg_method]
    pub fn namespace_uri(&self) -> jsg::Result<String> {
        self.token(|element| element.namespace_uri().to_owned())
    }

    #[jsg_method]
    pub fn removed(&self) -> jsg::Result<bool> {
        self.token(|element| element.removed())
    }

    #[jsg_method]
    pub fn get_attribute(&self, name: String) -> jsg::Result<Option<String>> {
        self.token(|element| element.get_attribute(&name))
    }

    #[jsg_method]
    pub fn has_attribute(&self, name: String) -> jsg::Result<bool> {
        self.token(|element| element.has_attribute(&name))
    }

    #[jsg_method]
    pub fn set_attribute(&self, name: String, value: String) -> jsg::Result<()> {
        self.token(|element| element.set_attribute(&name, &value))?
            .map_err(parser_error)
    }

    #[jsg_method]
    pub fn remove_attribute(&self, name: String) -> jsg::Result<()> {
        self.token(|element| element.remove_attribute(&name))
    }

    /// The number of attributes.
    #[jsg_method]
    pub fn attribute_count(&self) -> jsg::Result<u32> {
        self.token(|element| {
            // An element can't have more attributes than lol-html's memory limit allows bytes.
            u32::try_from(element.attributes().len()).unwrap_or(u32::MAX)
        })
    }

    /// The name and value of the attribute at `index`, or `undefined` past the last one.
    #[jsg_method]
    pub fn attribute(&self, index: u32) -> jsg::Result<Option<Vec<String>>> {
        self.token(|element| {
            let attribute = element.attributes().get(usize::try_from(index).ok()?)?;
            Some(vec![attribute.name(), attribute.value()])
        })
    }

    #[jsg_method]
    pub fn before(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|element| element.before(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn after(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|element| element.after(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn prepend(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|element| element.prepend(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn append(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|element| element.append(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn replace(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|element| element.replace(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn set_inner_content(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|element| element.set_inner_content(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn remove(&self) -> jsg::Result<()> {
        self.token(Element::remove)
    }

    #[jsg_method]
    pub fn remove_and_keep_content(&self) -> jsg::Result<()> {
        self.token(Element::remove_and_keep_content)
    }

    /// Inserts the streaming content numbered `id` before the element; see
    /// [`HandlerKind::Stream`].
    #[jsg_method]
    pub fn before_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|element| element.streaming_before(replacer(id, html)))
    }

    #[jsg_method]
    pub fn after_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|element| element.streaming_after(replacer(id, html)))
    }

    #[jsg_method]
    pub fn prepend_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|element| element.streaming_prepend(replacer(id, html)))
    }

    #[jsg_method]
    pub fn append_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|element| element.streaming_append(replacer(id, html)))
    }

    #[jsg_method]
    pub fn replace_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|element| element.streaming_replace(replacer(id, html)))
    }

    #[jsg_method]
    pub fn set_inner_content_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|element| element.streaming_set_inner_content(replacer(id, html)))
    }

    /// Replaces the element's end tag handler with the one numbered `id`, which the rewriter
    /// dispatches as a [`HandlerKind::EndTag`]. Fails for an element that can't have an end tag.
    #[jsg_method]
    pub fn on_end_tag(&self, id: u32) -> jsg::Result<()> {
        let shared = Arc::clone(&self.shared);
        self.token(|element| {
            if let Some(handlers) = element.end_tag_handlers() {
                handlers.clear();
            }
            element.on_end_tag(Box::new(move |end_tag: &mut EndTag<'_>| {
                shared.dispatch(HandlerKind::EndTag, id, Token::EndTag(end_tag))
            }))
        })?
        .map_err(parser_error)
    }
}

/// The handle of an end tag.
#[jsg_resource(custom_trace)]
pub struct EndTagHandle {
    slot: TokenSlot<EndTag<'static>>,
}

token_handle!(EndTagHandle, EndTag<'static>);

#[jsg_resource]
impl EndTagHandle {
    #[jsg_method]
    pub fn name(&self) -> jsg::Result<String> {
        self.token(|end_tag| end_tag.name())
    }

    #[jsg_method]
    pub fn set_name(&self, name: String) -> jsg::Result<()> {
        self.token(|end_tag| end_tag.set_name(name))
    }

    #[jsg_method]
    pub fn before(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|end_tag| end_tag.before(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn after(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|end_tag| end_tag.after(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn remove(&self) -> jsg::Result<()> {
        self.token(EndTag::remove)
    }

    #[jsg_method]
    pub fn before_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|end_tag| end_tag.streaming_before(replacer(id, html)))
    }

    #[jsg_method]
    pub fn after_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|end_tag| end_tag.streaming_after(replacer(id, html)))
    }
}

/// The handle of a chunk of text.
#[jsg_resource(custom_trace)]
pub struct TextHandle {
    slot: TokenSlot<TextChunk<'static>>,
}

token_handle!(TextHandle, TextChunk<'static>);

#[jsg_resource]
impl TextHandle {
    #[jsg_method]
    pub fn text(&self) -> jsg::Result<String> {
        self.token(|text| text.as_str().to_owned())
    }

    #[jsg_method]
    pub fn last_in_text_node(&self) -> jsg::Result<bool> {
        self.token(|text| text.last_in_text_node())
    }

    #[jsg_method]
    pub fn removed(&self) -> jsg::Result<bool> {
        self.token(|text| text.removed())
    }

    #[jsg_method]
    pub fn before(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|text| text.before(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn after(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|text| text.after(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn replace(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|text| text.replace(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn remove(&self) -> jsg::Result<()> {
        self.token(TextChunk::remove)
    }

    #[jsg_method]
    pub fn before_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|text| text.streaming_before(replacer(id, html)))
    }

    #[jsg_method]
    pub fn after_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|text| text.streaming_after(replacer(id, html)))
    }

    #[jsg_method]
    pub fn replace_stream(&self, id: u32, html: bool) -> jsg::Result<()> {
        self.token(|text| text.streaming_replace(replacer(id, html)))
    }
}

/// The handle of a comment.
#[jsg_resource(custom_trace)]
pub struct CommentHandle {
    slot: TokenSlot<Comment<'static>>,
}

token_handle!(CommentHandle, Comment<'static>);

#[jsg_resource]
impl CommentHandle {
    #[jsg_method]
    pub fn text(&self) -> jsg::Result<String> {
        self.token(|comment| comment.text())
    }

    #[jsg_method]
    pub fn set_text(&self, text: String) -> jsg::Result<()> {
        self.token(|comment| comment.set_text(&text))?
            .map_err(parser_error)
    }

    #[jsg_method]
    pub fn removed(&self) -> jsg::Result<bool> {
        self.token(|comment| comment.removed())
    }

    #[jsg_method]
    pub fn before(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|comment| comment.before(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn after(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|comment| comment.after(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn replace(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|comment| comment.replace(&content, content_type(html)))
    }

    #[jsg_method]
    pub fn remove(&self) -> jsg::Result<()> {
        self.token(Comment::remove)
    }
}

/// The handle of a doctype.
#[jsg_resource(custom_trace)]
pub struct DoctypeHandle {
    slot: TokenSlot<Doctype<'static>>,
}

token_handle!(DoctypeHandle, Doctype<'static>);

#[jsg_resource]
impl DoctypeHandle {
    #[jsg_method]
    pub fn name(&self) -> jsg::Result<Option<String>> {
        self.token(|doctype| doctype.name())
    }

    #[jsg_method]
    pub fn public_id(&self) -> jsg::Result<Option<String>> {
        self.token(|doctype| doctype.public_id())
    }

    #[jsg_method]
    pub fn system_id(&self) -> jsg::Result<Option<String>> {
        self.token(|doctype| doctype.system_id())
    }
}

/// The handle of the end of the document.
#[jsg_resource(custom_trace)]
pub struct DocumentEndHandle {
    slot: TokenSlot<DocumentEnd<'static>>,
}

token_handle!(DocumentEndHandle, DocumentEnd<'static>);

#[jsg_resource]
impl DocumentEndHandle {
    #[jsg_method]
    pub fn append(&self, content: String, html: bool) -> jsg::Result<()> {
        self.token(|end| end.append(&content, content_type(html)))
    }
}

/// The handle of the sink that streaming content is written to.
#[jsg_resource(custom_trace)]
pub struct SinkHandle {
    slot: TokenSlot<StreamingHandlerSink<'static>>,
    content_type: ContentType,
}

token_handle!(SinkHandle, StreamingHandlerSink<'static>);

#[jsg_resource]
impl SinkHandle {
    /// Writes the next chunk of the content, UTF-8 that may end in an incomplete character, which
    /// the next chunk completes. Fails with a parser error for invalid UTF-8.
    #[jsg_method]
    pub fn write<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        chunk: v8::Local<'s, v8::Uint8Array>,
    ) -> jsg::Result<()> {
        let chunk = chunk.as_slice(lock);
        let content_type = self.content_type;
        self.token(|sink| sink.write_utf8_chunk(chunk, content_type))?
            .map_err(parser_error)
    }
}
