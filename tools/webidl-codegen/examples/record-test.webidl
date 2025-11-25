// Test record type mapping to jsg::Dict

dictionary RecordExample {
  record<DOMString, long> stringToNumber;
  record<DOMString, DOMString> headers;
  record<DOMString, sequence<DOMString>> multiMap;
};

[Exposed=*]
interface RecordAPI {
  undefined processHeaders(record<DOMString, DOMString> headers);
  record<DOMString, any> getMetadata();
};
