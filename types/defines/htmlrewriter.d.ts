// HTMLRewriter and the tokens its handlers receive, for both implementations:
// the C++ one and the TypeScript one (src/per_isolate/htmlrewriter/, under
// the TS_HTML_REWRITER autogate), which has no RTTI to generate them from.
// The token types keep the names the C++ classes gave them.
interface ContentOptions {
  html?: boolean;
}
declare class HTMLRewriter {
  constructor();
  on(
    selector: string,
    handlers: HTMLRewriterElementContentHandlers,
  ): HTMLRewriter;
  onDocument(handlers: HTMLRewriterDocumentContentHandlers): HTMLRewriter;
  transform(response: Response): Response;
}
interface HTMLRewriterElementContentHandlers {
  element?(element: Element): void | Promise<void>;
  comments?(comment: Comment): void | Promise<void>;
  text?(element: Text): void | Promise<void>;
}
interface HTMLRewriterDocumentContentHandlers {
  doctype?(doctype: Doctype): void | Promise<void>;
  comments?(comment: Comment): void | Promise<void>;
  text?(text: Text): void | Promise<void>;
  end?(end: DocumentEnd): void | Promise<void>;
}
interface Doctype {
  readonly name: string | null;
  readonly publicId: string | null;
  readonly systemId: string | null;
}
interface Element {
  tagName: string;
  readonly attributes: IterableIterator<string[]>;
  readonly removed: boolean;
  readonly namespaceURI: string;
  getAttribute(name: string): string | null;
  hasAttribute(name: string): boolean;
  setAttribute(name: string, value: string): Element;
  removeAttribute(name: string): Element;
  before(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Element;
  after(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Element;
  prepend(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Element;
  append(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Element;
  replace(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Element;
  remove(): Element;
  removeAndKeepContent(): Element;
  setInnerContent(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Element;
  onEndTag(handler: (tag: EndTag) => void | Promise<void>): void;
}
interface EndTag {
  name: string;
  before(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): EndTag;
  after(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): EndTag;
  remove(): EndTag;
}
interface Comment {
  text: string;
  readonly removed: boolean;
  before(content: string, options?: ContentOptions): Comment;
  after(content: string, options?: ContentOptions): Comment;
  replace(content: string, options?: ContentOptions): Comment;
  remove(): Comment;
}
interface Text {
  readonly text: string;
  readonly lastInTextNode: boolean;
  readonly removed: boolean;
  before(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Text;
  after(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Text;
  replace(
    content: string | ReadableStream | Response,
    options?: ContentOptions,
  ): Text;
  remove(): Text;
}
interface DocumentEnd {
  append(content: string, options?: ContentOptions): DocumentEnd;
}
