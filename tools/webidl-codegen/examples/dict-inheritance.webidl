// Test dictionary inheritance flattening

dictionary BaseOptions {
  DOMString mode = "read";
  boolean excludeAll = false;
};

dictionary FilePickerOptions : BaseOptions {
  DOMString id;
  boolean multiple = false;
};

dictionary OpenFilePickerOptions : FilePickerOptions {
  boolean allowMultipleFiles = true;
  DOMString startDirectory;
};
