export class JsSpanBuilder {
  setTag(key: string, value: string | number | boolean | undefined): void;
  // Closes the span
  end(): void;
}

function startSpan(name: string): JsSpanBuilder;
function startSpanWithCallback<T>(
  name: string,
  callback: (span: JsSpanBuilder) => T
): T;
