export class Span {
  // Sets a tag on this span. If value is undefined, the tag is not set.
  setTag(key: string, value: string | number | boolean | undefined): void;
  // Closes the span
  end(): void;
}

function startSpan(name: string): Span;
