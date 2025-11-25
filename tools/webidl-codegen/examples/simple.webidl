// Simple example for testing implementation generation

[Exposed=*]
interface Calculator {
  constructor();

  long add(long a, long b);
  long subtract(long a, long b);

  readonly attribute DOMString version;
};

dictionary Point {
  long x;
  long y;
};

[JsgCode="
  double area() const;
  void validate(jsg::Lock& js);
"]
dictionary Circle {
  Point center;
  double radius;
};
