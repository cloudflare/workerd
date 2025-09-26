export class JsSpanBuilder {
  setTag(key: string, value: string | number | boolean | undefined): void;
  // Closes the span
  end(): void;
  // Returns whether this span is actively recording (not a no-op)
  getIsRecording(): boolean;
}

function startSpan(name: string): JsSpanBuilder;
