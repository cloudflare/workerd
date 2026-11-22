interface StructuredSerializeOptions {
  transfer?: any[];
}
interface ServiceWorkerGlobalScope {
  structuredClone<T>(value: T, options?: StructuredSerializeOptions): T;
}
/* [MDN Reference](https://developer.mozilla.org/docs/Web/API/Window/structuredClone) */
declare function structuredClone<T>(
  value: T,
  options?: StructuredSerializeOptions
): T;
