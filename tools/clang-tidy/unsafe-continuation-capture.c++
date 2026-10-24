#include "unsafe-continuation-capture.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tidy;

namespace workerd::clang_tidy {

// Per-function use-index: maps each VarDecl referenced inside the
// function body to the list of DeclRefExpr nodes that reference it.
// Built lazily by `getOrBuildFunctionUseMap` and cached for the
// lifetime of the current translation-unit analysis (cleared in
// `UnsafeContinuationCaptureCheck::onEndOfTranslationUnit`).
using FunctionUseMap =
    llvm::DenseMap<const clang::VarDecl *, llvm::SmallVector<const clang::DeclRefExpr *, 4>>;

// One-pass collector: visits the entire function body once and
// populates a FunctionUseMap keyed by VarDecl.
class FunctionUseMapBuilder: public clang::RecursiveASTVisitor<FunctionUseMapBuilder> {
 public:
  explicit FunctionUseMapBuilder(FunctionUseMap &out): Out(out) {}
  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (const auto *vd = clang::dyn_cast<clang::VarDecl>(e->getDecl())) {
      Out[vd].push_back(e);
    }
    return true;
  }

 private:
  FunctionUseMap &Out;
};

// Forward declaration. Defined below the anonymous namespace.
const FunctionUseMap &getOrBuildFunctionUseMap(const clang::FunctionDecl *fn);

namespace {

// ---- Suffix-qualified-name matching ----------------------------------------
//
// Returns true iff `qualifiedName` equals `suffix` or is of the form
// `<prefix>::<suffix>`. Borrowed in spirit from workerd-lint.c++; this
// avoids the substring trap that would match e.g. `foo::then_helper` against
// `then`.
static bool endsWithQualified(llvm::StringRef qualifiedName, llvm::StringRef suffix) {
  if (qualifiedName == suffix) return true;
  if (qualifiedName.size() < suffix.size() + 2) return false;
  if (!qualifiedName.ends_with(suffix)) return false;
  auto sep = qualifiedName.size() - suffix.size();
  return qualifiedName[sep - 1] == ':' && qualifiedName[sep - 2] == ':';
}

// ---- Async-sink callee identification --------------------------------------
//
// Lambdas passed as an argument to one of these callees are treated as
// "non-immediate" -- i.e. they outlive the full-expression that constructs
// them, and any reference / pointer / non-owning view captured by them can
// dangle by the time the lambda runs.
//
// These are matched as fully-qualified callee names. For member functions,
// the name compared is the method's qualified name (e.g.
// `kj::Promise::then`, `workerd::IoContext::run`). Plain functions are
// matched the same way (e.g. `kj::evalLater`).
//
// This list is intentionally conservative -- start narrow and expand. The
// `AsyncSinks` check option allows projects to add more without recompiling.
static const llvm::StringRef kBuiltinAsyncSinks[] = {
  // KJ promise continuations.
  "kj::Promise::then", "kj::Promise::catch_", "kj::Promise::eagerlyEvaluate", "kj::Promise::attach",
  "kj::Promise::detach",  // Explicit: result lives forever in event loop.
  "kj::ForkedPromise::addBranch", "kj::evalLater", "kj::evalLast", "kj::retryOnDisconnect",
  // Note: kj::evalNow is intentionally NOT included -- it invokes its
  // callable synchronously before returning, only wrapping any thrown
  // exception in the returned promise. Captures into an evalNow lambda
  // therefore have the same lifetime semantics as captures in an
  // immediately-invoked lambda and are safe.
  // Similarly jsg::Lock::evalNow is synchronous (try/catch wrapper).
  // JSG promise continuations.
  "workerd::jsg::Promise::then", "workerd::jsg::Promise::catch_",
  "workerd::jsg::Promise::markAsHandled",
  // IoContext entry points that schedule deferred work.
  "workerd::IoContext::run", "workerd::IoContext::addTask", "workerd::IoContext::addFunctor",
  "workerd::IoContext::addWaitUntil", "workerd::IoContext::awaitIo",
  "workerd::IoContext::awaitIoLegacy", "workerd::IoContext::awaitIoWithInputLock",
  "workerd::IoContext::awaitJs", "workerd::IoContext::makeReentryCallback",
  "workerd::IoContext::onAbort", "workerd::IoContext::blockConcurrencyWhile",
  // Timers.
  "workerd::IoContext::afterLimitTimeout",
  // Note: kj::TaskSet::add takes a kj::Promise<void>, not a callable,
  // so a lambda cannot be a direct argument. Removed.
  // Note: edgeworker-specific sinks (e.g. edgeworker::PromiseCache::findOrCreate)
  // should be configured via the AsyncSinks option in .clang-tidy.
};

static bool isAsyncSink(llvm::StringRef qualifiedName, const std::vector<std::string> &extras) {
  for (auto suffix: kBuiltinAsyncSinks) {
    if (endsWithQualified(qualifiedName, suffix)) return true;
  }
  for (const auto &extra: extras) {
    if (endsWithQualified(qualifiedName, extra)) return true;
  }
  return false;
}

// ---- Synchronous-sink callee identification --------------------------------
//
// Lambdas passed as an argument to one of these callees are invoked
// *synchronously* in the current activation, and any promise returned by
// the lambda is itself synchronously consumed (e.g. via `.wait()`) before
// the sink returns. Captures of stack-local references in any nested
// continuations whose escape route is the outer lambda's `return`
// therefore cannot dangle.
//
// This is the symmetric counterpart to `kBuiltinAsyncSinks`: the
// async-sink list says "this call defers the lambda" (lifetime escapes);
// the sync-sink list says "this call wraps the lambda in `.wait()`"
// (lifetime bounded by the call's own activation). Both lists are
// intentionally narrow -- unknown callees default to "escapes" to err on
// the side of false positives.
//
// Treated as suffix-qualified names (see `endsWithQualified`).
static const llvm::StringRef kBuiltinSynchronousSinks[] = {
  // workerd test harness: synchronously runs the callback in an
  // IoContext and `.wait()`s on the returned promise before returning.
  // See `deps/workerd/src/workerd/tests/test-fixture.h`.
  "workerd::TestFixture::runInIoContext",
  // KJ cross-thread synchronous dispatch. Schedules `func()` on the
  // target executor thread, blocks the calling thread, and if `func()`
  // returns a Promise, waits for it to resolve before returning.
  // See `deps/capnproto/c++/src/kj/async.h` (declaration) and
  // `async-inl.h` (definition).
  "kj::Executor::executeSync",
  // Note: project-specific synchronous sinks (e.g.
  // edgeworker::api::TestFixture::runInIoContext) should be configured
  // via the SynchronousSinks option in .clang-tidy.
};

static bool isSynchronousSink(
    llvm::StringRef qualifiedName, const std::vector<std::string> &extras) {
  for (auto suffix: kBuiltinSynchronousSinks) {
    if (endsWithQualified(qualifiedName, suffix)) return true;
  }
  for (const auto &extra: extras) {
    if (endsWithQualified(qualifiedName, extra)) return true;
  }
  return false;
}

// ---- Result-owning-sink callee identification -----------------------------
//
// Lambdas passed to one of these callees have their *return value* owned by
// the promise (or container) that the call itself produces: the callee
// either runs the lambda synchronously and wraps its result (`kj::evalNow`,
// `KJ_MAP`), or adopts the promise the lambda returns into the chain it
// returns (`kj::Promise::then`, `kj::evalLater`). A promise returned from
// such a lambda therefore lives no longer than the call's result, and its
// escape is decided by where that result goes.
//
// Treated as suffix-qualified names (see `endsWithQualified`).
static const llvm::StringRef kBuiltinResultOwningSinks[] = {
  "kj::Promise::then",
  "kj::Promise::catch_",
  "kj::evalLater",
  "kj::evalLast",
  "kj::evalNow",
  "kj::retryOnDisconnect",
  // `KJ_MAP(x, container) { ... }` expands to `Mapper(container) * lambda`.
  "kj::_::Mapper::operator*",
};

static bool isResultOwningSink(
    llvm::StringRef qualifiedName, const std::vector<std::string> &extras) {
  for (auto suffix: kBuiltinResultOwningSinks) {
    if (endsWithQualified(qualifiedName, suffix)) return true;
  }
  for (const auto &extra: extras) {
    if (endsWithQualified(qualifiedName, extra)) return true;
  }
  return false;
}

// Member functions of these classes take a promise argument and return a
// promise that owns it, so the argument lives exactly as long as the result.
static const llvm::StringRef kPromiseOwningMethods[] = {
  "kj::Timer::timeoutAfter",
  "kj::Timer::timeoutAt",
  "kj::Canceler::wrap",
  // Returns `canceler.wrap(promise)` or `promise` itself.
  "WorkerEntrypoint::wrapWithCanceler",
};

static bool isPromiseOwningMethod(llvm::StringRef qualifiedName) {
  for (auto suffix: kPromiseOwningMethods) {
    if (endsWithQualified(qualifiedName, suffix)) return true;
  }
  return false;
}

// Qualified name of a function, with the template arguments of an enclosing
// class-template specialization (or of a function-template specialization)
// dropped, so that `kj::Promise<int>::then` compares equal to
// `kj::Promise::then`.
static std::string stripTemplateArgs(const NamedDecl *nd) {
  if (!nd) return "";
  if (const auto *md = dyn_cast<CXXMethodDecl>(nd)) {
    if (const auto *spec = dyn_cast<ClassTemplateSpecializationDecl>(md->getParent())) {
      if (const auto *td = spec->getSpecializedTemplate()) {
        return td->getQualifiedNameAsString() + "::" + md->getNameAsString();
      }
    }
  }
  if (const auto *fd = dyn_cast<FunctionDecl>(nd)) {
    if (fd->isFunctionTemplateSpecialization()) {
      if (const auto *ftd = fd->getPrimaryTemplate()) {
        return ftd->getQualifiedNameAsString();
      }
    }
  }
  return nd->getQualifiedNameAsString();
}

static std::string calleeQualifiedName(const CallExpr *ce) {
  if (const auto *mce = dyn_cast<CXXMemberCallExpr>(ce)) {
    if (const auto *md = mce->getMethodDecl()) return stripTemplateArgs(md);
  }
  if (const auto *fd = ce->getDirectCallee()) return stripTemplateArgs(fd);
  return "";
}

// Returns true iff `fd` is the call operator of a lambda closure type.
static bool isLambdaCallOperator(const FunctionDecl *fd) {
  const auto *md = dyn_cast_or_null<CXXMethodDecl>(fd);
  return md && md->getParent() && md->getParent()->isLambda();
}

// Locate the FunctionDecl that most closely encloses a statement.
static const FunctionDecl *enclosingFunctionOfStmt(const Stmt *s, ASTContext &Ctx) {
  auto parents = Ctx.getParents(*s);
  while (!parents.empty()) {
    if (const auto *fd = parents[0].get<FunctionDecl>()) return fd;
    if (const auto *st = parents[0].get<Stmt>()) {
      parents = Ctx.getParents(*st);
      continue;
    }
    if (const auto *dd = parents[0].get<Decl>()) {
      parents = Ctx.getParents(*dd);
      continue;
    }
    break;
  }
  return nullptr;
}

// Given a lambda's call operator, return its `LambdaExpr`.
static const LambdaExpr *lambdaExprOf(const CXXMethodDecl *callOperator, ASTContext &Ctx) {
  auto parents = Ctx.getParents(*callOperator->getParent());
  for (int hops = 0; !parents.empty() && hops < 8; ++hops) {
    if (const auto *le = parents[0].get<LambdaExpr>()) return le;
    if (const auto *st = parents[0].get<Stmt>()) {
      parents = Ctx.getParents(*st);
      continue;
    }
    if (const auto *dd = parents[0].get<Decl>()) {
      parents = Ctx.getParents(*dd);
      continue;
    }
    break;
  }
  return nullptr;
}

// The nearest enclosing function of `s` that is not a lambda call operator.
static const FunctionDecl *enclosingNonLambdaFunction(const Stmt *s, ASTContext &Ctx) {
  const FunctionDecl *fd = enclosingFunctionOfStmt(s, Ctx);
  for (int hops = 0; fd && isLambdaCallOperator(fd) && hops < 16; ++hops) {
    const LambdaExpr *le = lambdaExprOf(cast<CXXMethodDecl>(fd), Ctx);
    fd = le ? enclosingFunctionOfStmt(le, Ctx) : nullptr;
  }
  return fd;
}

// Where a `return` statement sends its value.
struct ReturnTarget {
  // The return is from a lambda's call operator (rather than from a named
  // function).
  bool fromLambda = false;
  // When `fromLambda`, the call that takes the lambda as a direct argument
  // (looking through transparent wrappers), if there is one.
  const CallExpr *lambdaSinkCall = nullptr;
  // When `fromLambda`, the local variable the lambda initializes, if any.
  const VarDecl *lambdaVar = nullptr;
  // When `fromLambda`, the lambda itself, if found.
  const LambdaExpr *lambda = nullptr;
};

static ReturnTarget returnTargetOf(const Stmt *rs, ASTContext &Ctx) {
  ReturnTarget out;
  const FunctionDecl *enclosingFn = enclosingFunctionOfStmt(rs, Ctx);
  if (!enclosingFn || !isLambdaCallOperator(enclosingFn)) return out;
  out.fromLambda = true;
  const LambdaExpr *lambdaExpr = lambdaExprOf(cast<CXXMethodDecl>(enclosingFn), Ctx);
  if (!lambdaExpr) return out;
  out.lambda = lambdaExpr;
  // Walk parents of the LambdaExpr through transparent wrappers until we
  // find a CallExpr that takes the lambda as a direct argument.
  const Stmt *current = lambdaExpr;
  for (int hops = 0; hops < 16; ++hops) {
    auto parents = Ctx.getParents(*current);
    if (parents.empty()) return out;
    const Stmt *parent = parents[0].get<Stmt>();
    if (!parent) {
      if (const auto *vd = parents[0].get<VarDecl>()) {
        if (vd->hasLocalStorage() && !isa<ParmVarDecl>(vd)) out.lambdaVar = vd;
      }
      return out;
    }
    if (isa<ParenExpr>(parent) || isa<ImplicitCastExpr>(parent) || isa<ExprWithCleanups>(parent) ||
        isa<MaterializeTemporaryExpr>(parent) || isa<CXXBindTemporaryExpr>(parent)) {
      current = parent;
      continue;
    }
    if (const auto *cce = dyn_cast<CXXConstructExpr>(parent)) {
      if (cce->getNumArgs() == 1) {
        current = parent;
        continue;
      }
      return out;
    }
    if (const auto *ce = dyn_cast<CallExpr>(parent)) {
      for (const Expr *arg: ce->arguments()) {
        if (arg == current) {
          out.lambdaSinkCall = ce;
          break;
        }
      }
    }
    return out;
  }
  return out;
}

// ---- Owning-wrapper identification -----------------------------------------
//
// Captures whose static type is one of these owning wrappers (or a
// `kj::Maybe` of one) are treated as safe to capture by value. Captures by
// reference are *not* safe even for owning wrappers -- only the owned value
// itself transfers ownership into the continuation.
static const llvm::StringRef kOwningTemplates[] = {
  "kj::Own",
  "kj::Rc",
  "kj::Arc",
  "kj::Weak",
  "kj::WeakRc",
  "workerd::WeakRef",
  "workerd::AtomicWeakRef",
  "workerd::jsg::Ref",
  "workerd::jsg::V8Ref",
  "workerd::jsg::JsRef",
  "workerd::jsg::HashableV8Ref",
  "workerd::jsg::WeakRef",
  "workerd::IoOwn",
  "workerd::ReverseIoOwn",
  // IoPtr is debatable -- it is a non-owning *checked* view backed by
  // the IoContext. Treat it as safe: dereference is guarded.
  "workerd::IoPtr",
};

// Container templates whose ownership-safety is determined by their first
// type argument (e.g. `kj::Maybe<kj::Own<T>>` is safe, `kj::Maybe<T&>` is
// not).
static const llvm::StringRef kTransparentContainers[] = {
  "kj::Maybe",
  "kj::Array",
  "kj::Vector",
  "std::optional",
  "std::vector",
};

// Non-owning view type names. These are matched against the qualified
// name of either the underlying class template (for templates like
// `kj::ArrayPtr<T>`) or the RecordDecl (for non-template classes like
// `kj::StringPtr`). The two paths are checked separately in
// `isNonOwningViewType`; an entry only needs to appear once and will be
// matched by whichever path applies to the concrete type.
static const llvm::StringRef kNonOwningViewNames[] = {
  "kj::ArrayPtr",      // template
  "kj::StringPtr",     // non-template class
  "std::span",         // template
  "std::string_view",  // non-template (typedef of basic_string_view)
  // kj::FunctionParam<Sig>: explicitly documented as "MUST NOT outlive
  // the call". Capturing one into a long-lived continuation is exactly
  // the documented anti-use.
  "kj::FunctionParam",  // template
};

// "Stable singleton" types: instances are owned by the V8 isolate or some
// other long-lived singleton, and a captured reference / pointer cannot
// dangle for the lifetime of a continuation running on that isolate.
// Capturing these by reference is therefore safe by convention.
static const llvm::StringRef kStableSingletonNames[] = {
  // jsg::TypeHandler<T>: thin pointer into the isolate's TypeWrapper,
  // stable for the lifetime of the isolate.
  "workerd::jsg::TypeHandler",
  // workerd::api::CapnpTypeWrapperBase: base class of the isolate's
  // TypeWrapper used by the capnp bindings; the instance is owned by
  // the isolate and outlives any continuation that runs on it.
  "workerd::api::CapnpTypeWrapperBase",
};

// ---- Type classification ---------------------------------------------------

static std::string templateQualifiedName(QualType qt) {
  qt = qt.getNonReferenceType().getUnqualifiedType().getCanonicalType();
  const Type *t = qt.getTypePtrOrNull();
  if (!t) return "";
  if (const auto *tst = t->getAs<TemplateSpecializationType>()) {
    if (auto *td = tst->getTemplateName().getAsTemplateDecl())
      return td->getQualifiedNameAsString();
  }
  // Look through CXXRecordDecl that is a ClassTemplateSpecializationDecl.
  if (const auto *rt = t->getAs<RecordType>()) {
    if (const auto *spec = dyn_cast<ClassTemplateSpecializationDecl>(rt->getDecl())) {
      return spec->getSpecializedTemplate()->getQualifiedNameAsString();
    }
  }
  return "";
}

static std::string recordQualifiedName(QualType qt) {
  qt = qt.getNonReferenceType().getUnqualifiedType().getCanonicalType();
  const Type *t = qt.getTypePtrOrNull();
  if (!t) return "";
  if (const auto *rt = t->getAs<RecordType>()) {
    return rt->getDecl()->getQualifiedNameAsString();
  }
  return "";
}

// Returns the first template argument of `qt` as a QualType, or a null
// QualType if unavailable.
static QualType firstTemplateArg(QualType qt) {
  qt = qt.getNonReferenceType().getUnqualifiedType().getCanonicalType();
  const Type *t = qt.getTypePtrOrNull();
  if (!t) return {};
  if (const auto *tst = t->getAs<TemplateSpecializationType>()) {
    if (tst->template_arguments().size() > 0) {
      const auto &arg = tst->template_arguments()[0];
      if (arg.getKind() == TemplateArgument::Type) return arg.getAsType();
    }
  }
  if (const auto *rt = t->getAs<RecordType>()) {
    if (const auto *spec = dyn_cast<ClassTemplateSpecializationDecl>(rt->getDecl())) {
      const auto &args = spec->getTemplateArgs();
      if (args.size() > 0 && args[0].getKind() == TemplateArgument::Type)
        return args[0].getAsType();
    }
  }
  return {};
}

enum class CaptureSafety {
  Safe,
  UnsafeBareReference,
  UnsafeRawPointer,
  UnsafeNonOwningView,
  UnsafeThis,
};

// Classify the *value type* that the lambda member will hold for this
// capture. By-reference captures store a reference; by-value captures store
// a copy.
static bool isOwningType(QualType qt, const std::vector<std::string> &extraOwning, int depth = 0) {
  if (depth > 4) return false;  // bail on deeply nested templates
  qt = qt.getNonReferenceType().getUnqualifiedType().getCanonicalType();
  if (qt.isNull()) return false;

  std::string tmpl = templateQualifiedName(qt);
  if (!tmpl.empty()) {
    for (auto suffix: kOwningTemplates) {
      if (endsWithQualified(tmpl, suffix)) return true;
    }
    for (const auto &extra: extraOwning) {
      if (endsWithQualified(tmpl, extra)) return true;
    }
    for (auto suffix: kTransparentContainers) {
      if (endsWithQualified(tmpl, suffix)) {
        QualType inner = firstTemplateArg(qt);
        if (!inner.isNull()) return isOwningType(inner, extraOwning, depth + 1);
        return false;
      }
    }
  }

  return false;
}

static bool isNonOwningViewType(QualType qt) {
  qt = qt.getNonReferenceType().getUnqualifiedType().getCanonicalType();
  if (qt.isNull()) return false;
  // Try the underlying template name first (matches `kj::ArrayPtr<T>`,
  // `std::span<T>`, etc.), then fall back to the RecordDecl qualified
  // name (matches non-template classes like `kj::StringPtr`). The same
  // list is consulted for both paths -- only one path will produce a
  // non-empty string for any given type.
  std::string tmpl = templateQualifiedName(qt);
  if (!tmpl.empty()) {
    for (auto suffix: kNonOwningViewNames) {
      if (endsWithQualified(tmpl, suffix)) return true;
    }
  }
  std::string rec = recordQualifiedName(qt);
  if (!rec.empty()) {
    for (auto suffix: kNonOwningViewNames) {
      if (endsWithQualified(rec, suffix)) return true;
    }
  }
  return false;
}

// Returns true if `qt` is a "stable singleton" type whose instances are
// owned by the V8 isolate or some other long-lived singleton and cannot
// dangle relative to a continuation running on that isolate. References
// to such types are therefore safe to capture into continuations.
static bool isStableSingletonType(QualType qt) {
  qt = qt.getNonReferenceType().getUnqualifiedType().getCanonicalType();
  if (qt.isNull()) return false;
  std::string tmpl = templateQualifiedName(qt);
  if (!tmpl.empty()) {
    for (auto suffix: kStableSingletonNames) {
      if (endsWithQualified(tmpl, suffix)) return true;
    }
  }
  std::string rec = recordQualifiedName(qt);
  if (!rec.empty()) {
    for (auto suffix: kStableSingletonNames) {
      if (endsWithQualified(rec, suffix)) return true;
    }
  }
  return false;
}

// Returns true if `qt` is a "trivial value handle" that is cheap and safe
// to copy and contains no dangling references. We bias toward false here
// (be conservative); we mainly use this to whitelist scalars, enums, and
// well-known POD-ish types.
static bool isTriviallySafeValue(QualType qt) {
  qt = qt.getNonReferenceType().getUnqualifiedType().getCanonicalType();
  if (qt.isNull()) return false;
  if (qt->isPointerType()) return false;    // raw pointer -- unsafe
  if (qt->isReferenceType()) return false;  // shouldn't happen after stripping
  if (qt->isIntegerType() || qt->isFloatingType() || qt->isEnumeralType() || qt->isBooleanType())
    return true;
  // Well-known small value types.
  std::string rec = recordQualifiedName(qt);
  static const llvm::StringRef kTrivialRecords[] = {
    "kj::Date",
    "kj::Duration",
    "kj::String",  // owns its buffer
    "kj::Exception",
  };
  for (auto suffix: kTrivialRecords) {
    if (endsWithQualified(rec, suffix)) return true;
  }
  return false;
}

static CaptureSafety classifyCapture(
    const LambdaCapture &cap, QualType fieldTy, const std::vector<std::string> &extraOwning) {
  if (cap.capturesThis()) {
    return CaptureSafety::UnsafeThis;
  }

  // If the capture is by reference (either explicit `[&x]` or implicit
  // `[&]`), the lambda stores a reference. Generally unsafe regardless of
  // what is being referred to -- the referent's lifetime is what we cannot
  // verify locally. (Owning a `kj::Own<T>&` does not transfer ownership.)
  //
  // Exception: "stable singleton" types (e.g. `jsg::TypeHandler`) are
  // owned by the isolate's TypeWrapper and outlive any continuation that
  // runs on the same isolate; references to them are safe to capture.
  if (cap.getCaptureKind() == LCK_ByRef) {
    if (cap.capturesVariable()) {
      if (const auto *vd = cap.getCapturedVar()) {
        if (isStableSingletonType(vd->getType())) return CaptureSafety::Safe;
      }
    }
    return CaptureSafety::UnsafeBareReference;
  }

  // By-value capture (LCK_ByCopy or init-capture). The captured value is
  // a copy of whatever expression. Look at the resulting field type.
  QualType t = fieldTy.getNonReferenceType().getCanonicalType();
  if (t.isNull()) return CaptureSafety::Safe;  // give up safely

  // A by-value capture of a `T&` field shouldn't normally happen, but in
  // some clang versions the VarDecl field carries the reference type for
  // reference captures. Already handled above.

  // Raw pointer captured by value -- the pointer itself is safe to copy,
  // but the pointee lifetime is unknown. Flag.
  if (t->isPointerType()) {
    // Function pointers are fine.
    QualType pointee = t->getPointeeType();
    if (pointee->isFunctionType()) return CaptureSafety::Safe;
    return CaptureSafety::UnsafeRawPointer;
  }

  if (isOwningType(t, extraOwning)) return CaptureSafety::Safe;

  if (isNonOwningViewType(t)) return CaptureSafety::UnsafeNonOwningView;

  if (isTriviallySafeValue(t)) return CaptureSafety::Safe;

  // Unknown type -- be permissive. A future refinement could flag
  // captures of structs that contain non-owning fields, but that requires
  // a transitive scan and is high-noise.
  return CaptureSafety::Safe;
}

static const char *safetyLabel(CaptureSafety s) {
  switch (s) {
    case CaptureSafety::UnsafeBareReference:
      return "by-reference";
    case CaptureSafety::UnsafeRawPointer:
      return "raw pointer";
    case CaptureSafety::UnsafeNonOwningView:
      return "non-owning view";
    case CaptureSafety::UnsafeThis:
      return "this";
    case CaptureSafety::Safe:
      return "";
  }
  return "";
}

static const char *safetyAdvice(CaptureSafety s) {
  switch (s) {
    case CaptureSafety::UnsafeBareReference:
      return "transfer ownership (kj::Own / kj::Rc / jsg::Ref) or capture a "
             "kj::Weak / kj::WeakRc / workerd::WeakRef / jsg::WeakRef";
    case CaptureSafety::UnsafeRawPointer:
      return "capture an owning smart pointer (kj::Own / kj::Rc) instead";
    case CaptureSafety::UnsafeNonOwningView:
      return "materialize the data into a kj::String / kj::Array before "
             "capturing, or capture the owning source";
    case CaptureSafety::UnsafeThis:
      return "use JSG_THIS (jsg::Ref<Self>) for JSG resource types, "
             "kj::addRef(*this) for kj::Refcounted, an IoOwn<Self>, or a "
             "weak handle (addWeakToThis() / Rc::downgrade())";
    case CaptureSafety::Safe:
      return "";
  }
  return "";
}

// ---- Escape analysis -------------------------------------------------------
//
// `promiseEscapes(C, Context)` answers the question: "does the value
// produced by `C` (the continuation-creating CallExpr, e.g.
// `promise.then(lambda)`) outlive the current function activation?"
//
// If the result is locally consumed -- via `.wait()`/`.poll()`, `co_await`,
// or eventually as the operand of one of those -- then the lambda's
// captures are safe regardless of whether they refer to local stack
// variables, because the local variables (and, in a coroutine, the
// coroutine frame) remain alive while we wait. We can suppress the
// diagnostic.
//
// If the result is returned, stored in a member, passed to an unknown
// function (likely a task-set add), etc., the lambda escapes the current
// function activation, the locals may go out of scope before the lambda
// runs, and the diagnostic should fire.
//
// This is a deliberately syntactic analysis: it walks parent expressions
// and, when the call's result is bound to a local `VarDecl`, scans
// later uses of that variable. It does not attempt alias analysis. When
// we can't classify a use we bias toward "escapes" so that the diagnostic
// fires (false-negative-averse).

// Strip "transparent" expression wrappers (paren, casts, materialize-temp,
// bind-temporary, ExprWithCleanups, CXXConstructExpr that just copies a
// promise, etc.) without changing the answer.
static const Stmt *peelWrappers(const Stmt *s) {
  while (s) {
    if (const auto *e = dyn_cast<Expr>(s)) {
      const Expr *peeled = e->IgnoreImplicit()->IgnoreParens();
      if (peeled != e) {
        s = peeled;
        continue;
      }
    }
    if (const auto *ewc = dyn_cast<ExprWithCleanups>(s)) {
      s = ewc->getSubExpr();
      continue;
    }
    if (const auto *mte = dyn_cast<MaterializeTemporaryExpr>(s)) {
      s = mte->getSubExpr();
      continue;
    }
    if (const auto *bte = dyn_cast<CXXBindTemporaryExpr>(s)) {
      s = bte->getSubExpr();
      continue;
    }
    // CXXConstructExpr that constructs a promise from a single argument
    // (move-construction). Pass through.
    if (const auto *cce = dyn_cast<CXXConstructExpr>(s)) {
      if (cce->getNumArgs() == 1) {
        s = cce->getArg(0);
        continue;
      }
    }
    break;
  }
  return s;
}

// Safely retrieve a NamedDecl's name as a StringRef. Returns empty if the
// decl's name isn't a simple identifier (e.g. an operator, conversion
// function, constructor, destructor, ...). NamedDecl::getName() asserts
// in debug builds in those cases.
static llvm::StringRef safeGetName(const NamedDecl *nd) {
  if (!nd) return {};
  if (const auto *id = nd->getIdentifier()) return id->getName();
  return {};
}

// Methods on a promise (or `co_await`-able) that consume it synchronously
// in the current activation. The argument promise's lambda captures
// therefore cannot dangle.
static bool isSynchronousConsumerMethod(llvm::StringRef name) {
  return name == "wait" || name == "poll" || name == "waitForResult";
}

// Methods on a promise that simply transform it and produce another
// promise whose escape semantics determine the original's. The original
// captures are safe iff the *resulting* promise is locally consumed.
//
// Called both when the promise is the *receiver* of the method
// (`p.then(...)`) and when it is an *argument* to a binary combinator
// (`q.exclusiveJoin(kj::mv(p))`). In the latter case the resulting call
// inherits the argument's lifetime as well, so the same passthrough
// reasoning applies.
static bool isPromisePassThroughMethod(llvm::StringRef name) {
  // .then() chains: nested continuations -- the outer call's destination
  // tells us whether the chain escapes. (The inner .then's own lambda
  // will be visited separately by the matcher.)
  // .attach(), .eagerlyEvaluate(), .catch_(), .ignoreResult(),
  // .exclusiveJoin(): all return a new promise.
  return name == "then" || name == "catch_" || name == "attach" || name == "eagerlyEvaluate" ||
      name == "ignoreResult" || name == "exclusiveJoin" || name == "addBranch" ||
      name == "markAsHandled" ||
      // .fork() turns a Promise into a ForkedPromise. Lifetime of
      // the underlying chain is the lifetime of the resulting
      // ForkedPromise.
      name == "fork";
}

// Free functions that consume one or more promises and return a
// combined promise. When `current` is an argument to one of these, the
// resulting call inherits the lifetime of all its arguments.
static bool isPromiseCombinatorFunction(llvm::StringRef name) {
  return name == "joinPromises" || name == "joinPromisesFailFast" || name == "race";
}

// Free functions that build an array (or array-like) from their arguments,
// e.g. `kj::arr(a, b, ...)`. The resulting array carries the lifetime of all
// its elements, so when a promise is an argument the resulting array's escape
// determines the promise's fate -- treat as pass-through.
static bool isArrayConstructorFunction(llvm::StringRef name) {
  return name == "arr" || name == "arrOf";
}

// Methods that insert a value into a container without the container itself
// escaping, e.g. `kj::Vector<Promise>::add`, `kj::ArrayBuilder<Promise>::add`.
// When a continuation is passed as an *argument* to one of these, its lifetime
// is bounded by the (local) container's lifetime, which we then analyze via the
// receiver. When called on the container *as receiver*, the insertion does not
// itself cause the container to escape.
static bool isContainerInsertMethod(llvm::StringRef name) {
  return name == "add" || name == "addAll" || name == "addFront";
}

// Methods that finalize a promise container into an array/pointer whose
// lifetime carries the contained promises, e.g. `kj::ArrayBuilder::finish`,
// `kj::Vector::releaseAsArray`/`asArray`/`asPtr`. The result's escape determines
// the contained continuations' fate, so treat as pass-through.
static bool isContainerFinalizeMethod(llvm::StringRef name) {
  return name == "finish" || name == "releaseAsArray" || name == "asArray" || name == "asPtr";
}

// Const query methods on a container that neither move the container nor its
// contents anywhere -- e.g. `kj::Vector::empty`/`size`/`capacity`. A call to one
// of these (with the container as receiver) does not cause the container to
// escape, so the use is local.
static bool isContainerQueryMethod(llvm::StringRef name) {
  // `onEmpty()` / `isEmpty()` are `kj::TaskSet` queries; the promise
  // `onEmpty()` returns does not own the tasks.
  return name == "empty" || name == "size" || name == "capacity" || name == "onEmpty" ||
      name == "isEmpty";
}

// ---- Downstream `.attach(...)` lifetime binding ----------------------------
//
// A continuation chain may bind referents to its own lifetime via
// `.attach(<owning-expr>...)`. Captures of those referents in the chain's
// lambda are therefore safe regardless of where the chain itself ends up.
// We record the bound names as either "this" or a specific `VarDecl*`.

struct BoundNames {
  bool boundThis = false;
  llvm::SmallVector<const VarDecl *, 4> boundVars;
  // For member-access bindings like `kj::mv(paf.fulfiller)`, record the
  // (base-var, member-decl) pair. We match captures whose init
  // expression dereferences the same member of the same base variable.
  llvm::SmallVector<std::pair<const VarDecl *, const ValueDecl *>, 4> boundMembers;
};

// Strip outer wrappers (kj::mv / std::move / kj::addRef / IgnoreImplicit /
// CXXConstructExpr-of-one-arg) and return the inner expression and a
// flag indicating whether `kj::addRef` was peeled. `*this` is detected
// after the peel.
static const Expr *peelAttachArg(const Expr *e, bool &sawAddRef) {
  sawAddRef = false;
  if (!e) return nullptr;
  e = e->IgnoreImplicit()->IgnoreParens();
  while (true) {
    if (const auto *ce = dyn_cast<CallExpr>(e)) {
      if (const auto *fd = ce->getDirectCallee()) {
        llvm::StringRef name = safeGetName(fd);
        if (name == "mv" || name == "move" || name == "cp" || name == "fwd" || name == "forward") {
          if (ce->getNumArgs() == 1) {
            e = ce->getArg(0)->IgnoreImplicit()->IgnoreParens();
            continue;
          }
        }
        if (name == "addRef") {
          sawAddRef = true;
          if (ce->getNumArgs() == 1) {
            e = ce->getArg(0)->IgnoreImplicit()->IgnoreParens();
            continue;
          }
        }
      }
    }
    if (const auto *cce = dyn_cast<CXXConstructExpr>(e)) {
      if (cce->getNumArgs() == 1) {
        e = cce->getArg(0)->IgnoreImplicit()->IgnoreParens();
        continue;
      }
    }
    if (const auto *uo = dyn_cast<UnaryOperator>(e)) {
      // `*this` or `*ptr`.
      if (uo->getOpcode() == UO_Deref) {
        e = uo->getSubExpr()->IgnoreImplicit()->IgnoreParens();
        continue;
      }
    }
    break;
  }
  return e;
}

// Add the bindings introduced by a single `.attach(arg1, arg2, ...)`
// call to `out`.
static void collectAttachBindings(const CXXMemberCallExpr *attachCall, BoundNames &out) {
  for (unsigned i = 0; i < attachCall->getNumArgs(); ++i) {
    bool sawAddRef = false;
    const Expr *peeled = peelAttachArg(attachCall->getArg(i), sawAddRef);
    if (!peeled) continue;
    // `*this` or `this` (after deref-peel).
    if (isa<CXXThisExpr>(peeled)) {
      out.boundThis = true;
      continue;
    }
    // `<varname>` — DeclRefExpr to a local/parameter VarDecl.
    if (const auto *dre = dyn_cast<DeclRefExpr>(peeled)) {
      if (const auto *vd = dyn_cast<VarDecl>(dre->getDecl())) {
        out.boundVars.push_back(vd);
      }
      continue;
    }
    // `base.member` or `base->member` — MemberExpr. Record the
    // (base-VarDecl, member-ValueDecl) pair so captures of the same
    // member can be matched.
    if (const auto *me = dyn_cast<MemberExpr>(peeled)) {
      const Expr *baseE = me->getBase()->IgnoreImplicit()->IgnoreParens();
      if (const auto *baseDre = dyn_cast<DeclRefExpr>(baseE)) {
        if (const auto *baseVd = dyn_cast<VarDecl>(baseDre->getDecl())) {
          out.boundMembers.push_back({baseVd, me->getMemberDecl()});
        }
      }
    }
  }
}

// Walk forward (through parents) from `startCall` along the same promise
// chain, collecting bindings introduced by every `.attach(...)` call
// encountered. The walk stops when we leave the chain (e.g. the parent is
// no longer a passthrough method-call).
static BoundNames collectChainAttachBindings(const CallExpr *startCall, ASTContext &Ctx) {
  BoundNames out;
  const Stmt *current = startCall;
  for (int hops = 0; hops < 32; ++hops) {
    auto parents = Ctx.getParents(*current);
    if (parents.empty()) break;
    const Stmt *parent = parents[0].get<Stmt>();
    if (!parent) break;
    // Look through wrappers.
    if (isa<ParenExpr>(parent) || isa<ImplicitCastExpr>(parent) || isa<ExprWithCleanups>(parent) ||
        isa<MaterializeTemporaryExpr>(parent) || isa<CXXBindTemporaryExpr>(parent)) {
      current = parent;
      continue;
    }
    // MemberExpr -> grandparent should be the CXXMemberCallExpr.
    if (isa<MemberExpr>(parent)) {
      auto grandparents = Ctx.getParents(*parent);
      if (grandparents.empty()) break;
      const auto *mce = grandparents[0].get<CXXMemberCallExpr>();
      if (!mce) break;
      if (const auto *md = mce->getMethodDecl()) {
        llvm::StringRef name = safeGetName(md);
        if (name == "attach") {
          collectAttachBindings(mce, out);
        }
        if (isPromisePassThroughMethod(name)) {
          // Stay on the chain.
          current = mce;
          continue;
        }
      }
      // Leaves the chain.
      break;
    }
    // Direct CXXMemberCallExpr ancestor without a MemberExpr (rare).
    if (const auto *mce = dyn_cast<CXXMemberCallExpr>(parent)) {
      if (const auto *md = mce->getMethodDecl()) {
        llvm::StringRef name = safeGetName(md);
        if (name == "attach") {
          collectAttachBindings(mce, out);
        }
        if (isPromisePassThroughMethod(name)) {
          current = mce;
          continue;
        }
      }
      break;
    }
    break;
  }
  return out;
}

// Use the file-scope FunctionUseMap / FunctionUseMapBuilder /
// getOrBuildFunctionUseMap defined just below the anonymous namespace.
// (Forward declarations here are not needed because all references to
// them go through `getOrBuildFunctionUseMap`, which has its own forward
// declaration at file scope.)

// A lambda whose `return` the escape analysis followed a chain through. Its
// parameters and locals die when it returns, whatever then owns the chain.
struct CrossedLambda {
  const LambdaExpr *lambda;
  // The closure, and so its by-copy captures, dies with the return too:
  // a result-owning sink drops the closure once it has run (kj's
  // ChainPromiseNode destroys the `then` node before adopting the returned
  // chain). A synchronous sink keeps the closure, and a local variable keeps
  // it until the enclosing function returns.
  bool closureDies;
};

// The configured sink lists consulted by the escape analysis.
struct SinkLists {
  const std::vector<std::string> &async;
  const std::vector<std::string> &sync;
  const std::vector<std::string> &resultOwning;
  // Out-parameter: every lambda the analysis returned through, on any path.
  std::vector<CrossedLambda> *crossed = nullptr;
};

enum class Escape {
  // The continuation is consumed in the current function activation.
  // Captures are safe.
  Local,
  // The continuation outlives the current function activation. Captures
  // must be owning.
  Escapes,
  // The continuation is stored as a non-static, non-ForkedPromise member
  // field of the same class whose `this` is captured (or could be
  // captured). The object's destructor will cancel the chain before any
  // other field destructor runs, so `[this]` is safe -- but other
  // captures still escape past the chain's invocation point.
  StoredAsSelfMember,
  // The continuation is the return value of the nearest enclosing function
  // that is not a lambda. It outlives the activation, but no longer than
  // the promise that function returns, which its reference parameters
  // outlive under the promise parameter contract.
  Returned,
};

// Forward decl.
static Escape classifyExprUse(const Expr *e, ASTContext &Ctx, int depth, const SinkLists &sinks);

// Forward decl.
static Escape classifyVarDeclUses(const VarDecl *vd,
    const FunctionDecl *enclosingFn,
    ASTContext &Ctx,
    int depth,
    const SinkLists &sinks);

// Locate the FunctionDecl enclosing a given Decl by walking parents.
static const FunctionDecl *enclosingFunctionOf(const Decl *d, ASTContext &Ctx) {
  if (!d) return nullptr;
  auto parents = Ctx.getParents(*d);
  while (!parents.empty()) {
    if (const auto *fd = parents[0].get<FunctionDecl>()) return fd;
    if (const auto *dd = parents[0].get<Decl>()) {
      parents = Ctx.getParents(*dd);
      continue;
    }
    if (const auto *s = parents[0].get<Stmt>()) {
      parents = Ctx.getParents(*s);
      continue;
    }
    break;
  }
  return nullptr;
}

// If `current` is the receiver of a (local) promise-container method call,
// classify that use: container-finalize methods (finish/releaseAsArray/...)
// pass through to the resulting array; container-insert methods (add/...) do
// not make the container escape. Returns true and sets `result`/`next`
// accordingly when handled. `next` non-null means "continue the walk with
// `next` as the new current".
static bool classifyContainerReceiverMethod(
    llvm::StringRef name, const CXXMemberCallExpr *mce, Escape &result, const Stmt *&next) {
  next = nullptr;
  if (isContainerFinalizeMethod(name)) {
    next = mce;  // result array carries the contained promises' lifetime
    return true;
  }
  if (isContainerInsertMethod(name) || isContainerQueryMethod(name)) {
    // Inserting into / querying the container (as receiver) does not itself
    // cause the container to escape; this use is local.
    result = Escape::Local;
    return true;
  }
  return false;
}

// Walk uses of a local variable bound to a promise. Result is the
// "least-safe" of all use classifications:
//   any Escapes        -> Escapes
//   else any StoredAsSelfMember -> StoredAsSelfMember
//   else               -> Local
//
// Re-entrance guard: `promise = promise.exclusiveJoin(...)` produces a
// circular use graph -- classifying uses of `promise` walks back to the
// same assignment, which recurses into uses of `promise` again. Track
// the set of vars currently being analyzed; treat a re-entrant call as
// Local (we'll catch any escape via one of the other, non-circular uses
// in the outer call frame).
static Escape classifyVarDeclUses(const VarDecl *vd,
    const FunctionDecl *enclosingFn,
    ASTContext &Ctx,
    int depth,
    const SinkLists &sinks) {
  static thread_local llvm::SmallPtrSet<const VarDecl *, 8> inProgress;
  if (!enclosingFn || !enclosingFn->getBody()) return Escape::Escapes;
  if (!inProgress.insert(vd).second) {
    // Already analyzing this var elsewhere on the call stack; the
    // outer frame will decide.
    return Escape::Local;
  }
  // RAII: make sure `vd` is removed from `inProgress` on every exit
  // path, including exceptions thrown from anywhere below.
  // Manual RAII guard so we don't depend on llvm::scope_exit's evolving API.
  struct EraseOnExit {
    llvm::SmallPtrSet<const VarDecl *, 8> *set;
    const VarDecl *vd;
    ~EraseOnExit() {
      set->erase(vd);
    }
  } cleanup{&inProgress, vd};
  // Single-pass per-function use map: built once per enclosingFn and
  // cached, so analyzing N promise-bound variables in the same function
  // is O(|body|) rather than O(N * |body|).
  const FunctionUseMap &useMap = getOrBuildFunctionUseMap(enclosingFn);
  auto it = useMap.find(vd);
  if (it == useMap.end() || it->second.empty()) {
    return Escape::Local;  // dead store: no escape
  }
  bool sawStored = false;
  bool sawReturned = false;
  for (const auto *use: it->second) {
    Escape e = classifyExprUse(use, Ctx, depth + 1, sinks);
    if (e == Escape::Escapes) {
      return Escape::Escapes;
    }
    if (e == Escape::StoredAsSelfMember) sawStored = true;
    if (e == Escape::Returned) sawReturned = true;
  }
  // Each partial-escape kind only justifies its own captures, so a mix of
  // them justifies none.
  if (sawStored && sawReturned) return Escape::Escapes;
  if (sawStored) return Escape::StoredAsSelfMember;
  if (sawReturned) return Escape::Returned;
  return Escape::Local;
}

// A chain returned from a local lambda (`auto retry = [&](...) { return
// p.then(...); };`) goes wherever the lambda's call results go. Classify every
// use of `lambdaVar`: each must be a direct call, whose result is then
// classified like any other chain. Any other use (copying the lambda, passing
// it somewhere) lets the chain escape.
static Escape classifyLocalLambdaCalls(
    const VarDecl *lambdaVar, ASTContext &Ctx, int depth, const SinkLists &sinks) {
  const auto *fn = dyn_cast<FunctionDecl>(lambdaVar->getDeclContext());
  for (int hops = 0; fn && isLambdaCallOperator(fn) && hops < 16; ++hops) {
    fn = dyn_cast<FunctionDecl>(cast<CXXMethodDecl>(fn)->getParent()->getDeclContext());
  }
  if (!fn || !fn->getBody()) return Escape::Escapes;
  const FunctionUseMap &useMap = getOrBuildFunctionUseMap(fn);
  auto it = useMap.find(lambdaVar);
  if (it == useMap.end()) return Escape::Local;
  bool sawReturned = false;
  for (const auto *use: it->second) {
    const Stmt *callee = use;
    auto parents = Ctx.getParents(*callee);
    while (!parents.empty() && parents[0].get<ImplicitCastExpr>()) {
      callee = parents[0].get<ImplicitCastExpr>();
      parents = Ctx.getParents(*callee);
    }
    // A capture of the lambda by another lambda: that lambda's calls are
    // separate uses, and the capture itself is checked as a capture.
    if (!parents.empty() && parents[0].get<LambdaExpr>()) continue;
    const auto *call = parents.empty() ? nullptr : parents[0].get<CXXOperatorCallExpr>();
    if (!call || call->getOperator() != OO_Call || call->getArg(0) != callee) {
      return Escape::Escapes;
    }
    Escape e = classifyExprUse(call, Ctx, depth + 1, sinks);
    if (e == Escape::Returned) {
      sawReturned = true;
    } else if (e != Escape::Local) {
      return Escape::Escapes;
    }
  }
  return sawReturned ? Escape::Returned : Escape::Local;
}

// Walk parents of `e` to determine whether its value escapes the
// current function activation. `depth` guards against pathological
// recursion (cycles aren't possible in well-formed ASTs, but
// instantiation can produce deep parent chains).
static Escape classifyExprUse(const Expr *e, ASTContext &Ctx, int depth, const SinkLists &sinks) {
  if (depth > 32) return Escape::Escapes;
  if (!e) return Escape::Escapes;

  // Walk through any wrappers above `e` before consulting parents.
  // Parents queried via ParentMapContext skip implicit casts already in
  // many cases, but be defensive.
  const Stmt *current = e;
  while (true) {
    auto parents = Ctx.getParents(*current);
    if (parents.empty()) {
      // Top of TU or function body: result is discarded as a
      // full-expression statement -- safe (the promise destructor runs
      // here and either no-ops or blocks; either way the captures
      // outlive their lambda).
      return Escape::Local;
    }
    const Stmt *parent = parents[0].get<Stmt>();
    if (!parent) {
      // Parent is a Decl: most likely a VarDecl initializer or a
      // member initializer. Handle below.
      if (const auto *vd = parents[0].get<VarDecl>()) {
        // `auto x = continuation(...);`
        // x's lifetime is the enclosing scope (a local). Trace its
        // uses; if all uses are local, x is locally consumed.
        if (vd->isLocalVarDecl()) {
          // Locate enclosing function.
          auto fnParents = Ctx.getParents(*vd);
          const FunctionDecl *enclosingFn = nullptr;
          while (!fnParents.empty()) {
            if (const auto *fd = fnParents[0].get<FunctionDecl>()) {
              enclosingFn = fd;
              break;
            }
            if (const auto *d = fnParents[0].get<Decl>()) {
              fnParents = Ctx.getParents(*d);
              continue;
            }
            if (const auto *s = fnParents[0].get<Stmt>()) {
              fnParents = Ctx.getParents(*s);
              continue;
            }
            break;
          }
          return classifyVarDeclUses(vd, enclosingFn, Ctx, depth + 1, sinks);
        }
        // Non-local VarDecl (e.g. static/global/member). Escapes.
        return Escape::Escapes;
      }
      // CXXCtorInitializer: `C() : member(<expr>) {}`. The expression
      // initializes a member of the class being constructed. The
      // member's destructor (which runs when `*this` is destroyed) will
      // cancel the underlying chain. Note: even for kj::ForkedPromise
      // fields, the captured `this` cannot dangle: the ForkedPromise
      // owns the chain, and when `*this` dies the ForkedPromise dies,
      // cancelling the chain. Outstanding branches resolve to
      // cancelled, not UAF.
      if (const auto *ci = parents[0].get<CXXCtorInitializer>()) {
        if (ci->isMemberInitializer()) return Escape::StoredAsSelfMember;
        return Escape::Escapes;
      }
      // ParentMapContext sometimes elides the CXXCtorInitializer node
      // and returns the enclosing CXXConstructorDecl directly. In that
      // case, walk the constructor's init list and check whether our
      // current expression is (transitively) the init of a member
      // initializer.
      if (const auto *ctor = parents[0].get<CXXConstructorDecl>()) {
        for (const auto *init: ctor->inits()) {
          if (!init->isMemberInitializer()) continue;
          const Expr *initExpr = init->getInit();
          if (!initExpr) continue;
          // Check whether our `current` is a sub-expression of initExpr.
          // We use a source-range containment check; for member inits
          // the init expression is the whole RHS of `member(expr)`.
          auto initRange = initExpr->getSourceRange();
          auto curRange = current->getSourceRange();
          auto &SM = Ctx.getSourceManager();
          if (SM.isBeforeInTranslationUnit(curRange.getBegin(), initRange.getBegin())) continue;
          if (SM.isBeforeInTranslationUnit(initRange.getEnd(), curRange.getEnd())) continue;
          // current is within initExpr's source range.
          return Escape::StoredAsSelfMember;
        }
        return Escape::Escapes;
      }
      // Field/member initializer or other Decl context. Conservative.
      return Escape::Escapes;
    }

    // Transparent wrappers: keep walking up.
    if (isa<ParenExpr>(parent) || isa<ImplicitCastExpr>(parent) || isa<ExprWithCleanups>(parent) ||
        isa<MaterializeTemporaryExpr>(parent) || isa<CXXBindTemporaryExpr>(parent)) {
      current = parent;
      continue;
    }

    // CXXConstructExpr that copies/moves a single argument: this is
    // typically a move-construct of the promise into a target slot
    // (function return, member init, parameter pass). Pass through to
    // the actual consumer.
    if (const auto *cce = dyn_cast<CXXConstructExpr>(parent)) {
      if (cce->getNumArgs() == 1) {
        current = parent;
        continue;
      }
    }

    // MemberExpr: we're the base of a `.foo` access. The MemberExpr is
    // just the callee node; its parent (a CXXMemberCallExpr) is the
    // actual call we want to inspect. Hop directly to that call and
    // bypass the receiver-vs-current check (we are the receiver by
    // construction).
    if (isa<MemberExpr>(parent)) {
      // Look at the MemberExpr's parent to find the call.
      auto grandparents = Ctx.getParents(*parent);
      if (!grandparents.empty()) {
        if (const auto *mce = grandparents[0].get<CXXMemberCallExpr>()) {
          if (const auto *md = mce->getMethodDecl()) {
            llvm::StringRef name = safeGetName(md);
            if (isSynchronousConsumerMethod(name)) {
              return Escape::Local;
            }
            if (isPromisePassThroughMethod(name)) {
              // Recurse: classify the *resulting* call's destination.
              current = mce;
              continue;
            }
            // Local promise-container method (e.g. `promises.finish()` or
            // `promises.add(...)` with `promises` as receiver).
            Escape containerResult = Escape::Escapes;
            const Stmt *containerNext = nullptr;
            if (classifyContainerReceiverMethod(name, mce, containerResult, containerNext)) {
              if (containerNext) {
                current = containerNext;
                continue;
              }
              return containerResult;
            }
          }
          // Unknown method on what looks like a promise -- escapes.
          return Escape::Escapes;
        }
      }
      // MemberExpr without a CXXMemberCallExpr parent: e.g., taking
      // the address of a member, which would be very unusual for a
      // promise. Be conservative.
      return Escape::Escapes;
    }

    // Member access whose callee could not be resolved yet because the call
    // is in a template (e.g. `promises.add(x)` inside a generic lambda, with
    // a dependent `x`). Only the uninstantiated pattern is in the function's
    // use map, so classify the use by the member name as above.
    if (isa<UnresolvedMemberExpr>(parent) || isa<CXXDependentScopeMemberExpr>(parent)) {
      std::string name;
      if (const auto *ume = dyn_cast<UnresolvedMemberExpr>(parent)) {
        name = ume->getMemberName().getAsString();
      } else {
        name = cast<CXXDependentScopeMemberExpr>(parent)->getMember().getAsString();
      }
      auto grandparents = Ctx.getParents(*parent);
      const auto *call = grandparents.empty() ? nullptr : grandparents[0].get<CallExpr>();
      if (!call || call->getCallee()->IgnoreImplicit() != parent) return Escape::Escapes;
      if (isSynchronousConsumerMethod(name)) return Escape::Local;
      if (isPromisePassThroughMethod(name) || isContainerFinalizeMethod(name)) {
        current = call;
        continue;
      }
      if (isContainerInsertMethod(name) || isContainerQueryMethod(name)) return Escape::Local;
      return Escape::Escapes;
    }

    // `return <expr>;` / `co_return <expr>;` -- the continuation outlives
    // the function. When that function is a lambda passed directly to a
    // synchronous sink (see `kBuiltinSynchronousSinks`), the sink
    // `.wait()`s on the return value before its own activation ends, so
    // the captures cannot dangle. When it is passed to a result-owning sink
    // (see `kBuiltinResultOwningSinks`), the sink call's result owns the
    // return value, so classify that call's destination instead.
    if (isa<ReturnStmt>(parent) || isa<CoreturnStmt>(parent)) {
      ReturnTarget target = returnTargetOf(parent, Ctx);
      if (!target.fromLambda) return Escape::Returned;
      auto recordCrossed = [&](bool closureDies) {
        if (sinks.crossed && target.lambda) sinks.crossed->push_back({target.lambda, closureDies});
      };
      if (target.lambdaVar) {
        // The closure lives as long as the enclosing function, so it dies
        // first only when the chain is returned from that function.
        Escape lambdaEscape = classifyLocalLambdaCalls(target.lambdaVar, Ctx, depth, sinks);
        recordCrossed(lambdaEscape == Escape::Returned);
        return lambdaEscape;
      }
      if (!target.lambdaSinkCall) return Escape::Escapes;
      std::string sinkName = calleeQualifiedName(target.lambdaSinkCall);
      if (isSynchronousSink(sinkName, sinks.sync)) {
        recordCrossed(false);
        return Escape::Local;
      }
      if (isResultOwningSink(sinkName, sinks.resultOwning)) {
        recordCrossed(true);
        current = target.lambdaSinkCall;
        continue;
      }
      return Escape::Escapes;
    }

    // `co_await <expr>` / `co_yield <expr>` -- consumed by the coroutine
    // suspension; the coroutine frame keeps the captures alive.
    if (isa<CoawaitExpr>(parent) || isa<CoyieldExpr>(parent)) return Escape::Local;

    // Assignment via overloaded operator= (e.g. `kj::Promise::operator=`).
    // Represented as a CXXOperatorCallExpr with kind OO_Equal, two args:
    // arg(0) is the LHS, arg(1) is the RHS. We only get here when `current`
    // is the RHS argument of such a call. Must come before the generic
    // CXXMemberCallExpr / CallExpr branches because CXXOperatorCallExpr
    // inherits from CallExpr (and operator= calls have a CallExpr-shape).
    if (const auto *oce = dyn_cast<CXXOperatorCallExpr>(parent)) {
      if (oce->getOperator() == OO_Equal && oce->getNumArgs() == 2) {
        const Expr *lhs = oce->getArg(0)->IgnoreImplicit()->IgnoreParens();
        // `this->member = <expr>;` — the field's destructor cancels the chain
        // when `*this` dies, so a captured `this` cannot dangle.
        if (const auto *me = dyn_cast<MemberExpr>(lhs)) {
          const Expr *base = me->getBase()->IgnoreImplicit()->IgnoreParens();
          if (isa<CXXThisExpr>(base) && isa<FieldDecl>(me->getMemberDecl())) {
            return Escape::StoredAsSelfMember;
          }
        }
        // Assignment to a local variable.
        if (const auto *dre = dyn_cast<DeclRefExpr>(lhs)) {
          if (const auto *vd = dyn_cast<VarDecl>(dre->getDecl())) {
            if (vd->isLocalVarDecl()) {
              auto fnParents = Ctx.getParents(*vd);
              const FunctionDecl *enclosingFn = nullptr;
              while (!fnParents.empty()) {
                if (const auto *fd = fnParents[0].get<FunctionDecl>()) {
                  enclosingFn = fd;
                  break;
                }
                if (const auto *st = fnParents[0].get<Stmt>()) {
                  fnParents = Ctx.getParents(*st);
                  continue;
                }
                if (const auto *dd = fnParents[0].get<Decl>()) {
                  fnParents = Ctx.getParents(*dd);
                  continue;
                }
                break;
              }
              return classifyVarDeclUses(vd, enclosingFn, Ctx, depth + 1, sinks);
            }
          }
        }
        return Escape::Escapes;
      }
    }

    // Member call: `<expr>.method(...)`. The receiver is `<expr>`.
    if (const auto *mce = dyn_cast<CXXMemberCallExpr>(parent)) {
      // Confirm `current` is the *receiver* of the call, not an argument.
      const Expr *receiver = nullptr;
      if (const auto *me = dyn_cast<MemberExpr>(mce->getCallee())) {
        receiver = me->getBase();
      }
      // Compare ignoring wrappers.
      const Stmt *peeledRecv = receiver ? peelWrappers(receiver) : nullptr;
      const Stmt *peeledCurrent = peelWrappers(current);
      if (peeledRecv == peeledCurrent) {
        if (const auto *md = mce->getMethodDecl()) {
          llvm::StringRef name = safeGetName(md);
          if (isSynchronousConsumerMethod(name)) {
            return Escape::Local;
          }
          if (isPromisePassThroughMethod(name)) {
            // Recurse: the chain's escape is determined by what happens
            // to *this* call's result.
            current = mce;
            continue;
          }
          // Local promise-container method on the container as receiver.
          Escape containerResult = Escape::Escapes;
          const Stmt *containerNext = nullptr;
          if (classifyContainerReceiverMethod(name, mce, containerResult, containerNext)) {
            if (containerNext) {
              current = containerNext;
              continue;
            }
            return containerResult;
          }
        }
        // Unknown method on what looks like a promise -- could be a
        // side-effect-free chain (e.g. `.then(...).attach(...)`), or
        // could be `.fork()` which produces a ForkedPromise (escape
        // semantics differ). Conservative: escapes.
        return Escape::Escapes;
      }
      // We were an argument to a member call. If the callee is a
      // *promise combinator* (e.g. `q.exclusiveJoin(kj::mv(p))`), the
      // resulting call inherits our lifetime; recurse on it.
      // Otherwise the callee likely stores or forwards the promise.
      if (const auto *md = mce->getMethodDecl()) {
        llvm::StringRef name = safeGetName(md);
        if (isPromisePassThroughMethod(name)) {
          current = mce;
          continue;
        }
        // Container-insert method (e.g. `promises.add(continuation)`): the
        // continuation's lifetime is bounded by the (local) container's. Find
        // the receiver variable and classify the container's fate. If the
        // container is finalized + joined + awaited locally, the continuation
        // does not escape; if the container itself escapes, so does it.
        if (isContainerInsertMethod(name)) {
          const Expr *recv = nullptr;
          if (const auto *me = dyn_cast<MemberExpr>(mce->getCallee())) recv = me->getBase();
          if (recv) {
            const Stmt *peeled = peelWrappers(recv);
            if (const auto *dre = dyn_cast<DeclRefExpr>(peeled)) {
              if (const auto *vd = dyn_cast<VarDecl>(dre->getDecl())) {
                if (vd->isLocalVarDecl()) {
                  return classifyVarDeclUses(
                      vd, enclosingFunctionOf(vd, Ctx), Ctx, depth + 1, sinks);
                }
              }
            }
            // Member container of `*this` (e.g. `this->tasks.add(...)`, where
            // `tasks` is a kj::TaskSet / promise container field): the
            // continuation is owned by a member of `*this`. That member's
            // destructor cancels the continuation before any other field of
            // `*this` can dangle, so a captured `this` is safe (other captures
            // still escape past the chain's invocation point).
            if (const auto *me2 = dyn_cast<MemberExpr>(peeled)) {
              const Expr *base = me2->getBase()->IgnoreImplicit()->IgnoreParens();
              if (isa<CXXThisExpr>(base) && isa<FieldDecl>(me2->getMemberDecl())) {
                return Escape::StoredAsSelfMember;
              }
            }
          }
          // Other container (member of another object, global, ...) or
          // unrecognized receiver: the continuation escapes with it.
          return Escape::Escapes;
        }
        // Async-sink methods (e.g. `IoContext::awaitJs(js, promise, ...)`)
        // simultaneously consume an argument and return a *new* promise
        // whose escape determines whether our lifetime concerns are
        // bounded. The argument's continuation chain runs to completion
        // before the returned promise resolves -- so if that returned
        // promise is locally consumed, our argument chain's captures
        // cannot dangle. Recurse on the call.
        std::string qn;
        if (const auto *spec = dyn_cast<ClassTemplateSpecializationDecl>(md->getParent())) {
          if (const auto *td = spec->getSpecializedTemplate()) {
            qn = td->getQualifiedNameAsString() + "::" + md->getNameAsString();
          }
        }
        if (qn.empty()) qn = md->getQualifiedNameAsString();
        if (isAsyncSink(qn, sinks.async) || isPromiseOwningMethod(qn)) {
          current = mce;
          continue;
        }
      }
      return Escape::Escapes;
    }

    // We are an argument to a non-member call. A few "transparent"
    // wrappers are commonly applied to promises without changing their
    // semantics (`kj::mv(p)`, `std::move(p)`, `kj::cp(p)`). Treat these
    // as pass-through: the escape semantics of the wrapper expression
    // determine the inner promise's destiny.
    if (const auto *ce = dyn_cast<CallExpr>(parent)) {
      if (const auto *fd = ce->getDirectCallee()) {
        llvm::StringRef name = safeGetName(fd);
        if (name == "mv" || name == "move" || name == "cp" || name == "fwd" || name == "forward") {
          current = parent;
          continue;
        }
        // `kj::joinPromises({p1, p2, ...})`, `kj::joinPromisesFailFast(...)`:
        // the returned promise's lifetime includes our captures.
        if (isPromiseCombinatorFunction(name)) {
          current = parent;
          continue;
        }
        // `kj::arr(p1, p2, ...)`: the resulting array carries our lifetime;
        // its destination (typically a joinPromises* + co_await) decides.
        if (isArrayConstructorFunction(name)) {
          current = parent;
          continue;
        }
      }
      // Other non-member calls (joinPromises, addWaitUntil, custom
      // task-set adds, ...): the promise leaves the function.
      return Escape::Escapes;
    }

    // Variable initialization: `auto x = <expr>;`
    if (const auto *ds = dyn_cast<DeclStmt>(parent)) {
      // Find the VarDecl whose initializer we are. There can be multiple
      // decls in one DeclStmt (`int a = ..., b = ...;`).
      for (const auto *d: ds->decls()) {
        if (const auto *vd = dyn_cast<VarDecl>(d)) {
          if (vd->hasInit() && peelWrappers(vd->getInit()) == peelWrappers(current)) {
            if (vd->isLocalVarDecl()) {
              // Find enclosing function and analyze uses.
              auto fnParents = Ctx.getParents(*vd);
              const FunctionDecl *enclosingFn = nullptr;
              while (!fnParents.empty()) {
                if (const auto *fd = fnParents[0].get<FunctionDecl>()) {
                  enclosingFn = fd;
                  break;
                }
                if (const auto *st = fnParents[0].get<Stmt>()) {
                  fnParents = Ctx.getParents(*st);
                  continue;
                }
                if (const auto *dd = fnParents[0].get<Decl>()) {
                  fnParents = Ctx.getParents(*dd);
                  continue;
                }
                break;
              }
              return classifyVarDeclUses(vd, enclosingFn, Ctx, depth + 1, sinks);
            }
            return Escape::Escapes;
          }
        }
      }
      // DeclStmt with no matching VarDecl -- shouldn't happen, be safe.
      return Escape::Escapes;
    }

    // Assignment to an existing variable: `x = <expr>;`
    if (const auto *bo = dyn_cast<BinaryOperator>(parent)) {
      // Comparisons / arithmetic / etc: the value is consumed at this
      // point. The promise's .wait() or value-extraction must have
      // already happened to participate in the comparison, so the
      // captures cannot outlive the enclosing full-expression.
      if (bo->isComparisonOp() || bo->isLogicalOp() || bo->isAdditiveOp() ||
          bo->isMultiplicativeOp() || bo->isBitwiseOp() || bo->isShiftOp()) {
        return Escape::Local;
      }
      if (bo->isAssignmentOp() || bo->isCompoundAssignmentOp()) {
        // RHS of assignment. Trace the LHS:
        const Expr *lhs = bo->getLHS()->IgnoreImplicit()->IgnoreParens();
        if (const auto *dre = dyn_cast<DeclRefExpr>(lhs)) {
          if (const auto *vd = dyn_cast<VarDecl>(dre->getDecl())) {
            if (vd->isLocalVarDecl()) {
              // Find enclosing function and analyze uses (including this
              // assignment).
              auto fnParents = Ctx.getParents(*vd);
              const FunctionDecl *enclosingFn = nullptr;
              while (!fnParents.empty()) {
                if (const auto *fd = fnParents[0].get<FunctionDecl>()) {
                  enclosingFn = fd;
                  break;
                }
                if (const auto *st = fnParents[0].get<Stmt>()) {
                  fnParents = Ctx.getParents(*st);
                  continue;
                }
                if (const auto *dd = fnParents[0].get<Decl>()) {
                  fnParents = Ctx.getParents(*dd);
                  continue;
                }
                break;
              }
              return classifyVarDeclUses(vd, enclosingFn, Ctx, depth + 1, sinks);
            }
          }
        }
        // Is this `this->member = ...`? (member of the current class)
        // The field's destructor will cancel the chain when `*this`
        // dies, so `[this]` captures cannot dangle. (ForkedPromise
        // branches resolve to cancelled, not UAF.)
        if (const auto *me = dyn_cast<MemberExpr>(lhs)) {
          const Expr *base = me->getBase()->IgnoreImplicit()->IgnoreParens();
          if (isa<CXXThisExpr>(base) && isa<FieldDecl>(me->getMemberDecl())) {
            return Escape::StoredAsSelfMember;
          }
        }
        // Assignment to other member/static/global etc.: escapes.
        return Escape::Escapes;
      }
    }

    // Discarded as a full-expression: `continuation(...);`
    if (isa<CompoundStmt>(parent)) {
      return Escape::Local;
    }

    // Conditional / control flow that uses the value: be conservative.
    if (isa<IfStmt>(parent) || isa<SwitchStmt>(parent) || isa<WhileStmt>(parent) ||
        isa<DoStmt>(parent) || isa<ForStmt>(parent) || isa<CXXForRangeStmt>(parent)) {
      // Used as a condition: by the time the body runs, the value has
      // been consumed; treat as local.
      return Escape::Local;
    }

    // Ternary, comma operator, etc.: walk up.
    if (isa<ConditionalOperator>(parent) || isa<BinaryConditionalOperator>(parent)) {
      current = parent;
      continue;
    }

    // Default: unknown context. Bias toward "escapes" so the diagnostic
    // fires.
    return Escape::Escapes;
  }
}

static Escape promiseEscape(const CallExpr *Call, ASTContext &Ctx, const SinkLists &sinks) {
  return classifyExprUse(Call, Ctx, 0, sinks);
}

// ---- Referents with static storage duration --------------------------------
//
// A by-reference capture cannot dangle when its referent has static storage
// duration. Lambdas never capture such variables themselves (they name them
// directly), so the cases that reach the check are references bound to
// them: a by-reference init-capture (`[&s = staticVar]`) or a reference
// local (`auto& s = staticVar;`, or `auto& s = getStatic();` where the body
// of `getStatic()` is visible and only returns static variables).

static bool isStaticStorageVar(const VarDecl *vd) {
  return vd && vd->getStorageDuration() == SD_Static && !vd->getType()->isReferenceType();
}

static bool referenceBoundToStaticStorage(const VarDecl *vd, int depth);

// Collects the `return` statements of one function body, skipping nested
// lambdas and local classes, whose returns belong to other functions.
class ReturnCollector: public RecursiveASTVisitor<ReturnCollector> {
 public:
  llvm::SmallVector<const ReturnStmt *, 4> returns;
  bool VisitReturnStmt(ReturnStmt *rs) {
    returns.push_back(rs);
    return true;
  }
  bool TraverseLambdaExpr(LambdaExpr *) {
    return true;
  }
  bool TraverseCXXRecordDecl(CXXRecordDecl *) {
    return true;
  }
};

static bool exprRefersToStaticStorage(const Expr *e, int depth);

// True iff `fd` returns a reference and every `return` in its body returns
// a variable with static storage duration. Virtual functions are excluded
// because the call may dispatch to a different body.
static bool functionReturnsStaticStorage(const FunctionDecl *fd, int depth) {
  if (!fd || !fd->getReturnType()->isReferenceType()) return false;
  if (const auto *md = dyn_cast<CXXMethodDecl>(fd); md && md->isVirtual()) return false;
  const FunctionDecl *def = nullptr;
  if (!fd->hasBody(def) || !def || !def->getBody()) return false;
  ReturnCollector collector;
  collector.TraverseStmt(def->getBody());
  if (collector.returns.empty()) return false;
  for (const auto *rs: collector.returns) {
    if (!exprRefersToStaticStorage(rs->getRetValue(), depth + 1)) return false;
  }
  return true;
}

static bool exprRefersToStaticStorage(const Expr *e, int depth) {
  if (!e || depth > 4) return false;
  e = e->IgnoreImplicit()->IgnoreParens();
  if (const auto *dre = dyn_cast<DeclRefExpr>(e)) {
    const auto *vd = dyn_cast<VarDecl>(dre->getDecl());
    return isStaticStorageVar(vd) || referenceBoundToStaticStorage(vd, depth + 1);
  }
  if (const auto *ce = dyn_cast<CallExpr>(e)) {
    return functionReturnsStaticStorage(ce->getDirectCallee(), depth + 1);
  }
  return false;
}

// True iff `vd` is a reference local or by-reference init-capture whose
// initializer provably names a variable with static storage duration.
static bool referenceBoundToStaticStorage(const VarDecl *vd, int depth) {
  if (!vd || isa<ParmVarDecl>(vd) || !vd->hasLocalStorage()) return false;
  if (!vd->getType()->isReferenceType()) return false;
  return exprRefersToStaticStorage(vd->getInit(), depth);
}

// ---- Promise parameter contract ---------------------------------------------
//
// The KJ style guide (deps/capnproto/style-guide.md, "Ownership") makes this
// the default for every function: a pointer or reference parameter must stay
// valid until the function returns, or, when the function returns a promise,
// until that promise completes or is canceled. So a continuation that is part
// of the promise a function returns may capture that function's own
// reference, pointer and view parameters. A caller that breaks the contract is
// the bug, and this check cannot see callers.

// True iff `fn` returns `kj::Promise<T>`.
static bool returnsKjPromise(const FunctionDecl *fn) {
  if (!fn) return false;
  const auto *rd = fn->getReturnType().getCanonicalType()->getAsCXXRecordDecl();
  return rd && rd->getQualifiedNameAsString() == "kj::Promise";
}

// True iff the capture refers to a parameter of `promiseFn` whose
// referent the contract keeps alive: a reference parameter
// (captured either way), or a view or pointer parameter captured by value.
// A by-value parameter captured by reference refers to the function's own
// stack slot, which dies when the function returns.
static bool capturesContractParameter(const LambdaCapture &cap, const FunctionDecl *promiseFn) {
  if (!promiseFn || !cap.capturesVariable()) return false;
  const auto *pvd = dyn_cast_or_null<ParmVarDecl>(cap.getCapturedVar());
  if (!pvd || pvd->getDeclContext() != promiseFn) return false;
  return cap.getCaptureKind() != LCK_ByRef || pvd->getType()->isReferenceType();
}

// True iff `cap` is a by-reference capture of storage that a lambda in
// `crossed` owns, and that so dies when that lambda returns: one of its
// non-reference parameters or locals, or, when its closure dies too, one of
// its by-copy or non-reference init-captures. Nothing that holds the chain
// afterwards can keep that storage alive.
static bool capturesCrossedLambdaStorage(
    const LambdaCapture &cap, const std::vector<CrossedLambda> &crossed) {
  if (cap.getCaptureKind() != LCK_ByRef) return false;
  const auto *vd = dyn_cast_or_null<VarDecl>(cap.getCapturedVar());
  if (!vd) return false;
  for (const auto &c: crossed) {
    if (vd->getDeclContext() == c.lambda->getCallOperator()) {
      if (vd->getType()->isReferenceType()) continue;
      if (!vd->isInitCapture() || c.closureDies) return true;
    } else if (c.closureDies) {
      for (const auto &outer: c.lambda->captures()) {
        if (outer.capturesVariable() && outer.getCapturedVar() == vd &&
            outer.getCaptureKind() == LCK_ByCopy) {
          return true;
        }
      }
    }
  }
  return false;
}

// ---- Suspendable request contract ------------------------------------------
//
// `kj::HttpServer::SuspendableRequest` documents that its `url` and `headers`
// stay valid until the HttpService the factory returns is destroyed (unless
// the request is suspended, in which case the factory returns no service). So
// a chain owned by that returned service may capture the request's `url` and
// `headers`. "Owned by" is narrow: the chain is the direct argument of a
// `kj::heap<T>(...)` call, T is a kj::HttpService, and that heap call's result
// is what the enclosing function returns. Passing a promise by value to a
// constructor transfers ownership of it to the constructed object.

static bool derivesFrom(const CXXRecordDecl *rd, llvm::StringRef base, int depth = 0) {
  if (!rd || !rd->hasDefinition() || depth > 16) return false;
  if (rd->getQualifiedNameAsString() == base) return true;
  for (const auto &b: rd->bases()) {
    if (derivesFrom(b.getType()->getAsCXXRecordDecl(), base, depth + 1)) return true;
  }
  return false;
}

// The `kj::heap<T>(...)` call, with T a kj::HttpService, that takes the chain
// `call` as a direct argument, if any.
static const CallExpr *heapHttpServiceOwning(const CallExpr *call, ASTContext &Ctx) {
  const Stmt *current = call;
  for (int hops = 0; hops < 16; ++hops) {
    auto parents = Ctx.getParents(*current);
    if (parents.empty()) return nullptr;
    const Stmt *parent = parents[0].get<Stmt>();
    if (!parent) return nullptr;
    if (isa<ImplicitCastExpr>(parent) || isa<MaterializeTemporaryExpr>(parent) ||
        isa<CXXBindTemporaryExpr>(parent) || isa<ParenExpr>(parent) ||
        (isa<CXXConstructExpr>(parent) && cast<CXXConstructExpr>(parent)->getNumArgs() == 1)) {
      current = parent;
      continue;
    }
    const auto *heapCall = dyn_cast<CallExpr>(parent);
    if (!heapCall || calleeQualifiedName(heapCall) != "kj::heap") return nullptr;
    const auto *own = heapCall->getType().getCanonicalType()->getAsCXXRecordDecl();
    const auto *spec = dyn_cast_or_null<ClassTemplateSpecializationDecl>(own);
    if (!spec || spec->getQualifiedNameAsString() != "kj::Own" ||
        spec->getTemplateArgs().size() < 1) {
      return nullptr;
    }
    const auto *target = spec->getTemplateArgs()[0].getAsType()->getAsCXXRecordDecl();
    return derivesFrom(target, "kj::HttpService") ? heapCall : nullptr;
  }
  return nullptr;
}

// True iff the capture is `requestFn`'s `kj::HttpServer::SuspendableRequest&`
// parameter, or a reference bound (by an init-capture or local reference) to
// that parameter's `url` or `headers`.
static bool capturesSuspendableRequest(const LambdaCapture &cap, const FunctionDecl *requestFn) {
  if (!requestFn || !cap.capturesVariable()) return false;
  const auto *vd = dyn_cast_or_null<VarDecl>(cap.getCapturedVar());
  for (int hops = 0; vd && hops < 16; ++hops) {
    if (const auto *pvd = dyn_cast<ParmVarDecl>(vd)) {
      const auto *rd = pvd->getType().getNonReferenceType()->getAsCXXRecordDecl();
      return pvd->getDeclContext() == requestFn && pvd->getType()->isReferenceType() && rd &&
          rd->getQualifiedNameAsString() == "kj::HttpServer::SuspendableRequest";
    }
    if (!vd->getType()->isReferenceType() || !vd->getInit()) return false;
    const Expr *init = vd->getInit()->IgnoreParenImpCasts();
    if (const auto *me = dyn_cast<MemberExpr>(init)) {
      auto name = me->getMemberDecl()->getName();
      if (name != "url" && name != "headers") return false;
      init = me->getBase()->IgnoreParenImpCasts();
    }
    const auto *dre = dyn_cast<DeclRefExpr>(init);
    vd = dre ? dyn_cast<VarDecl>(dre->getDecl()) : nullptr;
  }
  return false;
}

}  // namespace

// Per-function use-map cache. Defined at namespace scope (outside the
// anonymous namespace) so that the check's `onEndOfTranslationUnit`
// member can clear it. `thread_local` keeps clang-tidy worker threads
// from racing on shared state.
static thread_local llvm::DenseMap<const FunctionDecl *, FunctionUseMap> g_perFunctionUseMaps;

const FunctionUseMap &getOrBuildFunctionUseMap(const FunctionDecl *fn) {
  auto it = g_perFunctionUseMaps.find(fn);
  if (it != g_perFunctionUseMaps.end()) return it->second;
  auto &slot = g_perFunctionUseMaps[fn];
  if (const Stmt *body = fn->getBody()) {
    FunctionUseMapBuilder builder(slot);
    builder.TraverseStmt(const_cast<Stmt *>(body));
  }
  return slot;
}

UnsafeContinuationCaptureCheck::UnsafeContinuationCaptureCheck(
    clang::StringRef Name, clang::tidy::ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      AsyncSinksRaw(Options.get("AsyncSinks", "").str()),
      OwningCaptureTypesRaw(Options.get("OwningCaptureTypes", "").str()),
      SynchronousSinksRaw(Options.get("SynchronousSinks", "").str()),
      ResultOwningSinksRaw(Options.get("ResultOwningSinks", "").str()) {
  auto split = [](llvm::StringRef in, std::vector<std::string> &out) {
    llvm::SmallVector<llvm::StringRef, 8> parts;
    in.split(parts, ',', -1, /*KeepEmpty=*/false);
    for (auto p: parts) {
      out.push_back(p.trim().str());
    }
  };
  split(AsyncSinksRaw, ExtraSinks);
  split(OwningCaptureTypesRaw, ExtraOwningTypes);
  split(SynchronousSinksRaw, ExtraSyncSinks);
  split(ResultOwningSinksRaw, ExtraResultOwningSinks);
}

void UnsafeContinuationCaptureCheck::storeOptions(clang::tidy::ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "AsyncSinks", AsyncSinksRaw);
  Options.store(Opts, "OwningCaptureTypes", OwningCaptureTypesRaw);
  Options.store(Opts, "SynchronousSinks", SynchronousSinksRaw);
  Options.store(Opts, "ResultOwningSinks", ResultOwningSinksRaw);
}

void UnsafeContinuationCaptureCheck::onEndOfTranslationUnit() {
  g_perFunctionUseMaps.clear();
}

void UnsafeContinuationCaptureCheck::registerMatchers(MatchFinder *Finder) {
  // Match every CallExpr that has a LambdaExpr as a *direct* argument.
  // The argument position alone is the cheap structural filter for
  // "non-immediate"; we then verify the callee is an async sink in
  // check(). Anchoring on the call (rather than walking ancestors from
  // the lambda) ensures the matcher fires exactly once per (lambda,
  // call) pair, regardless of how deeply the lambda is nested inside
  // other expressions.
  Finder->addMatcher(
      callExpr(unless(cxxOperatorCallExpr()),
          hasAnyArgument(ignoringImplicit(ignoringParenCasts(lambdaExpr().bind("lambda")))))
          .bind("call"),
      this);
}

void UnsafeContinuationCaptureCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Lambda = Result.Nodes.getNodeAs<LambdaExpr>("lambda");
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  if (!Lambda || !Call) return;

  // Don't lint generated code.
  auto &SM = *Result.SourceManager;
  if (SM.isInSystemHeader(Lambda->getBeginLoc())) return;

  // The matcher's `hasAnyArgument(ignoringImplicit(ignoringParenCasts(...)))`
  // already guarantees the lambda is a direct argument of this call;
  // no further argument-list scan is needed.

  // Identify the callee. A member of a class-template specialization has a
  // qualified name like `kj::Promise<int>::then`; `calleeQualifiedName`
  // drops the template arguments so it matches `kj::Promise::then`.
  std::string calleeName = calleeQualifiedName(Call);
  if (calleeName.empty()) return;

  if (!isAsyncSink(calleeName, ExtraSinks)) return;

  // Escape analysis: even though the lambda is passed to an async sink,
  // if the resulting promise is locally consumed (`.wait()`-ed,
  // `co_await`-ed, or used only by other locally-consumed promises), the
  // captures cannot outlive the current function activation. Suppress.
  std::vector<CrossedLambda> crossed;
  SinkLists sinks{ExtraSinks, ExtraSyncSinks, ExtraResultOwningSinks, &crossed};
  Escape escape = promiseEscape(Call, *Result.Context, sinks);
  if (escape == Escape::Local && crossed.empty()) {
    return;
  }

  // Promise parameter contract: the chain is what the enclosing function
  // returns as its promise, so that function's reference, pointer and view
  // parameters outlive it.
  const FunctionDecl *promiseFn = nullptr;
  if (escape == Escape::Returned) {
    const auto *fn = enclosingNonLambdaFunction(Call, *Result.Context);
    if (returnsKjPromise(fn)) promiseFn = fn;
  }

  // Suspendable request contract: the chain is owned by the HttpService the
  // enclosing function returns.
  const FunctionDecl *requestFn = nullptr;
  if (const CallExpr *heapCall = heapHttpServiceOwning(Call, *Result.Context)) {
    if (classifyExprUse(heapCall, *Result.Context, 0, sinks) == Escape::Returned) {
      requestFn = enclosingNonLambdaFunction(Call, *Result.Context);
    }
  }

  // Collect bindings introduced by downstream `.attach(<owning-expr>...)`
  // calls in the promise chain. Captures of those bound names are kept
  // alive by the chain itself and are safe.
  BoundNames bound = collectChainAttachBindings(Call, *Result.Context);

  // Receiver-owned capture suppression: if the matched sink is a
  // member call (e.g. `context.awaitIo(...)` / `context.run(...)`),
  // captures of the receiver's underlying VarDecl cannot dangle, because
  // the receiver owns the continuation. Record that VarDecl so we can
  // suppress those captures below.
  const VarDecl *receiverVar = nullptr;
  if (const auto *mce = dyn_cast<CXXMemberCallExpr>(Call)) {
    if (const auto *me = dyn_cast<MemberExpr>(mce->getCallee())) {
      const Expr *base = me->getBase()->IgnoreImplicit()->IgnoreParens();
      if (const auto *dre = dyn_cast<DeclRefExpr>(base)) {
        if (const auto *vd = dyn_cast<VarDecl>(dre->getDecl())) {
          receiverVar = vd;
        }
      }
    }
  }

  // Walk captures and report each unsafe one.
  const auto *LambdaClass = Lambda->getLambdaClass();
  auto fieldIt = LambdaClass->field_begin();
  for (const auto &cap: Lambda->captures()) {
    QualType fieldTy;
    if (fieldIt != LambdaClass->field_end()) {
      fieldTy = fieldIt->getType();
      ++fieldIt;
    }
    auto safety = classifyCapture(cap, fieldTy, ExtraOwningTypes);
    if (safety == CaptureSafety::Safe) continue;

    // A capture of storage owned by a lambda the chain was returned through
    // dies at that return, so neither a local consumer of the chain nor a
    // contract about the enclosing function's parameters keeps it alive.
    bool diesWithCrossedLambda = capturesCrossedLambdaStorage(cap, crossed);
    if (escape == Escape::Local && !diesWithCrossedLambda) continue;

    // Suppress: this capture is kept alive by a downstream `.attach()`.
    if (cap.capturesThis() && bound.boundThis) continue;

    // Suppress: the referent has static storage duration.
    if (safety == CaptureSafety::UnsafeBareReference && cap.capturesVariable() &&
        referenceBoundToStaticStorage(dyn_cast_or_null<VarDecl>(cap.getCapturedVar()), 0)) {
      continue;
    }

    // Suppress: the promise parameter contract keeps this parameter alive
    // until the returned promise settles.
    if (!diesWithCrossedLambda && capturesContractParameter(cap, promiseFn)) continue;

    // Suppress: the request's url and headers outlive the returned service.
    if (!diesWithCrossedLambda && capturesSuspendableRequest(cap, requestFn)) continue;

    // Suppress: the chain is stored as a member of the class whose `this`
    // was captured. The field's destructor will cancel the chain before
    // `this` becomes invalid. (Other captures are unaffected: a captured
    // reference to a stack local still dangles when the constructor /
    // method returns.)
    if (cap.capturesThis() && escape == Escape::StoredAsSelfMember) continue;

    // Suppress: the capture is a reference (or value) of the same
    // variable that is the receiver of the matched sink call. The
    // receiver owns the continuation, so its lifetime envelops the
    // capture.
    if (!diesWithCrossedLambda && receiverVar && cap.capturesVariable() &&
        cap.getCapturedVar() == receiverVar) {
      continue;
    }
    if (cap.capturesVariable()) {
      const auto *vd = cap.getCapturedVar();
      bool isBound = false;
      // Direct match on the captured VarDecl: `[&x]` + `.attach(kj::mv(x))`.
      if (vd) {
        for (const auto *bv: bound.boundVars) {
          if (bv == vd) {
            isBound = true;
            break;
          }
        }
      }
      // Init-capture match: `[&f = *base.member]` +
      // `.attach(kj::mv(base.member))`. Walk the capture's field
      // initializer expression and see if it dereferences a bound
      // (base, member) pair.
      if (!isBound && !bound.boundMembers.empty()) {
        const FieldDecl *fd = nullptr;
        // The field corresponding to this capture is the one we just
        // incremented past; rewind by one.
        auto it = LambdaClass->field_begin();
        for (unsigned i = 0; it != LambdaClass->field_end(); ++it, ++i) {
          // Match name-by-name: by-value/init-captures' field name
          // equals the capture name.
          if (vd && it->getName() == vd->getName()) {
            fd = *it;
            break;
          }
        }
        if (fd) {
          // Look up the per-capture init expression on the LambdaExpr.
          unsigned idx = 0;
          for (const auto &c: Lambda->captures()) {
            if (&c == &cap) break;
            ++idx;
          }
          const Expr *initE = nullptr;
          unsigned i = 0;
          for (const Expr *e: Lambda->capture_inits()) {
            if (i == idx) {
              initE = e;
              break;
            }
            ++i;
          }
          if (initE) {
            const Expr *peeled = initE->IgnoreImplicit()->IgnoreParens();
            // Peel a leading `*` (for `[&f = *paf.fulfiller]`
            // or `[&adapter = *adapter]`).
            if (const auto *uo = dyn_cast<UnaryOperator>(peeled)) {
              if (uo->getOpcode() == UO_Deref) {
                peeled = uo->getSubExpr()->IgnoreImplicit()->IgnoreParens();
              }
            }
            // `[&x = y]` or `[&x = *y]` — init is a DeclRefExpr to a
            // local; match against attach-bound vars.
            if (const auto *dre = dyn_cast<DeclRefExpr>(peeled)) {
              if (const auto *vd = dyn_cast<VarDecl>(dre->getDecl())) {
                for (const auto *bv: bound.boundVars) {
                  if (bv == vd) {
                    isBound = true;
                    break;
                  }
                }
              }
            }
            // `[&f = *base.member]` — init is a MemberExpr; match
            // against attach-bound (base, member) pairs.
            if (!isBound) {
              if (const auto *me = dyn_cast<MemberExpr>(peeled)) {
                const Expr *baseE = me->getBase()->IgnoreImplicit()->IgnoreParens();
                if (const auto *baseDre = dyn_cast<DeclRefExpr>(baseE)) {
                  if (const auto *baseVd = dyn_cast<VarDecl>(baseDre->getDecl())) {
                    for (const auto &bm: bound.boundMembers) {
                      if (bm.first == baseVd && bm.second == me->getMemberDecl()) {
                        isBound = true;
                        break;
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
      if (isBound) continue;
    }

    SourceLocation loc = cap.getLocation();
    if (!loc.isValid()) loc = Lambda->getBeginLoc();

    std::string name;
    if (cap.capturesThis()) {
      name = "this";
    } else if (cap.capturesVariable()) {
      if (const auto *vd = cap.getCapturedVar()) {
        name = vd->getNameAsString();
      }
    }
    if (name.empty()) name = "<capture>";

    diag(loc, "unsafe %0 capture of %1 in lambda passed to %2; %3")
        << safetyLabel(safety) << name << calleeName << safetyAdvice(safety);
    diag(Call->getBeginLoc(), "lambda is passed here", DiagnosticIDs::Note)
        << Call->getSourceRange();
  }
}

}  // namespace workerd::clang_tidy
