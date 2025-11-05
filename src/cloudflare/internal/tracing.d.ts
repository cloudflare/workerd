export class Span {
  // Sets an attribute on this span. If value is undefined, the attribute is not set.
  setAttribute(key: string, value: string | number | boolean | undefined): void;
  // Closes the span
  end(): void;
}

function startSpan(name: string): Span;
