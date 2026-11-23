// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The rewriter handle: a `lol_html::HtmlRewriter` that calls one JavaScript function, the
//! dispatcher, for every handler it runs, on the caller's stack or, for handlers that return
//! promises, on a fiber (see [`crate::fiber`]).

use std::borrow::Cow;
use std::cell::RefCell;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::sync::TryLockError;
use std::sync::Weak;

use jsg::ToJS;
use jsg::ToJsTypedArray;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;
use lol_html::AsciiCompatibleEncoding;
use lol_html::HandlerResult;
use lol_html::MemorySettings;
use lol_html::OutputSink;
use lol_html::html_content::Comment;
use lol_html::html_content::ContentType;
use lol_html::html_content::Doctype;
use lol_html::html_content::DocumentEnd;
use lol_html::html_content::EndTag;
use lol_html::html_content::StreamingHandler;
use lol_html::html_content::StreamingHandlerSink;
use lol_html::html_content::TextChunk;
use lol_html::send::DocumentContentHandlers;
pub use lol_html::send::Element;
use lol_html::send::ElementContentHandlers;
use lol_html::send::Settings;

use crate::fiber;
use crate::parser_error;
use crate::slot::DispatcherSlot;
use crate::tokens;

/// lol-html's working memory: the buffer it starts with, and the most it may use.
const PREALLOCATED_PARSING_BUFFER_SIZE: usize = 1024;
const MAX_ALLOWED_MEMORY_USAGE: usize = 3 * 1024 * 1024;

/// Which handler the dispatcher is called for. JavaScript gets the discriminant.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum HandlerKind {
    /// The `element` handler of the element handlers numbered `id`.
    Element = 0,
    /// The `comments` handler of the element handlers numbered `id`.
    Comments = 1,
    /// The `text` handler of the element handlers numbered `id`.
    Text = 2,
    /// The `doctype` handler of the document handlers numbered `id`.
    Doctype = 3,
    /// The `comments` handler of the document handlers numbered `id`.
    DocumentComments = 4,
    /// The `text` handler of the document handlers numbered `id`.
    DocumentText = 5,
    /// The `end` handler of the document handlers numbered `id`.
    DocumentEnd = 6,
    /// The end tag handler that `onEndTag(id)` registered.
    EndTag = 7,
    /// The streaming content numbered `id`, which a `*Stream(id, html)` method of a token
    /// inserted: the dispatcher writes it to the token, a sink.
    Stream = 8,
}

/// A lol-html token that a handler is called with.
pub enum Token<'a, 'r, 'i> {
    Element(&'a mut Element<'r, 'i>),
    EndTag(&'a mut EndTag<'i>),
    Text(&'a mut TextChunk<'i>),
    Comment(&'a mut Comment<'i>),
    Doctype(&'a mut Doctype<'i>),
    DocumentEnd(&'a mut DocumentEnd<'i>),
    /// Where streaming content is written, as HTML or as text.
    Sink(&'a mut StreamingHandlerSink<'i>, ContentType),
}

/// A handler that lol-html runs.
pub struct Call<'a, 'r, 'i> {
    pub kind: HandlerKind,
    pub id: u32,
    pub token: Token<'a, 'r, 'i>,
}

/// What a dispatcher returns when the handler failed, having kept why. lol-html stops on it.
#[derive(Debug)]
pub struct Stopped;

impl std::fmt::Display for Stopped {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("The rewriter has been stopped.")
    }
}

impl std::error::Error for Stopped {}

/// Runs the handlers of the `write()` or `end()` call that is running.
pub trait Dispatcher {
    fn dispatch(&mut self, call: Call<'_, '_, '_>) -> Result<(), Stopped>;
}

/// The lol-html rewriter of a [`HtmlRewriterHandle`].
pub type Rewriter = lol_html::send::HtmlRewriter<'static, Output>;

/// Locks `mutex`. Nothing panics while holding a rewriter's mutexes but lol-html and handlers,
/// whose panic fails the rewriter (see `HtmlRewriterHandle::borrow_state()`), so a poisoned
/// mutex holds nothing that is used again.
pub fn lock_mutex<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// What a rewriter's handlers and output sink share with it.
pub struct Shared {
    pub(crate) dispatcher: DispatcherSlot,
    output: Mutex<OutputBuffer>,
}

/// lol-html's output since it was last taken. It holds the output of a whole chunk unless the
/// dispatcher takes it, and streamed replacement content as it is written, so its capacity is
/// attributed to the isolate, as C++ `HTMLRewriter` attributes its output buffer. lol-html's
/// own memory, which `MAX_ALLOWED_MEMORY_USAGE` bounds, is not, as in C++.
struct OutputBuffer {
    bytes: Vec<u8>,
    /// The capacity of `bytes`, set only when it changes, so that most chunks don't cross the
    /// FFI boundary.
    memory: jsg::ExternalMemoryAdjustment,
}

impl OutputBuffer {
    fn extend(&mut self, chunk: &[u8]) {
        let capacity = self.bytes.capacity();
        self.bytes.extend_from_slice(chunk);
        if self.bytes.capacity() != capacity {
            self.memory.set(self.bytes.capacity());
        }
    }

    fn take(&mut self) -> Vec<u8> {
        if self.bytes.capacity() != 0 {
            self.memory.set(0);
        }
        std::mem::take(&mut self.bytes)
    }
}

thread_local! {
    /// The rewriter whose lol-html code runs on this thread: the one a [`Replacer`] belongs to,
    /// which the token handles that create replacers don't hold. Fibers interleave, and a
    /// handler can run another rewriter, so a rewriter sets it whenever lol-html's code resumes:
    /// when a call starts, and when a dispatch returns.
    static RUNNING: RefCell<Weak<Shared>> = const { RefCell::new(Weak::new()) };
}

impl Shared {
    pub(crate) fn take_output(&self) -> Vec<u8> {
        lock_mutex(&self.output).take()
    }

    /// Makes this the rewriter whose lol-html code runs on this thread.
    pub(crate) fn resume(self: &Arc<Self>) {
        RUNNING.with(|running| *running.borrow_mut() = Arc::downgrade(self));
    }

    /// Runs a handler through the dispatcher of the running call.
    pub(crate) fn dispatch(
        self: &Arc<Self>,
        kind: HandlerKind,
        id: u32,
        token: Token<'_, '_, '_>,
    ) -> HandlerResult {
        let call = Call { kind, id, token };
        let result = self.dispatcher.with(|dispatcher| dispatcher.dispatch(call));
        self.resume();
        match result {
            Some(Ok(())) => Ok(()),
            // lol-html runs handlers only within `write()` and `end()`, which lend a dispatcher.
            Some(Err(Stopped)) | None => Err(Box::new(Stopped)),
        }
    }
}

/// Streaming content that a token's `*Stream(id, html)` method inserted: lol-html calls it when it
/// writes the content, and it dispatches a [`HandlerKind::Stream`] to the running rewriter.
///
/// lol-html drops content it never writes, as when a later handler removes the element, without
/// a dispatch, so the dispatcher's caller keeps its streams until the rewriter ends.
pub struct Replacer {
    pub id: u32,
    pub content_type: ContentType,
}

impl StreamingHandler for Replacer {
    fn write_all(self: Box<Self>, sink: &mut StreamingHandlerSink<'_>) -> HandlerResult {
        // lol-html writes streaming content only within the call of the rewriter it belongs to.
        let shared = RUNNING
            .with(|running| running.borrow().upgrade())
            .ok_or_else(|| Box::new(Stopped))?;
        shared.dispatch(
            HandlerKind::Stream,
            self.id,
            Token::Sink(sink, self.content_type),
        )
    }
}

/// Collects lol-html's output for the dispatcher and for `write()` and `end()` to return.
pub struct Output(Arc<Shared>);

impl OutputSink for Output {
    fn handle_chunk(&mut self, chunk: &[u8]) {
        lock_mutex(&self.0.output).extend(chunk);
    }
}

/// A selector that lol-html parsed.
#[jsg_resource(custom_trace)]
pub struct HtmlSelector {
    selector: lol_html::Selector,
}

impl jsg::Traced for HtmlSelector {
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

#[jsg_resource]
impl HtmlSelector {}

/// Parses `selector`, or fails with `TypeError: Parser error: ...`, as C++ `HTMLRewriter::on()`
/// does.
pub fn parse_selector(
    lock: &mut jsg::Lock<'_>,
    selector: &str,
) -> jsg::Result<jsg::Rc<HtmlSelector>> {
    let selector = selector.parse().map_err(parser_error)?;
    Ok(jsg::Rc::new(lock, HtmlSelector { selector }))
}

/// The handlers of one `HTMLRewriter.on()` call: its selector, and which handlers it has.
#[jsg_struct]
pub struct ElementHandlers {
    pub selector: jsg::Rc<HtmlSelector>,
    pub element: bool,
    pub comments: bool,
    pub text: bool,
}

/// The handlers of one `HTMLRewriter.onDocument()` call: which handlers it has.
#[jsg_struct]
#[expect(
    clippy::struct_excessive_bools,
    reason = "mirrors the JavaScript object of handler flags"
)]
pub struct DocumentHandlers {
    pub doctype: bool,
    pub comments: bool,
    pub text: bool,
    pub end: bool,
}

pub enum State {
    Open(Box<Rewriter>),
    /// The fiber of a `writeAsync()` or `endAsync()` call has the rewriter.
    Running,
    Ended,
    Failed,
}

/// A rewriter, to which JavaScript writes the document in chunks.
///
/// Each call returns the output. lol-html calls the dispatcher, a JavaScript function, for every
/// handler: `dispatcher(kind, id, token)`, where `kind` is a [`HandlerKind`], `id` numbers the
/// handlers, and `token` is the token's handle. The output before the handler stays in the
/// rewriter, where the call returns it, unless the dispatcher takes it with `takeOutput()`, as
/// it does for a handler that waits.
///
/// The dispatcher runs in the async context frame the rewriter was created in. `write()` and
/// `end()` run it within the call, and fail if it returns a promise; the token is valid until it
/// returns. `writeAsync()` and `endAsync()`, which need a request, return promises; the token is
/// valid until the promise the dispatcher returns, if any, settles.
#[jsg_resource(custom_trace)]
pub struct HtmlRewriterHandle {
    dispatcher: v8::TracedGlobal<v8::Function>,
    frame: jsg::TracedAsyncContextFrame,
    shared: Arc<Shared>,
    /// Locked for the whole of `write()` and `end()`, so that a handler can't run them, or
    /// `abort()`, again. Shared with the fiber of a `writeAsync()` or `endAsync()` call, which
    /// leaves the rewriter in it.
    state: Arc<Mutex<State>>,
}

impl jsg::Traced for HtmlRewriterHandle {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        jsg::Traced::trace(&self.dispatcher, visitor);
        jsg::Traced::trace(&self.frame, visitor);
    }
}

/// Creates a rewriter for the handlers, the encoding labelled `encoding`, and the dispatcher.
///
/// Fails with `TypeError: Parser error: ...` for an encoding lol-html doesn't
/// support, as C++ `HTMLRewriter::transform()` does.
pub fn new_rewriter<'s>(
    lock: &mut jsg::Lock<'s>,
    element_handlers: Vec<ElementHandlers>,
    document_handlers: Vec<DocumentHandlers>,
    encoding: &str,
    dispatcher: v8::Local<'s, v8::Function>,
) -> jsg::Result<jsg::Rc<HtmlRewriterHandle>> {
    let encoding = encoding_rs::Encoding::for_label_no_replacement(encoding.as_bytes())
        .ok_or_else(|| parser_error("Unknown character encoding has been provided."))?;
    let encoding = AsciiCompatibleEncoding::new(encoding)
        .ok_or_else(|| parser_error("Expected ASCII-compatible encoding."))?;
    let shared = Arc::new(Shared {
        dispatcher: DispatcherSlot::empty(),
        output: Mutex::new(OutputBuffer {
            bytes: Vec::new(),
            memory: lock.external_memory_adjustment(0),
        }),
    });

    let mut settings = Settings::new_send()
        .with_encoding(encoding)
        .with_memory_settings(
            MemorySettings::new()
                .with_preallocated_parsing_buffer_size(PREALLOCATED_PARSING_BUFFER_SIZE)
                .with_max_allowed_memory_usage(MAX_ALLOWED_MEMORY_USAGE),
        )
        .with_strict(true)
        .with_enable_esi_tags(lock.feature_flags().get_esi_include_is_void_tag());
    for (id, handlers) in (0u32..).zip(element_handlers) {
        let mut content = ElementContentHandlers::default();
        if handlers.element {
            let shared = Arc::clone(&shared);
            content = content.element(move |element: &mut Element<'_, '_>| {
                shared.dispatch(HandlerKind::Element, id, Token::Element(element))
            });
        }
        if handlers.comments {
            let shared = Arc::clone(&shared);
            content = content.comments(move |comment: &mut Comment<'_>| {
                shared.dispatch(HandlerKind::Comments, id, Token::Comment(comment))
            });
        }
        if handlers.text {
            let shared = Arc::clone(&shared);
            content = content.text(move |text: &mut TextChunk<'_>| {
                shared.dispatch(HandlerKind::Text, id, Token::Text(text))
            });
        }
        let selector = Cow::Owned(handlers.selector.get(lock).selector.clone());
        settings = settings.append_element_content_handler((selector, content));
    }
    for (id, handlers) in (0u32..).zip(document_handlers) {
        let mut content = DocumentContentHandlers::default();
        if handlers.doctype {
            let shared = Arc::clone(&shared);
            content = content.doctype(move |doctype: &mut Doctype<'_>| {
                shared.dispatch(HandlerKind::Doctype, id, Token::Doctype(doctype))
            });
        }
        if handlers.comments {
            let shared = Arc::clone(&shared);
            content = content.comments(move |comment: &mut Comment<'_>| {
                shared.dispatch(HandlerKind::DocumentComments, id, Token::Comment(comment))
            });
        }
        if handlers.text {
            let shared = Arc::clone(&shared);
            content = content.text(move |text: &mut TextChunk<'_>| {
                shared.dispatch(HandlerKind::DocumentText, id, Token::Text(text))
            });
        }
        if handlers.end {
            let shared = Arc::clone(&shared);
            content = content.end(move |end: &mut DocumentEnd<'_>| {
                shared.dispatch(HandlerKind::DocumentEnd, id, Token::DocumentEnd(end))
            });
        }
        settings = settings.append_document_content_handler(content);
    }

    let rewriter = Rewriter::new(settings, Output(Arc::clone(&shared)));
    let dispatcher = dispatcher.to_traced_global(lock);
    let frame = jsg::TracedAsyncContextFrame::current(lock);
    Ok(jsg::Rc::new(
        lock,
        HtmlRewriterHandle {
            dispatcher,
            frame,
            shared,
            state: Arc::new(Mutex::new(State::Open(Box::new(rewriter)))),
        },
    ))
}

#[jsg_resource]
impl HtmlRewriterHandle {
    /// Rewrites the next chunk of the document, and returns the output that remains after the
    /// last handler.
    #[jsg_method]
    pub fn write<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        chunk: v8::Local<'s, v8::Uint8Array>,
    ) -> jsg::Result<v8::Local<'s, v8::Uint8Array>> {
        // Copied, since the handlers can detach the chunk's buffer.
        let chunk = chunk.as_slice(lock).to_vec();
        let mut state = self.borrow_state()?;
        let State::Open(rewriter) = &mut *state else {
            return Err(Self::closed_error(&state));
        };
        let result = self.run(lock, || rewriter.write(&chunk));
        if result.is_err() {
            *state = State::Failed;
        }
        drop(state);
        result?;
        Ok(self.shared.take_output().to_js_typed_array(lock))
    }

    /// Ends the document, and returns the output that remains after the last handler.
    #[jsg_method]
    pub fn end<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Uint8Array>> {
        let mut state = self.borrow_state()?;
        if !matches!(*state, State::Open(_)) {
            return Err(Self::closed_error(&state));
        }
        let State::Open(rewriter) = std::mem::replace(&mut *state, State::Ended) else {
            unreachable!("the state was checked to be open");
        };
        let result = self.run(lock, || rewriter.end());
        if result.is_err() {
            *state = State::Failed;
        }
        drop(state);
        result?;
        Ok(self.shared.take_output().to_js_typed_array(lock))
    }

    /// Rewrites the next chunk of the document on a fiber, and returns a promise for the output
    /// that remains after the last handler.
    #[jsg_method]
    pub fn write_async<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        chunk: v8::Local<'s, v8::Uint8Array>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let chunk = chunk.as_slice(lock).to_vec();
        self.start(lock, Some(chunk))
    }

    /// Ends the document on a fiber, and returns a promise for the output that remains after the
    /// last handler.
    #[jsg_method]
    pub fn end_async<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        self.start(lock, None)
    }

    /// Returns the output lol-html produced since the previous call, or since this was last
    /// called, which a call then no longer returns. A dispatcher calls it during a handler for
    /// the output before the handler; the token's own output follows once the handler is done.
    #[jsg_method]
    pub fn take_output<'s>(&self, lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Uint8Array> {
        self.shared.take_output().to_js_typed_array(lock)
    }

    /// Discards the rewriter: later calls fail.
    #[jsg_method]
    pub fn abort(&self) -> jsg::Result<()> {
        *self.borrow_state()? = State::Failed;
        Ok(())
    }
}

impl HtmlRewriterHandle {
    /// Locks the state, or fails if a handler of the running `write()` or `end()` call asks, as
    /// only the thread with the isolate lock locks it.
    ///
    /// A poisoned state means lol-html or a handler panicked within `write()` or `end()`, which
    /// leaves the rewriter part-way through a chunk, so the rewriter fails.
    fn borrow_state(&self) -> jsg::Result<MutexGuard<'_, State>> {
        match self.state.try_lock() {
            Ok(state) => Ok(state),
            Err(TryLockError::Poisoned(state)) => {
                let mut state = state.into_inner();
                *state = State::Failed;
                self.state.clear_poison();
                Ok(state)
            }
            Err(TryLockError::WouldBlock) => Err(jsg::Error::new_type_error(
                "An HTMLRewriter handler can't use the rewriter that is running it.",
            )),
        }
    }

    fn closed_error(state: &State) -> jsg::Error {
        jsg::Error::new_type_error(match state {
            State::Ended => "This HTMLRewriter has already ended.",
            State::Running => "An HTMLRewriter handler can't use the rewriter that is running it.",
            State::Open(_) | State::Failed => "This HTMLRewriter has failed.",
        })
    }

    /// Starts a `writeAsync()` or `endAsync()` call.
    fn start<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        chunk: Option<Vec<u8>>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = io_context::IoContext::current(lock)?;
        let mut state = self.borrow_state()?;
        if !matches!(*state, State::Open(_)) {
            return Err(Self::closed_error(&state));
        }
        let State::Open(rewriter) = std::mem::replace(&mut *state, State::Running) else {
            unreachable!("the state was checked to be open");
        };
        drop(state);
        let function = self.dispatcher.as_local(lock);
        let frame = self.frame.to_async_context_frame(lock);
        let call = fiber::AsyncCall::new(lock, function, frame, &self.shared);
        fiber::start(lock, &context, call, &self.state, rewriter, chunk)
    }

    /// Runs `f`, which calls lol-html, with a dispatcher that calls the JavaScript dispatcher
    /// with `lock`.
    fn run(
        &self,
        lock: &mut jsg::Lock<'_>,
        f: impl FnOnce() -> Result<(), lol_html::errors::RewritingError>,
    ) -> jsg::Result<()> {
        // Handlers run in the async context frame of `transform()`, as with C++ `HTMLRewriter`.
        self.shared.resume();
        self.frame.enter(lock, |lock| {
            let function = self.dispatcher.as_local(lock);
            let mut dispatcher = SyncDispatcher {
                lock,
                function,
                shared: &self.shared,
                error: None,
            };
            let result = self.shared.dispatcher.lend(&mut dispatcher, f);
            match (result, dispatcher.error) {
                (Ok(()), _) => Ok(()),
                (Err(_), Some(error)) => Err(error),
                (Err(error), None) => Err(parser_error(error)),
            }
        })
    }
}

/// Calls the JavaScript dispatcher on the stack of the `write()` or `end()` call.
struct SyncDispatcher<'a, 's> {
    lock: &'a mut jsg::Lock<'s>,
    function: v8::Local<'s, v8::Function>,
    shared: &'a Arc<Shared>,
    /// Why the dispatcher stopped lol-html.
    error: Option<jsg::Error>,
}

impl Dispatcher for SyncDispatcher<'_, '_> {
    fn dispatch(&mut self, call: Call<'_, '_, '_>) -> Result<(), Stopped> {
        let lock = &mut *self.lock;
        let function = self.function;
        let result = tokens::lend(lock, self.shared, call.token, |lock, token| {
            let kind = (call.kind as u8).to_js(lock)?;
            let id = call.id.to_js(lock)?;
            let result = function.call(lock, None, &[kind, id, token])?;
            if result.is_promise() {
                return Err(jsg::Error::new_type_error(
                    "HTMLRewriter handlers that return a promise are not supported yet.",
                ));
            }
            Ok(())
        });
        result.map_err(|error| {
            self.error = Some(error);
            Stopped
        })
    }
}

#[cfg(test)]
#[path = "rewriter-test.rs"]
mod tests;
