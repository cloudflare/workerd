#include "tests/ffi/tests.h"

#include "tests/ffi/lib.rs.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <numeric>
#ifdef __cpp_lib_span
#include <span>
#endif // __cpp_lib_span
#include <kj/debug.h>
#include <stdexcept>
#include <string>
#include <tuple>

extern "C" void cxx_test_suite_set_correct() noexcept;
extern "C" tests::R *cxx_test_suite_get_box() noexcept;
extern "C" bool cxx_test_suite_r_is_correct(const tests::R *) noexcept;

namespace tests {

static constexpr char SLICE_DATA[] = "2020";
static constexpr char16_t SLICE_DATA16[] = u"2020";

static_assert(sizeof(SharedWithKjOwn) == sizeof(kj::Own<C>));
static_assert(alignof(SharedWithKjOwn) == alignof(kj::Own<C>));
static_assert(sizeof(SharedWithMultipleKjOwns) == 2 * sizeof(kj::Own<C>));
static_assert(alignof(SharedWithMultipleKjOwns) == alignof(kj::Own<C>));
static_assert(sizeof(SharedWithKjRc) == sizeof(kj::Rc<RcC>));
static_assert(alignof(SharedWithKjRc) == alignof(kj::Rc<RcC>));
static_assert(sizeof(SharedWithMultipleKjRcs) == 2 * sizeof(kj::Rc<RcC>));
static_assert(alignof(SharedWithMultipleKjRcs) == alignof(kj::Rc<RcC>));
static_assert(sizeof(SharedWithKjArc) == sizeof(kj::Arc<ArcC>));
static_assert(alignof(SharedWithKjArc) == alignof(kj::Arc<ArcC>));
static_assert(sizeof(SharedWithMultipleKjArcs) == 2 * sizeof(kj::Arc<ArcC>));
static_assert(alignof(SharedWithMultipleKjArcs) == alignof(kj::Arc<ArcC>));

C::C(size_t n) : n(n) {}

size_t C::get() const { return this->n; }

size_t C::get2() const { return this->n; }

const size_t &C::getRef() const { return this->n; }

size_t &C::getMut() { return this->n; }

size_t C::set(size_t n) {
  this->n = n;
  return this->n;
}

size_t C::set_succeed(size_t n) { return this->set(n); }

size_t C::get_fail() { throw std::runtime_error("unimplemented"); }

size_t C::get_fail_infallible() {
  kj::throwFatalException(KJ_EXCEPTION(FAILED, "infallible method failure"));
}

RcC::RcC(size_t n) : n(n) {}

size_t RcC::get() const { return this->n; }

void RcC::set(size_t n) { this->n = n; }

NonRefcountedRcC::NonRefcountedRcC(size_t n) : n(n) {}

size_t NonRefcountedRcC::get() const { return this->n; }

ArcC::ArcC(size_t n) : n(n) {}

size_t ArcC::get() const { return this->n.load(); }

void ArcC::set(size_t n) const { this->n.store(n); }

NonAtomicArcC::NonAtomicArcC(size_t n) : n(n) {}

size_t NonAtomicArcC::get() const { return this->n; }

size_t Shared::c_method_on_shared() const noexcept { return 2021; }

const size_t &Shared::c_method_ref_on_shared() const noexcept {
  return this->z;
}

size_t &Shared::c_method_mut_on_shared() noexcept { return this->z; }

void Array::c_set_array(int32_t val) noexcept {
  this->a = {val, val, val, val};
}

const std::vector<uint8_t> &C::get_v() const { return this->v; }

std::vector<uint8_t> &C::get_v() { return this->v; }

size_t c_return_primitive() { return 2020; }

Shared c_return_shared() { return Shared{2020}; }

::A::AShared c_return_ns_shared() { return ::A::AShared{2020}; }

::A::B::ABShared c_return_nested_ns_shared() { return ::A::B::ABShared{2020}; }

rust::Box<R> c_return_box() {
  Shared shared{0};
  rust::Box<Shared> box{shared}; // explicit constructor from const T&
  rust::Box<Shared> other{std::move(shared)}; // explicit constructor from T&&
  box = std::move(other);                     // move assignment
  rust::Box<Shared> box2(*box);               // copy from another Box
  rust::Box<Shared> other2(std::move(other)); // move constructor
  rust::Box<Shared>::in_place(shared.z);      // placement static factory
  rust::Box<Shared>::in_place<size_t>(0);
  return rust::Box<R>::from_raw(cxx_test_suite_get_box());
}

rust::Box<R> c_return_box_from_aliased_rust_type() { return r_return_box(); }

std::unique_ptr<C> c_return_unique_ptr() {
  return std::unique_ptr<C>(new C{2020});
}

std::shared_ptr<C> c_return_shared_ptr() {
  return std::shared_ptr<C>(new C{2020});
}

std::unique_ptr<::H::H> c_return_ns_unique_ptr() {
  return std::unique_ptr<::H::H>(new ::H::H{"hello"});
}

const size_t &c_return_ref(const Shared &shared) { return shared.z; }

const size_t &c_return_ns_ref(const ::A::AShared &shared) {
  return shared.type;
}

const size_t &c_return_nested_ns_ref(const ::A::B::ABShared &shared) {
  return shared.z;
}

size_t &c_return_mut(Shared &shared) { return shared.z; }

rust::Str c_return_str(const Shared &shared) {
  (void)shared;
  return "2020";
}

rust::Slice<const char> c_return_slice_char(const Shared &shared) {
  (void)shared;
  return rust::Slice<const char>(SLICE_DATA, sizeof(SLICE_DATA));
}

rust::Slice<const char16_t> c_return_slice_char16(const Shared &shared) {
  (void)shared;
  // Excludes the trailing NUL, unlike c_return_slice_char, so the Rust side can
  // compare against "2020" directly.
  return rust::Slice<const char16_t>(SLICE_DATA16, 4);
}

rust::Vec<char16_t> c_return_rust_vec_char16() {
  rust::Vec<char16_t> vec;
  for (char16_t c : rust::Slice<const char16_t>(SLICE_DATA16, 4)) {
    vec.push_back(c);
  }
  return vec;
}

rust::Slice<uint8_t> c_return_mutsliceu8(rust::Slice<uint8_t> slice) {
  return slice;
}

rust::String c_return_rust_string() { return "2020"; }

rust::String c_return_rust_string_lossy() {
  return rust::String::lossy("Hello \xf0\x90\x80World");
}

std::unique_ptr<std::string> c_return_unique_ptr_string() {
  return std::unique_ptr<std::string>(new std::string("2020"));
}

std::unique_ptr<std::vector<uint8_t>> c_return_unique_ptr_vector_u8() {
  auto vec = std::unique_ptr<std::vector<uint8_t>>(new std::vector<uint8_t>());
  vec->push_back(86);
  vec->push_back(75);
  vec->push_back(30);
  vec->push_back(9);
  return vec;
}

std::unique_ptr<std::vector<double>> c_return_unique_ptr_vector_f64() {
  auto vec = std::unique_ptr<std::vector<double>>(new std::vector<double>());
  vec->push_back(86.0);
  vec->push_back(75.0);
  vec->push_back(30.0);
  vec->push_back(9.5);
  return vec;
}

std::unique_ptr<std::vector<std::string>> c_return_unique_ptr_vector_string() {
  return std::unique_ptr<std::vector<std::string>>(
      new std::vector<std::string>());
}

std::unique_ptr<std::vector<Shared>> c_return_unique_ptr_vector_shared() {
  auto vec = std::unique_ptr<std::vector<Shared>>(new std::vector<Shared>());
  vec->push_back(Shared{1010});
  vec->push_back(Shared{1011});
  return vec;
}

std::unique_ptr<std::vector<C>> c_return_unique_ptr_vector_opaque() {
  return std::unique_ptr<std::vector<C>>(new std::vector<C>());
}

const std::vector<uint8_t> &c_return_ref_vector(const C &c) {
  return c.get_v();
}

std::vector<uint8_t> &c_return_mut_vector(C &c) { return c.get_v(); }

rust::Vec<uint8_t> c_return_rust_vec_u8() {
  rust::Vec<uint8_t> vec{2, 0, 2, 0};
  return vec;
}

const rust::Vec<uint8_t> &c_return_ref_rust_vec(const C &c) {
  (void)c;
  throw std::runtime_error("unimplemented");
}

rust::Vec<uint8_t> &c_return_mut_rust_vec(C &c) {
  (void)c;
  throw std::runtime_error("unimplemented");
}

rust::Vec<rust::String> c_return_rust_vec_string() {
  return {"2", "0", "2", "0"};
}

rust::Vec<bool> c_return_rust_vec_bool() { return {true, true, false}; }

size_t c_return_identity(size_t n) { return n; }

size_t c_return_sum(size_t n1, size_t n2) { return n1 + n2; }

Enum c_return_enum(uint16_t n) {
  if (n <= static_cast<uint16_t>(Enum::AVal)) {
    return Enum::AVal;
  } else if (n <= static_cast<uint16_t>(Enum::BVal)) {
    return Enum::BVal;
  } else {
    return Enum::CVal;
  }
}

::A::AEnum c_return_ns_enum(uint16_t n) {
  if (n <= static_cast<uint16_t>(::A::AEnum::AAVal)) {
    return ::A::AEnum::AAVal;
  } else if (n <= static_cast<uint16_t>(::A::AEnum::ABVal)) {
    return ::A::AEnum::ABVal;
  } else {
    return ::A::AEnum::ACVal;
  }
}

::A::B::ABEnum c_return_nested_ns_enum(uint16_t n) {
  if (n <= static_cast<uint16_t>(::A::B::ABEnum::ABAVal)) {
    return ::A::B::ABEnum::ABAVal;
  } else if (n <= static_cast<uint16_t>(::A::B::ABEnum::ABBVal)) {
    return ::A::B::ABEnum::ABBVal;
  } else {
    return ::A::B::ABEnum::ABCVal;
  }
}

const C *c_return_const_ptr(size_t c) { return new C(c); }

C *c_return_mut_ptr(size_t c) { return new C(c); }

kj::Own<C> c_return_kj_own(size_t n) { return kj::heap<C>(n); }

size_t c_sizeof_shared_with_kj_own() { return sizeof(SharedWithKjOwn); }

size_t c_alignof_shared_with_kj_own() { return alignof(SharedWithKjOwn); }

size_t c_sizeof_shared_with_multiple_kj_owns() {
  return sizeof(SharedWithMultipleKjOwns);
}

size_t c_alignof_shared_with_multiple_kj_owns() {
  return alignof(SharedWithMultipleKjOwns);
}

SharedWithKjOwn c_return_shared_with_kj_own(size_t n) {
  return SharedWithKjOwn{kj::heap<C>(n)};
}

SharedWithMultipleKjOwns c_return_shared_with_multiple_kj_owns(size_t first,
                                                               size_t second) {
  return SharedWithMultipleKjOwns{kj::heap<C>(first), kj::heap<C>(second)};
}

size_t c_take_shared_with_kj_own_by_value(SharedWithKjOwn shared) {
  return shared.own->get();
}

size_t c_take_shared_with_kj_own_by_ref(const SharedWithKjOwn &shared) {
  return shared.own->get();
}

size_t
c_take_shared_with_multiple_kj_owns_by_value(SharedWithMultipleKjOwns shared) {
  return shared.first->get() + shared.second->get();
}

size_t c_take_shared_with_multiple_kj_owns_by_ref(
    const SharedWithMultipleKjOwns &shared) {
  return shared.first->get() + shared.second->get();
}

SharedWithKjOwn c_roundtrip_shared_with_kj_own(SharedWithKjOwn shared) {
  shared.own->set(shared.own->get() + 1);
  return shared;
}

SharedWithMultipleKjOwns
c_roundtrip_shared_with_multiple_kj_owns(SharedWithMultipleKjOwns shared) {
  shared.first->set(shared.first->get() + 10);
  shared.second->set(shared.second->get() + 20);
  return shared;
}

kj::Rc<RcC> c_return_kj_rc(size_t n) { return kj::rc<RcC>(n); }

kj::Rc<NonRefcountedRcC> c_return_non_refcounted_kj_rc(size_t n) {
  return kj::rc<NonRefcountedRcC>(n);
}

size_t c_take_non_refcounted_kj_rc_by_ref(const kj::Rc<NonRefcountedRcC> &rc) {
  return rc->get();
}

size_t c_sizeof_shared_with_kj_rc() { return sizeof(SharedWithKjRc); }

size_t c_alignof_shared_with_kj_rc() { return alignof(SharedWithKjRc); }

size_t c_sizeof_shared_with_multiple_kj_rcs() {
  return sizeof(SharedWithMultipleKjRcs);
}

size_t c_alignof_shared_with_multiple_kj_rcs() {
  return alignof(SharedWithMultipleKjRcs);
}

SharedWithKjRc c_return_shared_with_kj_rc(size_t n) {
  return SharedWithKjRc{kj::rc<RcC>(n)};
}

SharedWithMultipleKjRcs c_return_shared_with_multiple_kj_rcs(size_t first,
                                                             size_t second) {
  return SharedWithMultipleKjRcs{kj::rc<RcC>(first), kj::rc<RcC>(second)};
}

size_t c_take_shared_with_kj_rc_by_value(SharedWithKjRc shared) {
  return shared.rc->get();
}

size_t c_take_shared_with_kj_rc_by_ref(const SharedWithKjRc &shared) {
  return shared.rc->get();
}

size_t
c_take_shared_with_multiple_kj_rcs_by_value(SharedWithMultipleKjRcs shared) {
  return shared.first->get() + shared.second->get();
}

size_t c_take_shared_with_multiple_kj_rcs_by_ref(
    const SharedWithMultipleKjRcs &shared) {
  return shared.first->get() + shared.second->get();
}

SharedWithKjRc c_roundtrip_shared_with_kj_rc(SharedWithKjRc shared) {
  shared.rc->set(shared.rc->get() + 1);
  return shared;
}

SharedWithMultipleKjRcs
c_roundtrip_shared_with_multiple_kj_rcs(SharedWithMultipleKjRcs shared) {
  shared.first->set(shared.first->get() + 10);
  shared.second->set(shared.second->get() + 20);
  return shared;
}

kj::Arc<ArcC> c_return_kj_arc(size_t n) { return kj::arc<ArcC>(n); }

kj::Arc<NonAtomicArcC> c_return_non_atomic_kj_arc(size_t n) {
  return kj::arc<NonAtomicArcC>(n);
}

size_t c_take_non_atomic_kj_arc_by_ref(const kj::Arc<NonAtomicArcC> &arc) {
  return arc->get();
}

size_t c_sizeof_shared_with_kj_arc() { return sizeof(SharedWithKjArc); }

size_t c_alignof_shared_with_kj_arc() { return alignof(SharedWithKjArc); }

size_t c_sizeof_shared_with_multiple_kj_arcs() {
  return sizeof(SharedWithMultipleKjArcs);
}

size_t c_alignof_shared_with_multiple_kj_arcs() {
  return alignof(SharedWithMultipleKjArcs);
}

SharedWithKjArc c_return_shared_with_kj_arc(size_t n) {
  return SharedWithKjArc{kj::arc<ArcC>(n)};
}

SharedWithMultipleKjArcs c_return_shared_with_multiple_kj_arcs(size_t first,
                                                               size_t second) {
  return SharedWithMultipleKjArcs{kj::arc<ArcC>(first), kj::arc<ArcC>(second)};
}

size_t c_take_shared_with_kj_arc_by_value(SharedWithKjArc shared) {
  return shared.arc->get();
}

size_t c_take_shared_with_kj_arc_by_ref(const SharedWithKjArc &shared) {
  return shared.arc->get();
}

size_t
c_take_shared_with_multiple_kj_arcs_by_value(SharedWithMultipleKjArcs shared) {
  return shared.first->get() + shared.second->get();
}

size_t c_take_shared_with_multiple_kj_arcs_by_ref(
    const SharedWithMultipleKjArcs &shared) {
  return shared.first->get() + shared.second->get();
}

SharedWithKjArc c_roundtrip_shared_with_kj_arc(SharedWithKjArc shared) {
  shared.arc->set(shared.arc->get() + 1);
  return shared;
}

SharedWithMultipleKjArcs
c_roundtrip_shared_with_multiple_kj_arcs(SharedWithMultipleKjArcs shared) {
  shared.first->set(shared.first->get() + 10);
  shared.second->set(shared.second->get() + 20);
  return shared;
}

Borrow::Borrow(const std::string &s) : s(s) {}

void Borrow::const_member() const {}

void Borrow::nonconst_member() {}

std::unique_ptr<Borrow> c_return_borrow(const std::string &s) {
  return std::unique_ptr<Borrow>(new Borrow(s));
}

void c_take_primitive(size_t n) {
  if (n == 2020) {
    cxx_test_suite_set_correct();
  }
}

void c_take_shared(Shared shared) {
  if (shared.z == 2020) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ns_shared(::A::AShared shared) {
  if (shared.type == 2020) {
    cxx_test_suite_set_correct();
  }
}

void c_take_nested_ns_shared(::A::B::ABShared shared) {
  if (shared.z == 2020) {
    cxx_test_suite_set_correct();
  }
}

void c_take_box(rust::Box<R> r) {
  if (cxx_test_suite_r_is_correct(&*r)) {
    cxx_test_suite_set_correct();
  }
}

void c_take_unique_ptr(std::unique_ptr<C> c) {
  if (c->get() == 2020) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ref_r(const R &r) {
  if (cxx_test_suite_r_is_correct(&r)) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ref_c(const C &c) {
  if (c.get() == 2020) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ref_ns_c(const ::H::H &h) {
  if (h.h == "hello") {
    cxx_test_suite_set_correct();
  }
}

void c_take_str(rust::Str s) {
  if (std::string(s) == "2020") {
    cxx_test_suite_set_correct();
  }
}

void c_take_slice_char(rust::Slice<const char> s) {
  if (std::string(s.data(), s.size()) == "2020") {
    cxx_test_suite_set_correct();
  }
}

void c_take_slice_char16(rust::Slice<const char16_t> s) {
  if (std::u16string(s.data(), s.size()) == u"2020") {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_char16(rust::Vec<char16_t> v) {
  if (std::u16string(v.data(), v.size()) == u"2020") {
    cxx_test_suite_set_correct();
  }
}

void c_take_slice_shared(rust::Slice<const Shared> s) {
  if (s.size() == 2 && s.data()->z == 2020 && s[1].z == 2021 &&
      s.at(1).z == 2021 && s.front().z == 2020 && s.back().z == 2021) {
    cxx_test_suite_set_correct();
  }
}

void c_take_slice_shared_sort(rust::Slice<Shared> s) {
  // Exercise requirements of RandomAccessIterator.
  // https://en.cppreference.com/w/cpp/named_req/RandomAccessIterator
  std::sort(s.begin(), s.end());
  if (s[0].z == 0 && s[1].z == 2 && s[2].z == 4 && s[3].z == 7) {
    cxx_test_suite_set_correct();
  }
}

void c_take_slice_r(rust::Slice<const R> s) {
  if (s.size() == 3 && s[0].get() == 2020 && s[1].get() == 2050) {
    cxx_test_suite_set_correct();
  }
}

bool operator<(const R &a, const R &b) noexcept { return a.get() < b.get(); }

void c_take_slice_r_sort(rust::Slice<R> s) {
  std::qsort(s.data(), s.size(), rust::size_of<decltype(s)::value_type>(),
             [](const void *fst, const void *snd) {
               auto &a = *static_cast<const R *>(fst);
               auto &b = *static_cast<const R *>(snd);
               return a < b ? -1 : b < a ? 1 : 0;
             });
  if (s[0].get() == 2020 && s[1].get() == 2021 && s[2].get() == 2050) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_string(rust::String s) {
  if (std::string(s) == "2020") {
    cxx_test_suite_set_correct();
  }
}

void c_take_unique_ptr_string(std::unique_ptr<std::string> s) {
  if (*s == "2020") {
    cxx_test_suite_set_correct();
  }
}

void c_take_unique_ptr_vector_u8(std::unique_ptr<std::vector<uint8_t>> v) {
  if (v->size() == 3) {
    cxx_test_suite_set_correct();
  }
}

void c_take_unique_ptr_vector_f64(std::unique_ptr<std::vector<double>> v) {
  if (v->size() == 5) {
    cxx_test_suite_set_correct();
  }
}

void c_take_unique_ptr_vector_string(
    std::unique_ptr<std::vector<std::string>> v) {
  (void)v;
  cxx_test_suite_set_correct();
}

void c_take_unique_ptr_vector_shared(std::unique_ptr<std::vector<Shared>> v) {
  if (v->size() == 3) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ref_vector(const std::vector<uint8_t> &v) {
  if (v.size() == 4) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec(rust::Vec<uint8_t> v) { c_take_ref_rust_vec(v); }

void c_take_rust_vec_index(rust::Vec<uint8_t> v) {
  try {
    v.at(100);
  } catch (const std::out_of_range &ex) {
    std::string expected = "rust::Vec index out of range";
    if (ex.what() == expected) {
      cxx_test_suite_set_correct();
    }
  }
}

void c_take_rust_vec_shared(rust::Vec<Shared> v) {
  uint32_t sum = 0;
  for (auto i : v) {
    sum += i.z;
  }
  if (sum == 2021) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_ns_shared(rust::Vec<::A::AShared> v) {
  uint32_t sum = 0;
  for (auto i : v) {
    sum += i.type;
  }
  if (sum == 2021) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_nested_ns_shared(rust::Vec<::A::B::ABShared> v) {
  uint32_t sum = 0;
  for (auto i : v) {
    sum += i.z;
  }
  if (sum == 2021) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_string(rust::Vec<rust::String> v) {
  (void)v;
  cxx_test_suite_set_correct();
}

void c_take_rust_vec_shared_forward_iterator(rust::Vec<Shared> v) {
  // Exercise requirements of ForwardIterator
  // https://en.cppreference.com/w/cpp/named_req/ForwardIterator
  uint32_t sum = 0, csum = 0;
  for (auto it = v.begin(), it_end = v.end(); it != it_end; it++) {
    sum += it->z;
  }
  for (auto it = v.cbegin(), it_end = v.cend(); it != it_end; it++) {
    csum += it->z;
  }
  if (sum == 2021 && csum == 2021) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_shared_sort(rust::Vec<Shared> v) {
  // Exercise requirements of RandomAccessIterator.
  // https://en.cppreference.com/w/cpp/named_req/RandomAccessIterator
  std::sort(v.begin(), v.end());
  if (v[0].z == 0 && v[1].z == 2 && v[2].z == 4 && v[3].z == 7) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_shared_index(rust::Vec<Shared> v) {
  if (v[0].z == 1010 && v.at(0).z == 1010 && v.front().z == 1010 &&
      v[1].z == 1011 && v.at(1).z == 1011 && v.back().z == 1011) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_shared_push(rust::Vec<Shared> v) {
  v.push_back(Shared{3});
  v.emplace_back(Shared{2});
  if (v[v.size() - 2].z == 3 && v.back().z == 2) {
    cxx_test_suite_set_correct();
  }
}

void c_take_rust_vec_shared_truncate(rust::Vec<Shared> v) {
  v.truncate(1);
  if (v.size() == 1) {
    v.truncate(0);
    if (v.size() == 0) {
      cxx_test_suite_set_correct();
    }
  }
}

void c_take_rust_vec_shared_clear(rust::Vec<Shared> v) {
  v.clear();
  if (v.size() == 0) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ref_rust_vec(const rust::Vec<uint8_t> &v) {
  uint8_t sum = std::accumulate(v.begin(), v.end(), 0);
  if (sum == 200) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ref_rust_vec_string(const rust::Vec<rust::String> &v) {
  (void)v;
  cxx_test_suite_set_correct();
}

void c_take_ref_rust_vec_index(const rust::Vec<uint8_t> &v) {
  if (v[0] == 86 && v.at(0) == 86 && v.front() == 86 && v[1] == 75 &&
      v.at(1) == 75 && v[3] == 9 && v.at(3) == 9 && v.back() == 9) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ref_rust_vec_copy(const rust::Vec<uint8_t> &v) {
  // The std::copy() will make sure rust::Vec<>::const_iterator satisfies the
  // requirements for std::iterator_traits.
  // https://en.cppreference.com/w/cpp/iterator/iterator_traits
  std::vector<uint8_t> stdv;
  std::copy(v.begin(), v.end(), std::back_inserter(stdv));
  uint8_t sum = std::accumulate(stdv.begin(), stdv.end(), 0);
  if (sum == 200) {
    cxx_test_suite_set_correct();
  }
}

const SharedString &c_take_ref_shared_string(const SharedString &s) {
  if (std::string(s.msg) == "2020") {
    cxx_test_suite_set_correct();
  }
  return s;
}

void c_take_callback(rust::Fn<size_t(rust::String)> callback) {
  callback("2020");
}

void c_take_callback_ref(rust::Fn<void(const rust::String &)> callback) {
  const rust::String string = "2020";
  callback(string);
}

void c_take_callback_mut(rust::Fn<void(rust::String &)> callback) {
  rust::String string = "2020";
  callback(string);
}

void c_take_enum(Enum e) {
  if (e == Enum::AVal) {
    cxx_test_suite_set_correct();
  }
}

void c_take_ns_enum(::A::AEnum e) {
  if (e == ::A::AEnum::AAVal) {
    cxx_test_suite_set_correct();
  }
}

void c_take_nested_ns_enum(::A::B::ABEnum e) {
  if (e == ::A::B::ABEnum::ABAVal) {
    cxx_test_suite_set_correct();
  }
}

size_t c_take_const_ptr(const C *c) { return c->get(); }

size_t c_take_mut_ptr(C *c) {
  size_t result = c->get();
  delete c;
  return result;
}

void c_try_return_void() {}

size_t c_try_return_primitive() { return 2020; }

size_t c_fail_return_primitive() { throw std::logic_error("logic error"); }
size_t c_fail_kj_exception_return_primitive() {
  kj::throwFatalException(KJ_EXCEPTION(FAILED, "logic error"));
}

size_t c_fail_kj_exception_disconnected_return_primitive() {
  kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "connection lost"));
}

// User-defined literal to create kj::ArrayPtr<const kj::byte> from string literal
kj::ArrayPtr<const kj::byte> operator""_kjb(const char *str, size_t len) {
  return kj::ArrayPtr<const kj::byte>(reinterpret_cast<const kj::byte *>(str),
                                      len);
}

size_t c_fail_kj_exception_with_details_return_primitive() {
  auto ex = KJ_EXCEPTION(FAILED, "test exception with details");
  // Add some test details - these will be type_id + data pairs
  ex.setDetail(42, kj::heapArray("test detail 1"_kjb));
  ex.setDetail(999, kj::heapArray("another detail"_kjb));
  kj::throwFatalException(kj::mv(ex));
}

size_t c_cancel_return_primitive() { throw kj::CanceledException{}; }

// Call Rust function that panics with CanceledException
// This will be caught and re-thrown as kj::CanceledException
size_t c_cancel_via_rust_return_primitive() {
  r_cancel_panic_test();
  return 2020; // Should not reach here
}

// Test C++->Rust->C++ cancellation roundtrip
// Call Rust function that calls C++ function that throws CanceledException
size_t c_cancel_roundtrip_return_primitive() {
  r_call_c_cancel_return_primitive();
  return 2020; // Should not reach here
}

// The functions below throw even though their bridge signature is infallible.
// Rust cannot report the exception to its caller, so it panics instead. None of
// them may terminate the process.
void c_infallible_fail_void() {
  kj::throwFatalException(KJ_EXCEPTION(FAILED, "infallible void failure"));
}

size_t c_infallible_fail_primitive() {
  throw std::logic_error("infallible logic error");
}

size_t c_infallible_fail_kj_exception_disconnected() {
  kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "infallible disconnect"));
}

rust::String c_infallible_fail_rust_string() {
  kj::throwFatalException(KJ_EXCEPTION(FAILED, "infallible string failure"));
}

// Neither a kj::Exception nor a std::exception. This is the shape of
// workerd's jsg::JsExceptionThrown, which used to abort the process here.
namespace {
struct ForeignException {};
} // namespace

size_t c_infallible_fail_foreign_exception() { throw ForeignException{}; }

size_t c_infallible_cancel() { throw kj::CanceledException{}; }

// Test C++->Rust->C++ roundtrip of an exception through an infallible bridge
// function on both sides: the exception becomes a panic in Rust and is rethrown
// as a kj::Exception when it crosses back into C++.
size_t c_infallible_fail_roundtrip() {
  r_call_c_infallible_fail_primitive();
  return 2020; // Should not reach here
}

rust::Box<R> c_try_return_box() { return c_return_box(); }

const rust::String &c_try_return_ref(const rust::String &s) { return s; }

rust::Str c_try_return_str(rust::Str s) { return s; }

rust::Slice<const uint8_t> c_try_return_sliceu8(rust::Slice<const uint8_t> s) {
  return s;
}

rust::Slice<uint8_t> c_try_return_mutsliceu8(rust::Slice<uint8_t> s) {
  return s;
}

rust::String c_try_return_rust_string() { return c_return_rust_string(); }

std::unique_ptr<std::string> c_try_return_unique_ptr_string() {
  return c_return_unique_ptr_string();
}

rust::Vec<uint8_t> c_try_return_rust_vec() {
  throw std::runtime_error("unimplemented");
}

rust::Vec<rust::String> c_try_return_rust_vec_string() {
  throw std::runtime_error("unimplemented");
}

const rust::Vec<uint8_t> &c_try_return_ref_rust_vec(const C &c) {
  (void)c;
  throw std::runtime_error("unimplemented");
}

size_t c_get_use_count(const std::weak_ptr<C> &weak) noexcept {
  return weak.use_count();
}

extern "C" C *cxx_test_suite_get_unique_ptr() noexcept {
  return std::unique_ptr<C>(new C{2020}).release();
}

extern "C" void
cxx_test_suite_get_shared_ptr(std::shared_ptr<C> *repr) noexcept {
  new (repr) std::shared_ptr<C>(new C{2020});
}

extern "C" std::string *cxx_test_suite_get_unique_ptr_string() noexcept {
  return std::unique_ptr<std::string>(new std::string("2020")).release();
}

rust::String C::cOverloadedMethod(int32_t x) const {
  return rust::String(std::to_string(x));
}

rust::String C::cOverloadedMethod(rust::Str x) const {
  return rust::String(std::string(x));
}

rust::String cOverloadedFunction(int x) {
  return rust::String(std::to_string(x));
}

rust::String cOverloadedFunction(rust::Str x) {
  return rust::String(std::string(x));
}

void c_take_trivial_ptr(std::unique_ptr<D> d) {
  if (d->d == 30) {
    cxx_test_suite_set_correct();
  }
}

void c_take_trivial_ref(const D &d) {
  if (d.d == 30) {
    cxx_test_suite_set_correct();
  }
}

void c_take_trivial_mut_ref(D &d) { (void)d; }

void c_take_trivial_pin_ref(const D &d) { (void)d; }

void c_take_trivial_pin_mut_ref(D &d) { (void)d; }

void D::c_take_trivial_ref_method() const {
  if (d == 30) {
    cxx_test_suite_set_correct();
  }
}

void D::c_take_trivial_mut_ref_method() {
  if (d == 30) {
    cxx_test_suite_set_correct();
  }
}

void c_take_trivial(D d) {
  if (d.d == 30) {
    cxx_test_suite_set_correct();
  }
}

void c_take_trivial_ns_ptr(std::unique_ptr<::G::G> g) {
  if (g->g == 30) {
    cxx_test_suite_set_correct();
  }
}

void c_take_trivial_ns_ref(const ::G::G &g) {
  if (g.g == 30) {
    cxx_test_suite_set_correct();
  }
}

void c_take_trivial_ns(::G::G g) {
  if (g.g == 30) {
    cxx_test_suite_set_correct();
  }
}

void c_take_opaque_ptr(std::unique_ptr<E> e) {
  if (e->e == 40) {
    cxx_test_suite_set_correct();
  }
}

void c_take_opaque_ns_ptr(std::unique_ptr<::F::F> f) {
  if (f->f == 40) {
    cxx_test_suite_set_correct();
  }
}

void c_take_opaque_ref(const E &e) {
  if (e.e == 40 && e.e_str == "hello") {
    cxx_test_suite_set_correct();
  }
}

void E::c_take_opaque_ref_method() const {
  if (e == 40 && e_str == "hello") {
    cxx_test_suite_set_correct();
  }
}

void E::c_take_opaque_mut_ref_method() {
  if (e == 40 && e_str == "hello") {
    cxx_test_suite_set_correct();
  }
}

void c_take_opaque_ns_ref(const ::F::F &f) {
  if (f.f == 40 && f.f_str == "hello") {
    cxx_test_suite_set_correct();
  }
}

std::unique_ptr<D> c_return_trivial_ptr() {
  auto d = std::unique_ptr<D>(new D());
  d->d = 30;
  return d;
}

D c_return_trivial() {
  D d;
  d.d = 30;
  return d;
}

std::unique_ptr<::G::G> c_return_trivial_ns_ptr() {
  auto g = std::unique_ptr<::G::G>(new ::G::G());
  g->g = 30;
  return g;
}

::G::G c_return_trivial_ns() {
  ::G::G g;
  g.g = 30;
  return g;
}

std::unique_ptr<E> c_return_opaque_ptr() {
  auto e = std::unique_ptr<E>(new E());
  e->e = 40;
  e->e_str = std::string("hello");
  return e;
}

E &c_return_opaque_mut_pin(E &e) { return e; }

std::unique_ptr<::F::F> c_return_ns_opaque_ptr() {
  auto f = std::unique_ptr<::F::F>(new ::F::F());
  f->f = 40;
  f->f_str = std::string("hello");
  return f;
}

extern "C" const char *cxx_run_test() noexcept {
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define ASSERT(x)                                                              \
  do {                                                                         \
    if (!(x)) {                                                                \
      return "Assertion failed: `" #x "`, " __FILE__ ":" TOSTRING(__LINE__);   \
    }                                                                          \
  } while (false)

  ASSERT(rust::size_of<R>() == sizeof(size_t));
  ASSERT(rust::align_of<R>() == alignof(size_t));
  ASSERT(rust::size_of<size_t>() == sizeof(size_t));
  ASSERT(rust::align_of<size_t>() == alignof(size_t));

  ASSERT(r_return_primitive() == 2020);
  ASSERT(r_return_shared().z == 2020);
  ASSERT(cxx_test_suite_r_is_correct(&*r_return_box()));
  ASSERT(r_return_unique_ptr()->get() == 2020);
  ASSERT(r_return_shared_ptr()->get() == 2020);
  ASSERT(r_return_ref(Shared{2020}) == 2020);
  ASSERT(std::string(r_return_str(Shared{2020})) == "2020");
  ASSERT(std::string(r_return_rust_string()) == "2020");
  ASSERT(*r_return_unique_ptr_string() == "2020");
  ASSERT(r_return_identity(2020) == 2020);
  ASSERT(r_return_sum(2020, 1) == 2021);
  ASSERT(r_return_enum(0) == Enum::AVal);
  ASSERT(r_return_enum(1) == Enum::BVal);
  ASSERT(r_return_enum(2021) == Enum::CVal);

  r_take_primitive(2020);
  r_take_shared(Shared{2020});
  r_take_unique_ptr(std::unique_ptr<C>(new C{2020}));
  r_take_shared_ptr(std::shared_ptr<C>(new C{2020}));
  r_take_ref_c(C{2020});
  r_take_str(rust::Str("2020"));
  r_take_slice_char(rust::Slice<const char>(SLICE_DATA, sizeof(SLICE_DATA)));
  r_take_slice_char16(rust::Slice<const char16_t>(SLICE_DATA16, 4));
  ASSERT(std::u16string(r_return_rust_vec_char16().data(), 4) == u"2020");
  r_take_rust_string(rust::String("2020"));
  r_take_unique_ptr_string(
      std::unique_ptr<std::string>(new std::string("2020")));
  r_take_ref_vector(std::vector<uint8_t>{20, 2, 0});
  std::vector<uint64_t> empty_vector;
  r_take_ref_empty_vector(empty_vector);
  empty_vector.reserve(10);
  r_take_ref_empty_vector(empty_vector);
  r_take_enum(Enum::AVal);

  ASSERT(r_try_return_primitive() == 2020);
  try {
    r_fail_return_primitive();
    ASSERT(false);
  } catch (const kj::Exception &e) {
    KJ_ASSERT(e.getDescription() == "rust error"_kj);
    KJ_ASSERT(e.getFile() == "rust/cxx/tests/ffi/lib.rs"_kj);
  }

  KJ_ASSERT(r_result_kj_exception_return_primitive() == 2020);
  try {
    r_result_kj_exception_fail_return_primitive();
    KJ_ASSERT(false);
  } catch (const kj::Exception &e) {
    KJ_ASSERT(e.getDescription() == "test kj exception"_kj);
    KJ_ASSERT(e.getFile() == "rust/cxx/tests/ffi/lib.rs"_kj);
  }

  // Test Rust->C++ DISCONNECTED exception passing
  try {
    r_result_kj_exception_disconnected_return_primitive();
    KJ_ASSERT(false);
  } catch (const kj::Exception &e) {
    KJ_ASSERT(e.getType() == kj::Exception::Type::DISCONNECTED);
    KJ_ASSERT(e.getDescription() == "connection lost from rust"_kj);
    KJ_ASSERT(e.getFile() == "rust/cxx/tests/ffi/lib.rs"_kj);
  }

  // Test Rust->C++ exception with details
  try {
    r_result_kj_exception_with_details_return_primitive();
    KJ_ASSERT(false);
  } catch (const kj::Exception &e) {
    KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
    KJ_ASSERT(e.getDescription() == "rust exception with details"_kj);
    KJ_ASSERT(e.getFile() == "rust/cxx/tests/ffi/lib.rs"_kj);

    // Check details
    auto details = e.getDetails();
    KJ_ASSERT(details.size() == 2, "expected 2 details");

    // Check first detail
    KJ_ASSERT(details[0].id == 123);
    KJ_ASSERT(details[0].value.size() == 13);
    KJ_ASSERT(memcmp(details[0].value.begin(), "rust detail 1", 13) == 0);

    // Check second detail
    KJ_ASSERT(details[1].id == 456);
    KJ_ASSERT(details[1].value.size() == 13);
    KJ_ASSERT(memcmp(details[1].value.begin(), "rust detail 2", 13) == 0);
  }

  auto r = r_return_box();
  ASSERT(r->get() == 2020);
  ASSERT(r->set(2021) == 2021);
  ASSERT(r->get() == 2021);

  using std::swap;
  auto r2 = r_return_box();
  swap(r, r2);
  ASSERT(r->get() == 2020);
  ASSERT(r2->get() == 2021);

  ASSERT(std::string(Shared{0}.r_method_on_shared()) == "2020");

  ASSERT(std::string(rAliasedFunction(2020)) == "2020");

  ASSERT(Shared{1} == Shared{1});
  ASSERT(Shared{1} != Shared{2});

  rust::String first = "first", second = "second", sec = "sec";
  bool (rust::String::*cmp)(const rust::String &) const;
  bool first_first, first_second, sec_second, second_sec;
  for (auto test : {
           std::tuple<decltype(cmp), bool, bool, bool, bool>{
               &rust::String::operator==, true, false, false, false},
           {&rust::String::operator!=, false, true, true, true},
           {&rust::String::operator<, false, true, true, false},
           {&rust::String::operator<=, true, true, true, false},
           {&rust::String::operator>, false, false, false, true},
           {&rust::String::operator>=, true, false, false, true},
       }) {
    std::tie(cmp, first_first, first_second, sec_second, second_sec) = test;
    ASSERT((first.*cmp)(first) == first_first);
    ASSERT((first.*cmp)(second) == first_second);
    ASSERT((sec.*cmp)(second) == sec_second);
    ASSERT((second.*cmp)(sec) == second_sec);
  }

  rust::String cstring = "test";
  ASSERT(cstring.length() == 4);
  ASSERT(strncmp(cstring.data(), "test", 4) == 0);
  ASSERT(strncmp(cstring.c_str(), "test", 5) == 0);
  ASSERT(cstring.length() == 4);

  rust::String other_cstring = "foo";
  swap(cstring, other_cstring);
  ASSERT(cstring == "foo");
  ASSERT(other_cstring == "test");

  ASSERT(cstring.capacity() == 3);
  cstring.reserve(2);
  ASSERT(cstring.capacity() == 3);
  cstring.reserve(5);
  ASSERT(cstring.capacity() >= 5);

  {
    rust::Str out_param;
    r_return_str_via_out_param(Shared{2020}, out_param);
    ASSERT(out_param == "2020");

#if __cplusplus >= 201703L
    std::string_view out_param_as_string_view{out_param};
    ASSERT(out_param_as_string_view == "2020");
#endif
  }

  rust::Str cstr = "test";
  rust::Str other_cstr = "foo";
  swap(cstr, other_cstr);
  ASSERT(cstr == "foo");
  ASSERT(other_cstr == "test");

  // Auto because u8"..." is `const char*` before C++20, and `const char8_t*` since.
  const auto *utf8_literal = u8"Test string";
  const char16_t *utf16_literal = u"Test string";
  rust::String utf8_rstring = utf8_literal;
  rust::String utf16_rstring = utf16_literal;
  ASSERT(utf8_rstring == utf16_rstring);

  const char *bad_utf8_literal = "test\x80";
  const char16_t *bad_utf16_literal = u"test\xDD1E";
  rust::String bad_utf8_rstring = rust::String::lossy(bad_utf8_literal);
  rust::String bad_utf16_rstring = rust::String::lossy(bad_utf16_literal);
  ASSERT(bad_utf8_rstring == bad_utf16_rstring);

  const char *ascii_literal = "42";
  const char *latin1_literal = "\xff";
  rust::String ascii =
      rust::String::latin1(ascii_literal, strlen(ascii_literal));
  rust::String latin1 =
      rust::String::latin1(latin1_literal, strlen(latin1_literal));
  KJ_ASSERT(kj::arrayPtr(ascii.c_str(), ascii.length()) == "42"_kjb);
  // \xff is a valid latin1 character but needs to converted into multi-byte utf8.
  char expected[] = {static_cast<char>(195), static_cast<char>(191)};
  KJ_ASSERT(kj::arrayPtr(latin1.c_str(), latin1.length()) ==
            kj::arrayPtr(expected));

  // Test Slice<T> explicit constructor from container
  {
    std::vector<int> cpp_vec{1, 2, 3};
    rust::Slice<int> slice_of_cpp_vec(cpp_vec);
    ASSERT(slice_of_cpp_vec.data() == cpp_vec.data());
    ASSERT(slice_of_cpp_vec.size() == cpp_vec.size());
    ASSERT(slice_of_cpp_vec[0] == 1);
  }

  // Test Slice<T> template deduction guides
#ifdef __cpp_deduction_guides
  {
    // std::array<T> -> Slice<T>
    std::array<int, 3> cpp_array{1, 2, 3};
    auto auto_slice_of_cpp_array = rust::Slice(cpp_array);
    static_assert(
        std::is_same_v<decltype(auto_slice_of_cpp_array), rust::Slice<int>>);
  }
  {
    // const std::array<T> -> Slice<const T>
    const std::array<int, 3> cpp_array{1, 2, 3};
    auto auto_slice_of_cpp_array = rust::Slice(cpp_array);
    static_assert(std::is_same_v<decltype(auto_slice_of_cpp_array),
                                 rust::Slice<const int>>);
  }
  {
    // std::array<const T> -> Slice<const T>
    std::array<const int, 3> cpp_array{1, 2, 3};
    auto auto_slice_of_cpp_array = rust::Slice(cpp_array);
    static_assert(std::is_same_v<decltype(auto_slice_of_cpp_array),
                                 rust::Slice<const int>>);
  }
  {
    // std::vector<T> -> Slice<T>
    std::vector<int> cpp_vec{1, 2, 3};
    auto auto_slice_of_cpp_vec = rust::Slice(cpp_vec);
    static_assert(
        std::is_same_v<decltype(auto_slice_of_cpp_vec), rust::Slice<int>>);
  }
  {
    // const std::vector<T> -> Slice<const T>
    const std::vector<int> cpp_vec{1, 2, 3};
    auto auto_slice_of_cpp_vec = rust::Slice(cpp_vec);
    static_assert(std::is_same_v<decltype(auto_slice_of_cpp_vec),
                                 rust::Slice<const int>>);
  }
#ifdef __cpp_lib_span
  {
    // std::span<T> -> Slice<T>
    std::array<int, 3> cpp_array{1, 2, 3};
    std::span<int> cpp_span(cpp_array);
    auto auto_slice_of_cpp_span = rust::Slice(cpp_span);
    static_assert(
        std::is_same_v<decltype(auto_slice_of_cpp_span), rust::Slice<int>>);
  }
  {
    // std::span<const T> -> Slice<const T>
    const std::array<int, 3> cpp_array{1, 2, 3};
    std::span<const int> cpp_span(cpp_array);
    auto auto_slice_of_cpp_span = rust::Slice(cpp_span);
    static_assert(std::is_same_v<decltype(auto_slice_of_cpp_span),
                                 rust::Slice<const int>>);
  }
#endif // __cpp_lib_span
#endif // __cpp_deduction_guides

  rust::Vec<int> vec1{1, 2};
  rust::Vec<int> vec2{3, 4};
  swap(vec1, vec2);
  ASSERT(vec1[0] == 3 && vec1[1] == 4);
  ASSERT(vec2[0] == 1 && vec2[1] == 2);

  // Test Vec<usize> and Vec<isize>. These are weird because on Linux and
  // Windows size_t is exactly the same C++ type as one of the sized integer
  // types (typically uint64_t, both of which are defined as unsigned long),
  // while on macOS it is a distinct type.
  // https://github.com/dtolnay/cxx/issues/705
  (void)rust::Vec<size_t>();
  (void)rust::Vec<rust::isize>();

  try {
    r_panic("foobar");
    ASSERT(false);
  } catch (const kj::Exception &e) {
    ASSERT(std::strcmp(e.getDescription().cStr(),
                       "panic in cxx_test_suite::ffi::r_panic: foobar") == 0);
  }

  // Test C++->Rust CanceledException roundtrip
  try {
    c_cancel_via_rust_return_primitive();
    ASSERT(false); // Should not reach here
  } catch (const kj::CanceledException &) {
    // Expected: CanceledException should propagate through Rust panic back to C++
  } catch (...) {
    KJ_FAIL_ASSERT("Unexpected exception", kj::getCaughtExceptionAsKj());
  }

  // Test C++->Rust->C++ CanceledException roundtrip
  try {
    r_call_c_cancel_return_primitive();
    ASSERT(false); // Should not reach here
  } catch (const kj::CanceledException &) {
    // Expected: CanceledException should propagate from C++ through Rust back to C++
  } catch (...) {
    KJ_FAIL_ASSERT("Unexpected exception in roundtrip test",
                   kj::getCaughtExceptionAsKj());
  }

  // An exception thrown by an infallible C++ function becomes a panic in Rust,
  // which turns back into a kj::Exception here rather than aborting.
  try {
    r_call_c_infallible_fail_primitive();
    ASSERT(false); // Should not reach here
  } catch (const kj::Exception &e) {
    ASSERT(e.getDescription().contains("infallible logic error"));
  } catch (...) {
    KJ_FAIL_ASSERT("Unexpected exception in infallible failure test",
                   kj::getCaughtExceptionAsKj());
  }

  cxx_test_suite_set_correct();
  return nullptr;
}

} // namespace tests

namespace other {
void ns_c_take_trivial(::tests::D d) {
  if (d.d == 30) {
    cxx_test_suite_set_correct();
  }
}

::tests::D ns_c_return_trivial() {
  ::tests::D d;
  d.d = 30;
  return d;
}

void ns_c_take_ns_shared(::A::AShared shared) {
  if (shared.type == 2020) {
    cxx_test_suite_set_correct();
  }
}
} // namespace other

namespace I {
uint32_t I::get() const { return a; }

std::unique_ptr<I> ns_c_return_unique_ptr_ns() {
  return std::unique_ptr<I>(new I());
}
} // namespace I

// Instantiate any remaining class member functions not already covered above.
// This is an easy way to at least typecheck anything missed by unit tests.
// https://en.cppreference.com/w/cpp/language/class_template#Explicit_instantiation
// > When an explicit instantiation names a class template specialization, it
// > serves as an explicit instantiation of the same kind (declaration or
// > definition) of each of its non-inherited non-template members that has not
// > been previously explicitly specialized in the translation unit.
#if defined(CXX_TEST_INSTANTIATIONS)
template class rust::Box<tests::Shared>;
template class rust::Slice<const char>;
template class rust::Slice<const uint8_t>;
template class rust::Slice<uint8_t>;
template class rust::Slice<const tests::Shared>;
template class rust::Slice<tests::Shared>;
template class rust::Slice<const tests::R>;
template class rust::Slice<tests::R>;
template class rust::Vec<uint8_t>;
template class rust::Vec<rust::String>;
template class rust::Vec<tests::Shared>;
template class rust::Fn<size_t(rust::String)>;
#endif
