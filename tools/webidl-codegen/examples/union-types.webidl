// Example demonstrating union types (kj::OneOf)

[Exposed=*]
interface UnionExample {
  constructor();

  // Simple union: string or number
  (DOMString or long) getValue();

  // Union with nullable: string, number, or null
  (DOMString or long)? getNullableValue();

  // Union parameter
  undefined setValue((DOMString or long) value);

  // Union with BufferSource
  undefined processData((DOMString or BufferSource) data);

  // Complex union: multiple types
  (DOMString or long or boolean) getMultiValue();

  // Union with optional
  undefined setOptionalValue(optional (DOMString or long) value);

  // Union return with Promise
  Promise<(DOMString or BufferSource)> fetchData();
};

dictionary UnionOptions {
  // Union in dictionary
  (DOMString or long) identifier;

  // Optional union
  (boolean or DOMString) mode = true;

  // Nullable union
  (DOMString or long)? optionalValue;
};

// Callback with union
callback UnionCallback = undefined ((DOMString or long) result);
