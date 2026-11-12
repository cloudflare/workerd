// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "js-reentry.h"

#include "clang-tidy/utils/ExprSequence.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

#include <algorithm>
#include <climits>

namespace workerd::clang_tidy {
namespace {

using namespace clang;

constexpr llvm::StringLiteral MAY_RUN_JS = "jsg_may_run_js";
constexpr llvm::StringLiteral NO_JS = "jsg_no_js";

// V8 entry points that can run script: calling a function, resolving a promise (which reads the
// resolution's `then`), reading, writing, enumerating or testing a property (getters, setters,
// proxy traps), converting to a string or number (`toString()`, `valueOf()`), comparing or
// serializing a value, running microtasks, or forcing a GC.
const llvm::StringSet<>& v8EntryPoints() {
  static const llvm::StringSet<> names = {
    "v8::Function::Call",
    "v8::Function::NewInstance",
    "v8::Object::CallAsFunction",
    "v8::Object::CallAsConstructor",
    "v8::Script::Run",
    "v8::Module::Evaluate",
    "v8::Promise::Resolver::Resolve",
    "v8::Object::Get",
    "v8::Object::Set",
    "v8::Object::Has",
    "v8::Object::HasOwnProperty",
    "v8::Object::Delete",
    "v8::Object::DefineOwnProperty",
    "v8::Object::CreateDataProperty",
    "v8::Object::GetOwnPropertyNames",
    "v8::Object::GetPropertyNames",
    "v8::Object::GetOwnPropertyDescriptor",
    "v8::Object::ObjectProtoToString",
    "v8::Value::ToString",
    "v8::Value::ToNumber",
    "v8::Value::ToBigInt",
    "v8::Value::ToInteger",
    "v8::Value::ToInt32",
    "v8::Value::ToUint32",
    "v8::Value::NumberValue",
    "v8::Value::IntegerValue",
    "v8::Value::Int32Value",
    "v8::Value::Uint32Value",
    "v8::Value::InstanceOf",
    "v8::Value::Equals",
    "v8::ValueSerializer::WriteValue",
    "v8::JSON::Stringify",
    "v8::Isolate::PerformMicrotaskCheckpoint",
    "v8::MicrotaskQueue::PerformCheckpoint",
    "v8::Isolate::RequestGarbageCollectionForTesting",
    "v8::Isolate::LowMemoryNotification",
  };
  return names;
}

// What a borrow may point into without JavaScript being able to free it.
const llvm::StringSet<>& ignoredReferents() {
  static const llvm::StringSet<> names = {
    "workerd::jsg::Lock",
    "workerd::IoContext",
    "v8::Isolate",
    "v8::Context",
  };
  return names;
}

bool isAnnotated(const FunctionDecl* function, llvm::StringRef annotation) {
  for (const auto* redecl: function->redecls()) {
    for (const auto* attr: redecl->specific_attrs<AnnotateAttr>()) {
      if (attr->getAnnotation() == annotation) return true;
    }
  }
  if (const auto* pattern = function->getTemplateInstantiationPattern();
      pattern != nullptr && pattern != function && isAnnotated(pattern, annotation)) {
    return true;
  }
  if (const auto* method = llvm::dyn_cast<CXXMethodDecl>(function)) {
    for (const auto* base: method->overridden_methods()) {
      if (isAnnotated(base, annotation)) return true;
    }
  }
  return false;
}

std::string recordName(QualType type) {
  const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
  return record == nullptr ? std::string() : record->getQualifiedNameAsString();
}

// Running JavaScript requires the lock, so a function passed one may run JavaScript unless it is
// marked JSG_NO_JS. A lambda's body is always in the translation unit, so its calls decide.
bool takesLock(const FunctionDecl* function) {
  if (const auto* method = llvm::dyn_cast<CXXMethodDecl>(function);
      method != nullptr && method->getParent()->isLambda()) {
    return false;
  }
  for (const auto* param: function->parameters()) {
    if (recordName(param->getType().getNonReferenceType()) == "workerd::jsg::Lock") return true;
  }
  return false;
}

// A call through a jsg::Function, which keeps its own target alive while the call runs, or a
// kj::Function, which is held in C++ state (a member or a capture) rather than in an object that
// JavaScript can release.
bool isFunctionObjectCall(const CallExpr* call) {
  const auto* method = llvm::dyn_cast_or_null<CXXMethodDecl>(call->getDirectCallee());
  if (method == nullptr) return false;
  auto name = method->getParent()->getQualifiedNameAsString();
  return name == "workerd::jsg::Function" || name == "kj::Function";
}

// An owner whose direct dereference keeps its target alive.
bool isOwningPointer(QualType type) {
  auto name = recordName(type.getNonReferenceType());
  return name == "workerd::jsg::Ref" || name == "kj::Own" || name == "kj::Rc" ||
      name == "kj::Arc" || name == "std::unique_ptr" || name == "std::shared_ptr";
}

// A reference, pointer, iterator or view: a value that does not keep what it points to alive.
bool isBorrowType(QualType type) {
  QualType referent;
  if (type->isReferenceType() || type->isPointerType()) {
    referent = type->getPointeeType();
    if (referent->isFunctionType()) return false;
    return !ignoredReferents().contains(recordName(referent));
  }
  const auto* record = type.getCanonicalType()->getAsCXXRecordDecl();
  if (record == nullptr) return false;
  auto name = record->getQualifiedNameAsString();
  if (name == "kj::ArrayPtr" || name == "kj::StringPtr" || name == "kj::Ptr" ||
      name == "std::basic_string_view" || name == "std::span") {
    return true;
  }
  auto shortName = record->getName();
  return shortName.ends_with("iterator") || shortName.ends_with("Iterator");
}

const Expr* strip(const Expr* expr) {
  while (true) {
    const Expr* next = expr->IgnoreImplicit()->IgnoreParenImpCasts();
    if (const auto* cast = llvm::dyn_cast<CXXFunctionalCastExpr>(next)) {
      next = cast->getSubExpr();
    }
    if (next == expr) return expr;
    expr = next;
  }
}

// Where a borrow points: into the function's own frame, into state the function does not own
// (reached through `this`, a reference parameter, a view parameter, or a member of an owning
// pointer), or through another tracked borrow.
struct Root {
  enum Kind { FRAME, SHARED, BORROW } kind = FRAME;
  const VarDecl* borrow = nullptr;

  static Root frame() {
    return {};
  }
  static Root shared() {
    return {SHARED, nullptr};
  }
  bool tracked() const {
    return kind != FRAME;
  }
};

class BorrowAnalysis {
 public:
  bool isTracked(const VarDecl* var) {
    auto it = tracked.find(var);
    if (it != tracked.end()) return it->second;
    tracked[var] = false;
    bool result = computeTracked(var);
    tracked[var] = result;
    return result;
  }

  Root rootOf(const Expr* expr, bool subobject, int depth = 0) {
    if (expr == nullptr || depth > 24) return Root::frame();
    expr = strip(expr);
    ++depth;

    if (llvm::isa<CXXThisExpr>(expr)) return Root::shared();

    if (const auto* ref = llvm::dyn_cast<DeclRefExpr>(expr)) {
      const auto* var = llvm::dyn_cast<VarDecl>(ref->getDecl());
      if (var == nullptr) return Root::frame();
      if (isTracked(var)) return {Root::BORROW, var};
      QualType type = var->getType();
      if (llvm::isa<ParmVarDecl>(var)) {
        if (type->isReferenceType() || type->isPointerType()) {
          return subobject ? Root::shared() : Root::frame();
        }
        // A view passed by value points into its caller's state, like a reference does.
        if (isBorrowType(type)) return Root::shared();
        return subobject && isOwningPointer(type) ? Root::shared() : Root::frame();
      }
      if (!var->hasLocalStorage()) return Root::frame();
      if (type->isReferenceType() || type->isPointerType()) {
        if (!isBorrowType(type)) return Root::frame();
        return rootOf(var->getInit(), subobject, depth);
      }
      return subobject && isOwningPointer(type) ? Root::shared() : Root::frame();
    }

    if (const auto* member = llvm::dyn_cast<MemberExpr>(expr)) {
      return rootOf(member->getBase(), true, depth);
    }

    if (const auto* call = llvm::dyn_cast<CXXMemberCallExpr>(expr)) {
      const Expr* object = call->getImplicitObjectArgument();
      const auto* method = call->getMethodDecl();
      if (method != nullptr && method->getIdentifier() != nullptr && method->getName() == "get" &&
          isOwningPointer(object->getType())) {
        return rootOfDeref(object, subobject, depth);
      }
      return rootOf(object, true, depth);
    }

    if (const auto* op = llvm::dyn_cast<CXXOperatorCallExpr>(expr)) {
      if (op->getNumArgs() == 0) return Root::frame();
      switch (op->getOperator()) {
        case OO_Star:
        case OO_Arrow:
          return rootOfDeref(op->getArg(0), subobject, depth);
        case OO_Subscript:
          return rootOf(op->getArg(0), true, depth);
        case OO_Call:
          if (op->getType()->isReferenceType() || op->isGLValue()) {
            return rootOf(op->getArg(0), true, depth);
          }
          return Root::frame();
        default:
          return Root::frame();
      }
    }

    if (const auto* unary = llvm::dyn_cast<UnaryOperator>(expr)) {
      if (unary->getOpcode() == UO_Deref || unary->getOpcode() == UO_AddrOf) {
        return rootOf(unary->getSubExpr(), subobject, depth);
      }
      return Root::frame();
    }

    if (const auto* subscript = llvm::dyn_cast<ArraySubscriptExpr>(expr)) {
      return rootOf(subscript->getBase(), true, depth);
    }

    if (const auto* statement = llvm::dyn_cast<StmtExpr>(expr)) {
      // `({ ...; value; })`, as KJ_ASSERT_NONNULL and KJ_REQUIRE_NONNULL expand to.
      const auto* compound = statement->getSubStmt();
      if (compound == nullptr || compound->body_empty()) return Root::frame();
      const auto* last = llvm::dyn_cast<Expr>(compound->body_back());
      return last == nullptr ? Root::frame() : rootOf(last, subobject, depth);
    }

    if (const auto* conditional = llvm::dyn_cast<AbstractConditionalOperator>(expr)) {
      Root a = rootOf(conditional->getTrueExpr(), subobject, depth);
      return a.tracked() ? a : rootOf(conditional->getFalseExpr(), subobject, depth);
    }

    if (const auto* construct = llvm::dyn_cast<CXXConstructExpr>(expr)) {
      // A view constructed from a container borrows the container's storage.
      if (construct->getNumArgs() > 0 && isBorrowType(construct->getType())) {
        return rootOf(construct->getArg(0), true, depth);
      }
      return Root::frame();
    }

    if (const auto* call = llvm::dyn_cast<CallExpr>(expr)) {
      // A free function returning a reference into its first argument, such as
      // kj::_::readMaybe() behind KJ_IF_SOME.
      QualType type = call->getType();
      if (call->getNumArgs() > 0 &&
          (call->isGLValue() || type->isPointerType() || isBorrowType(type))) {
        return rootOf(call->getArg(0), subobject, depth);
      }
      return Root::frame();
    }

    return Root::frame();
  }

 private:
  llvm::DenseMap<const VarDecl*, bool> tracked;

  // `*p`, `p->`, `p.get()`: a local owning pointer keeps its own target alive, but not one stored
  // in state the function does not own.
  Root rootOfDeref(const Expr* pointer, bool subobject, int depth) {
    const Expr* stripped = strip(pointer);
    if (const auto* ref = llvm::dyn_cast<DeclRefExpr>(stripped)) {
      if (const auto* var = llvm::dyn_cast<VarDecl>(ref->getDecl()); var != nullptr &&
          !var->getType()->isReferenceType() && isOwningPointer(var->getType()) &&
          (var->hasLocalStorage() || llvm::isa<ParmVarDecl>(var))) {
        return subobject ? Root::shared() : Root::frame();
      }
    }
    return rootOf(pointer, true, depth);
  }

  bool computeTracked(const VarDecl* var) {
    if (llvm::isa<ParmVarDecl>(var)) return false;
    if (!var->hasLocalStorage() && !var->isInitCapture()) return false;
    if (!isBorrowType(var->getType())) return false;
    // A lambda that captures a reference holds nothing that keeps its target alive.
    if (var->isInitCapture()) return var->getType()->isReferenceType();
    if (var->isImplicit()) {
      // Of a range-for's hidden variables, the iterator is the one that is used after the body.
      auto name = var->getName();
      if (!name.starts_with("__begin")) {
        return name.starts_with("__range") && rootOf(var->getInit(), false).tracked();
      }
    }
    return var->hasInit() && rootOf(var->getInit(), false).tracked();
  }
};

// Collects what a function body does itself, without the bodies of the lambdas it creates, which
// run later or through a call the analysis sees separately, and without unevaluated operands
// (`decltype`, `sizeof`, `noexcept`, a `typeid` of a non-polymorphic type), whose calls never run.
// kj::Promise::then() names its continuation's call in a `decltype`, for example.
template <typename Derived>
class BodyVisitor: public RecursiveASTVisitor<Derived> {
 public:
  bool shouldVisitTemplateInstantiations() const {
    return true;
  }
  bool shouldVisitImplicitCode() const {
    return true;
  }
  bool TraverseLambdaExpr(LambdaExpr* lambda) {
    for (Expr* init: lambda->capture_inits()) {
      if (init != nullptr && !this->TraverseStmt(init)) return false;
    }
    return true;
  }
  bool TraverseBlockExpr(BlockExpr*) {
    return true;
  }
  bool TraverseDecltypeTypeLoc(DecltypeTypeLoc, bool) {
    return true;
  }
  bool TraverseDecltypeType(DecltypeType*, bool) {
    return true;
  }
  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr*) {
    return true;
  }
  bool TraverseCXXNoexceptExpr(CXXNoexceptExpr*) {
    return true;
  }
  bool TraverseCXXTypeidExpr(CXXTypeidExpr* expr) {
    return expr->isPotentiallyEvaluated()
        ? RecursiveASTVisitor<Derived>::TraverseCXXTypeidExpr(expr)
        : true;
  }
};

const Expr* receiverOf(const CallExpr* call) {
  if (const auto* member = llvm::dyn_cast<CXXMemberCallExpr>(call)) {
    return member->getImplicitObjectArgument();
  }
  if (const auto* op = llvm::dyn_cast<CXXOperatorCallExpr>(call)) {
    if (op->getOperator() == OO_Call && op->getNumArgs() > 0) return op->getArg(0);
  }
  return nullptr;
}

class UseFinder {
 public:
  UseFinder(CFG& cfg, const Stmt* body, ASTContext& context)
      : sequence(&cfg, body, &context),
        blocks(&cfg, &context),
        entry(&cfg.getEntry()) {
    for (const CFGBlock* block: cfg) {
      const auto* loop = llvm::dyn_cast_or_null<ForStmt>(block->getTerminatorStmt());
      if (loop != nullptr && runsOnce(loop, context) && block->succ_size() == 2) {
        onceLoopExits[loop] = *std::next(block->succ_begin());
      }
    }
  }

  // A use of `var` that can run after `effect` without `var` being bound again in between.
  const DeclRefExpr* find(const CallExpr* effect, const VarDecl* var) {
    const CFGBlock* block = blocks.blockContainingStmt(effect);
    if (block == nullptr) return nullptr;
    visited.clear();
    return search(block, effect, var);
  }

 private:
  tidy::utils::ExprSequence sequence;
  tidy::utils::StmtToBlockMap blocks;
  const CFGBlock* entry;
  llvm::SmallPtrSet<const CFGBlock*, 16> visited;
  // The exit of each loop that runs once, such as the one KJ_CASE_ONEOF expands to.
  llvm::DenseMap<const ForStmt*, const CFGBlock*> onceLoopExits;

  // `for (...; flag; flag = nullptr)`: the increment ends the loop the first time it runs.
  static bool runsOnce(const ForStmt* loop, ASTContext& context) {
    const auto* cond =
        loop->getCond() == nullptr ? nullptr : llvm::dyn_cast<DeclRefExpr>(strip(loop->getCond()));
    const auto* inc = llvm::dyn_cast_or_null<BinaryOperator>(loop->getInc());
    if (cond == nullptr || inc == nullptr || inc->getOpcode() != BO_Assign) return false;
    const auto* lhs = llvm::dyn_cast<DeclRefExpr>(strip(inc->getLHS()));
    return lhs != nullptr && lhs->getDecl() == cond->getDecl() &&
        inc->getRHS()->isNullPointerConstant(context, Expr::NPC_ValueDependentIsNotNull);
  }

  const DeclRefExpr* search(const CFGBlock* block, const CallExpr* effect, const VarDecl* var) {
    if (visited.contains(block)) return nullptr;
    if (effect == nullptr) visited.insert(block);

    llvm::SmallVector<const DeclRefExpr*, 4> uses;
    llvm::SmallVector<const Stmt*, 2> rebinds;
    for (const auto& element: *block) {
      auto stmt = element.getAs<CFGStmt>();
      if (!stmt) continue;
      collect(stmt->getStmt(), var, uses, rebinds);
    }

    // Bindings that come after the effect end the borrow's life on this path.
    llvm::SmallVector<const Stmt*, 2> live;
    for (const Stmt* rebind: rebinds) {
      if (effect == nullptr || sequence.potentiallyAfter(rebind, effect)) live.push_back(rebind);
    }

    for (const DeclRefExpr* use: uses) {
      if (effect != nullptr && !sequence.potentiallyAfter(use, effect)) continue;
      bool rebound = false;
      for (const Stmt* rebind: live) {
        if (!sequence.potentiallyAfter(rebind, use)) rebound = true;
      }
      if (!rebound) return use;
    }

    if (live.empty()) {
      // The back edge of a loop that runs once leads only out of the loop.
      if (const auto* loop = llvm::dyn_cast_or_null<ForStmt>(block->getLoopTarget())) {
        if (auto exit = onceLoopExits.find(loop); exit != onceLoopExits.end()) {
          return exit->second == nullptr ? nullptr : search(exit->second, nullptr, var);
        }
      }
      for (const auto& succ: block->succs()) {
        if (succ) {
          if (const auto* found = search(succ, nullptr, var)) return found;
        }
      }
    }
    return nullptr;
  }

  static void collect(const Stmt* stmt,
      const VarDecl* var,
      llvm::SmallVectorImpl<const DeclRefExpr*>& uses,
      llvm::SmallVectorImpl<const Stmt*>& rebinds) {
    struct Collector: BodyVisitor<Collector> {
      const VarDecl* var;
      llvm::SmallVectorImpl<const DeclRefExpr*>* uses;
      llvm::SmallVectorImpl<const Stmt*>* rebinds;
      llvm::SmallPtrSet<const DeclRefExpr*, 2> assigned;
      bool VisitDeclRefExpr(DeclRefExpr* ref) {
        if (ref->getDecl() == var && !assigned.contains(ref)) uses->push_back(ref);
        return true;
      }
      bool VisitDeclStmt(DeclStmt* decl) {
        for (const auto* d: decl->decls()) {
          if (d == var) rebinds->push_back(decl);
        }
        return true;
      }
      bool VisitBinaryOperator(BinaryOperator* op) {
        if (op->getOpcode() == BO_Assign) {
          if (const auto* lhs = llvm::dyn_cast<DeclRefExpr>(strip(op->getLHS()));
              lhs != nullptr && lhs->getDecl() == var) {
            // Assigning to the borrow binds it again rather than using it.
            assigned.insert(lhs);
            rebinds->push_back(op);
          }
        }
        return true;
      }
    } collector;
    collector.var = var;
    collector.uses = &uses;
    collector.rebinds = &rebinds;
    collector.TraverseStmt(const_cast<Stmt*>(stmt));
  }
};

}  // namespace

void JsReentryCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
  using namespace ast_matchers;
  Finder->addMatcher(
      functionDecl(isDefinition(), unless(isExpansionInSystemHeader())).bind("function"), this);
  Finder->addMatcher(lambdaExpr(unless(isExpansionInSystemHeader())).bind("lambda"), this);
}

void JsReentryCheck::onStartOfTranslationUnit() {
  mayRunJsCache.clear();
  activeDepth.clear();
  provisionalFalse.clear();
  provisionalDependency.clear();
  lowestActiveDependency = UINT_MAX;
}

bool JsReentryCheck::mayRunJs(const FunctionDecl* function) {
  if (function == nullptr) return false;
  const FunctionDecl* key = function->getCanonicalDecl();
  auto it = mayRunJsCache.find(key);
  if (it != mayRunJsCache.end()) return it->second;

  if (auto provisional = provisionalDependency.find(key);
      provisional != provisionalDependency.end()) {
    // Decided `false` earlier under a caller that is still undecided. Walking it again would reach
    // the same answer, since nothing it depends on has been decided since.
    lowestActiveDependency = std::min(lowestActiveDependency, provisional->second);
    return false;
  }

  if (auto active = activeDepth.find(key); active != activeDepth.end()) {
    // A call back into a function still being decided. Treat it as not running JS for now, and
    // note how far up the stack the answer depends.
    lowestActiveDependency = std::min(lowestActiveDependency, active->second);
    return false;
  }

  if (isAnnotated(function, NO_JS)) {
    mayRunJsCache[key] = false;
    return false;
  }
  if (isAnnotated(function, MAY_RUN_JS) || takesLock(function) ||
      v8EntryPoints().contains(function->getQualifiedNameAsString())) {
    mayRunJsCache[key] = true;
    return true;
  }

  const FunctionDecl* definition = nullptr;
  if (!function->hasBody(definition) || definition->getBody() == nullptr) {
    mayRunJsCache[key] = false;
    return false;
  }

  unsigned depth = activeDepth.size();
  activeDepth[key] = depth;
  size_t provisionalMark = provisionalFalse.size();
  unsigned outerDependency = lowestActiveDependency;
  lowestActiveDependency = UINT_MAX;

  struct Finder: BodyVisitor<Finder> {
    JsReentryCheck* check;
    bool found = false;
    bool VisitCallExpr(CallExpr* call) {
      if (check->mayRunJs(call->getDirectCallee())) {
        found = true;
        return false;
      }
      return true;
    }
  } finder;
  finder.check = this;
  finder.TraverseStmt(definition->getBody());
  activeDepth.erase(key);

  // A `false` that relied on a caller still being decided is provisional, since that caller can
  // still turn out to run JavaScript. It is reused until that caller is decided.
  bool provisional = !finder.found && lowestActiveDependency < depth;
  if (provisional) {
    provisionalFalse.push_back(key);
    provisionalDependency[key] = lowestActiveDependency;
  } else {
    mayRunJsCache[key] = finder.found;
    // Every provisional `false` decided under this function depended only on functions at this
    // depth or deeper, all of which are now decided. If none ran JavaScript, neither do they.
    for (size_t i = provisionalMark; i < provisionalFalse.size(); ++i) {
      if (!finder.found) mayRunJsCache[provisionalFalse[i]] = false;
      provisionalDependency.erase(provisionalFalse[i]);
    }
    provisionalFalse.resize(provisionalMark);
  }
  lowestActiveDependency =
      provisional ? std::min(outerDependency, lowestActiveDependency) : outerDependency;
  return finder.found;
}

void JsReentryCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
  if (const auto* lambda = Result.Nodes.getNodeAs<LambdaExpr>("lambda")) {
    const auto* op = lambda->getCallOperator();
    if (op == nullptr || lambda->getBody() == nullptr) return;
    if (!op->isDependentContext()) {
      checkBody(op, lambda->getBody(), *Result.Context);
    } else if (const auto* generic = lambda->getDependentCallOperator()) {
      // A generic lambda's calls resolve only once its parameter types are known, so check each
      // instantiation of its call operator.
      for (const auto* instance: generic->specializations()) {
        if (instance->hasBody()) checkBody(instance, instance->getBody(), *Result.Context);
      }
    }
    return;
  }
  const auto* function = Result.Nodes.getNodeAs<FunctionDecl>("function");
  if (function == nullptr || function->isDependentContext() || !function->hasBody()) return;
  if (const auto* method = llvm::dyn_cast<CXXMethodDecl>(function);
      method != nullptr && method->getParent()->isLambda()) {
    return;
  }
  checkBody(function, function->getBody(), *Result.Context);
}

void JsReentryCheck::checkBody(
    const FunctionDecl* function, const Stmt* body, ASTContext& context) {
  BorrowAnalysis borrows;

  struct Collector: BodyVisitor<Collector> {
    JsReentryCheck* check;
    BorrowAnalysis* borrows;
    llvm::SmallVector<const VarDecl*, 8> vars;
    llvm::SmallVector<const CallExpr*, 8> effects;
    bool VisitVarDecl(VarDecl* var) {
      if (borrows->isTracked(var)) vars.push_back(var);
      return true;
    }
    bool VisitCallExpr(CallExpr* call) {
      if (check->mayRunJs(call->getDirectCallee())) effects.push_back(call);
      return true;
    }
  } collector;
  collector.check = this;
  collector.borrows = &borrows;
  collector.TraverseStmt(const_cast<Stmt*>(body));
  if (const auto* method = llvm::dyn_cast<CXXMethodDecl>(function);
      method != nullptr && method->getParent()->isLambda()) {
    for (const auto& capture: method->getParent()->captures()) {
      if (capture.capturesVariable()) {
        if (const auto* var = llvm::dyn_cast<VarDecl>(capture.getCapturedVar());
            var != nullptr && var->isInitCapture() && borrows.isTracked(var)) {
          collector.vars.push_back(var);
        }
      }
    }
  }
  if (collector.effects.empty()) return;

  // A call that can run JavaScript made on a borrow can free its own receiver.
  llvm::SmallPtrSet<const VarDecl*, 8> reported;
  for (const CallExpr* effect: collector.effects) {
    const Expr* receiver = receiverOf(effect);
    if (receiver == nullptr) continue;
    if (isFunctionObjectCall(effect)) continue;
    Root root = borrows.rootOf(receiver, false);
    if (root.kind != Root::BORROW || root.borrow->isImplicit()) continue;
    if (!reported.insert(root.borrow).second) continue;
    diag(effect->getExprLoc(),
        "%0 borrows JSG-owned state, and this call on it can run JavaScript that frees it while "
        "the call is running; hold a strong reference (addRef()) instead")
        << root.borrow << effect->getSourceRange();
  }

  if (collector.vars.empty()) return;

  CFG::BuildOptions options;
  options.AddImplicitDtors = true;
  options.AddTemporaryDtors = true;
  std::unique_ptr<CFG> cfg = CFG::buildCFG(nullptr, const_cast<Stmt*>(body), &context, options);
  if (!cfg) return;
  UseFinder finder(*cfg, body, context);

  for (const VarDecl* var: collector.vars) {
    if (reported.contains(var)) continue;
    for (const CallExpr* effect: collector.effects) {
      const DeclRefExpr* use = finder.find(effect, var);
      if (use == nullptr) continue;
      if (var->isImplicit()) {
        diag(use->getBeginLoc(),
            "this loop iterates JSG-owned state across a call that can run JavaScript, which can "
            "change or free it; iterate a copy, or strong references to the elements");
      } else {
        diag(use->getBeginLoc(),
            "%0 borrows JSG-owned state and is used after a call that can run JavaScript, which "
            "can free it; hold a strong reference (addRef()), or look it up again after the call")
            << var;
      }
      diag(effect->getExprLoc(), "JavaScript can run here", DiagnosticIDs::Note)
          << effect->getSourceRange();
      reported.insert(var);
      break;
    }
  }
}

}  // namespace workerd::clang_tidy
