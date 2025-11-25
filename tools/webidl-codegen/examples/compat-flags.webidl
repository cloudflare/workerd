// Example demonstrating compatibility flags
// Shows how runtime feature flags can control API availability

[Exposed=*,
 JsgTsDefine="type DataFormat = 'json' | 'text' | 'binary';",
 JsgTsOverride="{
  constructor();
  readonly version: string;
  basicMethod(): void;
  experimentalFeature?: string;
  experimentalMethod?(data: string): void;
  processData(input: string): string;
  processData(input: string, format: DataFormat): string;
  delete(id: string): void;
  fetch(url: string, method?: string): Promise<string>;
}"]
interface ExperimentalAPI {
  constructor();

  // Always available
  readonly attribute DOMString version;
  undefined basicMethod();

  // Property scope controls whether property is on instance or prototype
  // Instance property (own property on each object instance)
  [JsgPropertyScope=instance]
  readonly attribute DOMString instanceProperty;

  // Prototype property (shared on prototype, default behavior)
  [JsgPropertyScope=prototype]
  readonly attribute DOMString prototypeProperty;

  // Backwards compatibility: instance when flag is off, prototype when on
  // Old code incorrectly used instance properties, new code uses prototype
  [JsgCompatFlagOff=JsgPropertyOnPrototypeTemplate, JsgPropertyScope=instance]
  readonly attribute DOMString legacyProperty;

  [JsgCompatFlag=JsgPropertyOnPrototypeTemplate, JsgPropertyScope=prototype]
  readonly attribute DOMString legacyProperty;

  // Only available with WorkerdExperimental flag
  [JsgCompatFlag=WorkerdExperimental]
  readonly attribute DOMString experimentalFeature;

  [JsgCompatFlag=WorkerdExperimental]
  undefined experimentalMethod(DOMString data);

  // Only available with NodeJsCompat flag
  [JsgCompatFlag=NodeJsCompat]
  undefined nodeCompatMethod();

  // Only available with ReplicaRouting flag
  [JsgCompatFlag=ReplicaRouting]
  Promise<undefined> enableReplicas();

  [JsgCompatFlag=ReplicaRouting]
  undefined disableReplicas();

  // Method with different signatures based on compat flag
  // Default signature
  DOMString processData(DOMString input);

  // Enhanced signature when WorkerdExperimental is enabled
  // Uses custom C++ method name to differentiate
  [JsgCompatFlag=WorkerdExperimental, JsgMethodName=processDataWithFormat]
  DOMString processData(DOMString input, optional DOMString format);

  // Method with custom C++ name (useful for reserved keywords or naming conventions)
  [JsgMethodName=delete_]
  undefined delete(DOMString id);

  // Mutually exclusive signatures based on compat flag
  // Old signature when flag is OFF
  [JsgCompatFlagOff=NewApiSignature, JsgMethodName=fetchOld]
  Promise<DOMString> fetch(DOMString url);

  // New signature when flag is ON - returns union type
  [JsgCompatFlag=NewApiSignature, JsgMethodName=fetchNew]
  Promise<(DOMString or BufferSource)> fetch(DOMString url, optional DOMString method);

  // Method accepting union type
  undefined processInput((DOMString or long or boolean) value);
};

[JsgTsOverride="{
  enabled?: boolean;
  mode?: 'standard' | 'experimental';
  experimentalOption?: string;
}"]
dictionary ExperimentalOptions {
  boolean enabled = true;
  DOMString mode = "standard";

  // This field would be conditionally used based on flags in the implementation
  DOMString experimentalOption;
};
