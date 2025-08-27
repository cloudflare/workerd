export class JsSpanBuilder {
  end(): void;
  setTag(key: string, value: string | number | boolean): void;
}

export function startSpan(name: string): JsSpanBuilder;

export default {
  startSpan,
};
