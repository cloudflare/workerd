// Test advanced dictionary inheritance scenarios

dictionary Level1 {
  DOMString field1;
  [JsgInternal] SelfRef selfRef;
};

dictionary Level2 : Level1 {
  long field2;
  boolean flag = true;
};

dictionary Level3 : Level2 {
  [JsgInternal] Unimplemented internal;
  sequence<DOMString> items;
};
