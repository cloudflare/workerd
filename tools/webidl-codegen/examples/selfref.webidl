// Example demonstrating SelfRef and internal fields in dictionaries
// SelfRef provides a reference back to the JavaScript object representation
// Fields marked with [JsgInternal] are excluded from JSG_STRUCT parameters

dictionary ConfigWithSelfRef {
  DOMString name;
  long priority = 0;

  // SelfRef stores a reference to the JavaScript object
  // Automatically excluded from JSG_STRUCT parameters
  SelfRef self;

  // Custom internal field that exists in C++ but isn't exposed to JS
  // Must be explicitly marked with [JsgInternal]
  [JsgInternal] long internalCounter;

  // Note: WebIDL dictionaries don't support methods or constructors in the spec.
  // However, we can add custom C++ code using [JsgCode] to declare
  // constructors and methods that won't be exposed to the type wrapper.
  // Only signatures are needed - implementations are provided separately.
};

// Example with custom constructor and methods
[JsgCode="
  // Custom constructor
  ConfigWithMethods(kj::String n, int p);

  // Custom methods (not exposed to JS)
  void incrementCounter();
  int getCounter() const;
"]
dictionary ConfigWithMethods {
  DOMString name;
  long priority = 0;

  [JsgInternal] long counter;
};

// Example with validate() method for input validation
// Note: validate() is a special method called during deserialization from JS
[JsgCode="
  // Validation method called when unwrapping from JavaScript
  void validate(jsg::Lock& js);
"]
dictionary ValidatedConfig {
  DOMString apiKey;
  long timeout = 30;

  // Implementation would check apiKey.size() > 0, timeout > 0, etc.
};

dictionary NestedData {
  DOMString value;
  boolean enabled = true;

  // SelfRef is always internal, no attribute needed
  SelfRef jsObject;

  // Another internal-only field
  [JsgInternal] DOMString debugInfo;
};

// Example with TypeScript root marker
[JsgTsRoot,
 JsgTsDefine="type ConfigFormat = 'json' | 'binary' | 'text';"]
dictionary ExportedConfig {
  DOMString name;
  DOMString format;
};

[Exposed=*]
interface ConfigProcessor {
  constructor();

  // Accept a dictionary with SelfRef
  undefined processConfig(ConfigWithSelfRef config);

  // Return a dictionary (SelfRef will be populated automatically)
  ConfigWithSelfRef createConfig(DOMString name, optional long priority);

  // Nested dictionary example
  undefined handleNested(NestedData data);

  // Use the custom methods dictionary
  undefined processWithMethods(ConfigWithMethods config);
};
