// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

// HTMLRewriter's declarations, from types/defines/htmlrewriter.d.ts, and the global scope's
// HTMLRewriter member, from JSG_NESTED_TYPE(HTMLRewriter) in src/workerd/api/global-scope.h.

function expectType<T>(_value: T) {}

declare const response: Response;
declare const stream: ReadableStream;

// The global, and the global scope's member, which JSG_NESTED_TYPE contributes.
expectType<typeof HTMLRewriter>(self.HTMLRewriter);

const rewriter = new HTMLRewriter()
  .on('p', {
    element(element) {
      expectType<Element>(element);
      expectType<string>(element.tagName);
      element.tagName = 'div';
      expectType<IterableIterator<string[]>>(element.attributes);
      expectType<boolean>(element.removed);
      expectType<string>(element.namespaceURI);
      // @ts-expect-error removed is read-only.
      element.removed = true;
      expectType<string | null>(element.getAttribute('a'));
      expectType<boolean>(element.hasAttribute('a'));
      expectType<Element>(element.setAttribute('a', 'b'));
      expectType<Element>(element.removeAttribute('a'));
      expectType<Element>(element.before('x'));
      expectType<Element>(element.after(stream, { html: true }));
      expectType<Element>(element.prepend(response));
      expectType<Element>(element.append('x', { html: false }));
      expectType<Element>(element.replace('x'));
      expectType<Element>(element.setInnerContent('x'));
      expectType<Element>(element.remove());
      expectType<Element>(element.removeAndKeepContent());
      element.onEndTag((tag) => {
        expectType<EndTag>(tag);
        expectType<string>(tag.name);
        tag.name = 'div';
        expectType<EndTag>(tag.before('x', { html: true }));
        expectType<EndTag>(tag.after(stream));
        expectType<EndTag>(tag.remove());
      });
      element.onEndTag(async () => {});
    },
    comments(comment) {
      expectType<Comment>(comment);
      expectType<string>(comment.text);
      comment.text = 'c';
      expectType<boolean>(comment.removed);
      expectType<Comment>(comment.before('x', { html: true }));
      expectType<Comment>(comment.after('x'));
      expectType<Comment>(comment.replace('x'));
      expectType<Comment>(comment.remove());
      // @ts-expect-error a comment's content is a string.
      comment.replace(stream);
    },
    async text(text) {
      expectType<Text>(text);
      expectType<string>(text.text);
      expectType<boolean>(text.lastInTextNode);
      expectType<boolean>(text.removed);
      // @ts-expect-error a text chunk's text is read-only.
      text.text = 'x';
      expectType<Text>(text.before(response));
      expectType<Text>(text.after('x', { html: true }));
      expectType<Text>(text.replace(stream));
      expectType<Text>(text.remove());
    },
  })
  .onDocument({
    doctype(doctype) {
      expectType<Doctype>(doctype);
      expectType<string | null>(doctype.name);
      expectType<string | null>(doctype.publicId);
      expectType<string | null>(doctype.systemId);
    },
    comments(comment) {
      expectType<Comment>(comment);
    },
    text(text) {
      expectType<Text>(text);
    },
    end(end) {
      expectType<DocumentEnd>(end);
      expectType<DocumentEnd>(end.append('x', { html: true }));
      // @ts-expect-error the document end's content is a string.
      end.append(stream);
    },
  });
expectType<HTMLRewriter>(rewriter);
expectType<Response>(rewriter.transform(response));

// The handler dictionaries and the content options are named types.
const elementHandlers: HTMLRewriterElementContentHandlers = {};
const documentHandlers: HTMLRewriterDocumentContentHandlers = {};
const options: ContentOptions = { html: true };
new HTMLRewriter().on('*', elementHandlers).onDocument(documentHandlers);
expectType<boolean | undefined>(options.html);

// @ts-expect-error on() takes a selector.
new HTMLRewriter().on({});
