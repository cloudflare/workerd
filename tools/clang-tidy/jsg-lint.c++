#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace workerd {
namespace jsglint {

// Anchored suffix match: returns true iff `qualifiedName` equals `suffix` or
// is of the form `<prefix>::<suffix>`. Avoids the substring trap that would
// otherwise match `foo::jsg::Refrigerator` against `jsg::Ref`.
//
// TODO: Replace this with proper scope analysis: resolve each known visitable
// template by qualified name once via Sema::LookupQualifiedName at the first
// MatchFinder callback, cache the TemplateDecl* pointers, and compare those
// directly. Pointer-identity is the correct primitive for AST scope queries;
// suffix matching is a pragmatic shortcut that depends on jsg/kj never having
// `using namespace` aliases that introduce a name into an unrelated namespace.
static bool endsWithQualified(llvm::StringRef qualifiedName,
                              llvm::StringRef suffix) {
  if (qualifiedName == suffix) return true;
  if (qualifiedName.size() < suffix.size() + 2) return false;
  if (!qualifiedName.ends_with(suffix)) return false;
  auto sep = qualifiedName.size() - suffix.size();
  return qualifiedName[sep - 1] == ':' && qualifiedName[sep - 2] == ':';
}

// Visitable leaf templates: each holds a GC root and must be visited.
static const llvm::StringRef kVisitableLeafTemplates[] = {
    "jsg::Ref",      "jsg::V8Ref",   "jsg::JsRef",
    "jsg::Function", "jsg::Promise", "jsg::HashableV8Ref",
    "jsg::MemoizedIdentity",
};

// Non-template visitable leaf types.
static const llvm::StringRef kVisitableLeafTypes[] = {
    "jsg::BufferSource",
    "jsg::Name",
    "jsg::Value",
    "jsg::Data",
};

// Container templates whose visitability is determined by their type
// arguments. `FirstArg` containers visit one element; `AnyArg` containers
// (variants) are visitable if any element type is.
enum class ContainerKind { None, FirstArg, AnyArg };

static const llvm::StringRef kFirstArgContainers[] = {
    "kj::Maybe",  "kj::Array",       "kj::Vector",
    "jsg::Optional", "jsg::LenientOptional",
};

static const llvm::StringRef kAnyArgContainers[] = {
    "kj::OneOf",
};

static ContainerKind getContainerKind(llvm::StringRef qualifiedName) {
  for (auto suffix : kFirstArgContainers) {
    if (endsWithQualified(qualifiedName, suffix)) return ContainerKind::FirstArg;
  }
  for (auto suffix : kAnyArgContainers) {
    if (endsWithQualified(qualifiedName, suffix)) return ContainerKind::AnyArg;
  }
  return ContainerKind::None;
}

// Returns the qualified name of the template (e.g. "workerd::jsg::Ref") if
// `qt` is a template specialization; otherwise an empty string.
static std::string getTemplateQualifiedName(clang::QualType qt) {
  const auto *t = qt.getTypePtr()->getAs<clang::TemplateSpecializationType>();
  if (!t) return "";
  auto *td = t->getTemplateName().getAsTemplateDecl();
  if (!td) return "";
  return td->getQualifiedNameAsString();
}

static bool isVisitableType(clang::QualType qt) {
  if (qt.isNull()) return false;
  qt = qt.getNonReferenceType().getUnqualifiedType();

  // Direct named record type, e.g. `jsg::BufferSource`.
  if (const auto *rt = qt.getTypePtr()->getAs<clang::RecordType>()) {
    auto fqn = rt->getDecl()->getQualifiedNameAsString();
    for (auto suffix : kVisitableLeafTypes) {
      if (endsWithQualified(fqn, suffix)) return true;
    }
  }

  // Template specialization: dispatch on outer template name.
  std::string tmpl = getTemplateQualifiedName(qt);
  if (tmpl.empty()) return false;

  for (auto suffix : kVisitableLeafTemplates) {
    if (endsWithQualified(tmpl, suffix)) return true;
  }

  auto kind = getContainerKind(tmpl);
  if (kind == ContainerKind::None) return false;

  const auto *t = qt.getTypePtr()->getAs<clang::TemplateSpecializationType>();
  if (!t) return false;
  auto args = t->template_arguments();

  if (kind == ContainerKind::FirstArg) {
    if (args.empty()) return false;
    if (args[0].getKind() != clang::TemplateArgument::Type) return false;
    return isVisitableType(args[0].getAsType());
  }
  // AnyArg
  for (const auto &arg : args) {
    if (arg.getKind() == clang::TemplateArgument::Type) {
      if (isVisitableType(arg.getAsType())) return true;
    }
  }
  return false;
}

// Returns true if `filename` looks like a C++ implementation file (.c++ /
// .cpp / .cc / .c). Implementation files have full visibility into the
// out-of-line bodies of methods declared in the headers they include, so they
// are the correct place to validate visitForGc; running against headers alone
// would yield false "no body" diagnostics when the definition lives in a
// sibling .c++ file.
static bool isImplFile(llvm::StringRef filename) {
  return filename.ends_with(".c++") || filename.ends_with(".cpp") ||
         filename.ends_with(".cc") || filename.ends_with(".c");
}

// Returns true if `decl` is lexically nested inside a `namespace jsg` (whose
// fully-qualified name suffix is `::jsg` or which is the top-level `jsg`).
// Used to skip JSG framework internals; the check targets user resource types,
// not the GC primitives the framework itself defines.
static bool isInJsgNamespace(const clang::Decl *decl) {
  for (const auto *ctx = decl->getDeclContext(); ctx; ctx = ctx->getParent()) {
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(ctx)) {
      if (ns->getName() == "jsg") return true;
    }
  }
  return false;
}

class VisitForGcCheck : public clang::tidy::ClangTidyCheck {
 public:
  VisitForGcCheck(clang::StringRef Name, clang::tidy::ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder *Finder) override {
    using namespace clang::ast_matchers;
    // Match every concrete record definition in the TU; we filter inside
    // check(). We also match every FieldDecl so we can compute the set of
    // record types that appear as fields of other records (the "used as
    // member" set), which gates the Option B diagnostic.
    Finder->addMatcher(
        cxxRecordDecl(isDefinition(), unless(isImplicit())).bind("record"),
        this);
    Finder->addMatcher(fieldDecl().bind("field"), this);
  }

  void onStartOfTranslationUnit() override {
    records_.clear();
    usedAsField_.clear();
    holderVisitForGcVisible_.clear();
    transitivelyVisitedFields_.clear();
    sourceManager_ = nullptr;
  }

  void check(const clang::ast_matchers::MatchFinder::MatchResult &Result) override {
    sourceManager_ = Result.SourceManager;

    if (const auto *field = Result.Nodes.getNodeAs<clang::FieldDecl>("field")) {
      recordUsedAsField(field->getType());
      return;
    }

    const auto *record = Result.Nodes.getNodeAs<clang::CXXRecordDecl>("record");
    if (!record) return;

    records_.push_back(record);
  }

  void onEndOfTranslationUnit() override {
    if (sourceManager_ == nullptr) return;

    // Only validate when the primary translation unit is an implementation
    // file; .h passes can only see declarations and would yield false "no
    // body" diagnostics when visitForGc is defined out-of-line in a sibling
    // .c++ that this header pass cannot observe. Each header gets walked from
    // every .c++ that includes it, so coverage is preserved.
    auto mainFile = sourceManager_->getFileEntryRefForID(sourceManager_->getMainFileID());
    if (!mainFile || !isImplFile(mainFile->getName())) return;

    // First pass: walk every visible visitForGc body to populate
    // transitivelyVisitedFields_ and holderVisitForGcVisible_. This lets us
    // (a) recognize when a parent's visitForGc reaches into a nested struct
    // field, and (b) restrict "used-as-field" diagnostics to TUs where some
    // holder's body is actually parseable here.
    for (const auto *record : records_) {
      if (isInJsgNamespace(record)) continue;
      if (record->getDescribedClassTemplate() != nullptr) continue;
      if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(record)) continue;
      if (record->isDependentContext()) continue;
      for (const auto *method : record->methods()) {
        if (method->getNameAsString() != "visitForGc") continue;
        const clang::FunctionDecl *defn = nullptr;
        if (!method->isDefined(defn) || defn == nullptr) continue;
        llvm::DenseSet<const clang::FieldDecl *> unused;
        collectVisitedFields(defn->getBody(), unused);
      }
    }

    // Second pass: emit diagnostics for records that need them.
    for (const auto *record : records_) {
      checkRecord(record);
    }
  }

 private:
  // Records we encountered in this TU and need to evaluate after the field-
  // collection pass completes.
  llvm::SmallVector<const clang::CXXRecordDecl *, 64> records_;

  // CanonicalDecl* of records that appear as a field type somewhere in this
  // TU. We use canonical decls so forward-declared types and their definitions
  // alias; declaration redeclarations point at the same canonical decl.
  llvm::DenseSet<const clang::CXXRecordDecl *> usedAsField_;

  // Records whose holder's visitForGc body is visible in this TU. A struct
  // qualifies for the "used as field" diagnostic only when its holder is
  // analyzable here; otherwise we'd false-positive in every TU that includes
  // the header but not the holder's defining .c++. CanonicalDecl* keys.
  llvm::DenseSet<const clang::CXXRecordDecl *> holderVisitForGcVisible_;

  // (FieldDecl canonical, ...) pairs marking that some outer holder's
  // visitForGc body transitively reaches the given field via a member-access
  // chain (e.g., `visitor.visit(s.func)` where `s` is a struct field).
  // Used to suppress diagnostics on nested structs whose visitable fields are
  // already covered by an enclosing record's visitForGc.
  llvm::DenseSet<const clang::FieldDecl *> transitivelyVisitedFields_;

  const clang::SourceManager *sourceManager_ = nullptr;

  void recordUsedAsField(clang::QualType qt) {
    if (qt.isNull()) return;
    qt = qt.getNonReferenceType().getUnqualifiedType();

    if (const auto *rt = qt.getTypePtr()->getAs<clang::RecordType>()) {
      if (const auto *rd = llvm::dyn_cast<clang::CXXRecordDecl>(rt->getDecl())) {
        usedAsField_.insert(rd->getCanonicalDecl());
      }
    }

    // Recurse into template arguments so kj::Maybe<Impl>, kj::Vector<Impl>,
    // etc. mark Impl as used-as-field.
    if (const auto *t = qt.getTypePtr()->getAs<clang::TemplateSpecializationType>()) {
      for (const auto &arg : t->template_arguments()) {
        if (arg.getKind() == clang::TemplateArgument::Type) {
          recordUsedAsField(arg.getAsType());
        }
      }
    }
  }

  void checkRecord(const clang::CXXRecordDecl *record) {
    // Skip uninstantiated template definitions: primary class templates,
    // partial specializations, and anything dependent. Field types in these
    // are unresolved.
    if (record->getDescribedClassTemplate() != nullptr) return;
    if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(record)) return;
    if (record->isDependentContext()) return;

    // Skip JSG framework internals; the check targets user resource types.
    if (isInJsgNamespace(record)) return;

    llvm::SmallVector<const clang::FieldDecl *, 8> visitableFields;
    for (const auto *field : record->fields()) {
      if (isVisitableType(field->getType())) {
        visitableFields.push_back(field);
      }
    }
    if (visitableFields.empty()) return;

    const clang::CXXMethodDecl *visitMethod = nullptr;
    for (const auto *method : record->methods()) {
      if (method->getNameAsString() == "visitForGc") {
        visitMethod = method;
        break;
      }
    }

    if (!visitMethod) {
      // No visitForGc on the record itself. Decide whether to diagnose:
      //   - If the record participates in JSG visitation (has visitForGc in
      //     a base class, e.g., jsg::Object's empty default), diagnose: the
      //     framework will dispatch to the empty default and miss the
      //     visitable fields. This is local-TU-decidable.
      //   - If the record is used as a field of another record AND some
      //     holder's visitForGc body in this TU reaches into it, diagnose:
      //     we have an authoritative view here, and any field the holder
      //     didn't visit is a real gap. Other TUs that include this header
      //     but not the holder's defining .c++ stay silent for this struct
      //     (deferred to whichever TU is authoritative).
      //   - If used as a field but no holder visitForGc is visible in this
      //     TU, defer — some other TU will be authoritative.
      //   - Standalone struct not held anywhere, no diagnostic.
      bool hasBaseVisitForGc = false;
      for (const auto &base : record->bases()) {
        if (const auto *baseRecord = base.getType()->getAsCXXRecordDecl()) {
          if (baseHasVisitForGc(baseRecord)) {
            hasBaseVisitForGc = true;
            break;
          }
        }
      }
      bool usedAsField = usedAsField_.count(record->getCanonicalDecl()) != 0;
      bool holderVisible =
          holderVisitForGcVisible_.count(record->getCanonicalDecl()) != 0;
      if (!hasBaseVisitForGc && !(usedAsField && holderVisible)) return;

      for (const auto *field : visitableFields) {
        // Suppress when an enclosing record's visitForGc body reaches this
        // field via a member-access chain (e.g., visitor.visit(state.func)
        // covers State::func from NativeHandler's body).
        if (transitivelyVisitedFields_.count(field->getCanonicalDecl())) continue;

        diag(field->getLocation(),
             "field '%0' of visitable type '%1' is not visited in visitForGc "
             "(class has no visitForGc method)")
            << field->getName() << field->getType().getAsString();
      }
      return;
    }

    // The class has visitForGc declared but no body visible in this TU — the
    // out-of-line definition lives in a sibling .c++ that this pass cannot
    // observe. Skip silently; the defining TU will check it.
    const clang::FunctionDecl *defn = nullptr;
    if (!visitMethod->isDefined(defn) || defn == nullptr) return;
    const auto *body = defn->getBody();
    if (!body) return;

    llvm::DenseSet<const clang::FieldDecl *> visitedFields;
    collectVisitedFields(body, visitedFields);

    for (const auto *field : visitableFields) {
      if (!visitedFields.count(field->getCanonicalDecl())) {
        diag(field->getLocation(),
             "field '%0' of visitable type '%1' is not visited in visitForGc")
            << field->getName() << field->getType().getAsString();
      }
    }
  }

  // True if `record` declares its own visitForGc method or transitively
  // inherits one. Uses a visited set to avoid revisiting shared bases in
  // diamond hierarchies (and to defend against malformed cycles).
  static bool baseHasVisitForGc(const clang::CXXRecordDecl *record) {
    llvm::DenseSet<const clang::CXXRecordDecl *> visited;
    return baseHasVisitForGcImpl(record, visited);
  }

  static bool baseHasVisitForGcImpl(
      const clang::CXXRecordDecl *record,
      llvm::DenseSet<const clang::CXXRecordDecl *> &visited) {
    if (record == nullptr) return false;
    record = record->getDefinition();
    if (record == nullptr) return false;
    if (!visited.insert(record->getCanonicalDecl()).second) return false;
    for (const auto *method : record->methods()) {
      if (method->getNameAsString() == "visitForGc") return true;
    }
    for (const auto &base : record->bases()) {
      if (const auto *baseRecord = base.getType()->getAsCXXRecordDecl()) {
        if (baseHasVisitForGcImpl(baseRecord, visited)) return true;
      }
    }
    return false;
  }

  void collectVisitedFields(const clang::Stmt *stmt,
                            llvm::DenseSet<const clang::FieldDecl *> &visitedFields) {
    if (!stmt) return;

    if (auto *memberExpr = llvm::dyn_cast<clang::MemberExpr>(stmt)) {
      auto *memberDecl = memberExpr->getMemberDecl();
      if (auto *fieldDecl = llvm::dyn_cast<clang::FieldDecl>(memberDecl)) {
        if (isVisitableType(fieldDecl->getType())) {
          visitedFields.insert(fieldDecl->getCanonicalDecl());
        }
        // Record this field as "transitively visited" from an enclosing
        // record's perspective. This lets a parent's visitForGc body cover
        // a nested struct's visitable fields without that nested struct
        // having to declare its own visitForGc (the `NativeHandler::State`
        // pattern, where `visitor.visit(state.func)` reaches `State::func`
        // from NativeHandler's body).
        transitivelyVisitedFields_.insert(fieldDecl->getCanonicalDecl());
        // Mark the record this field belongs to as "holder visible here",
        // i.e., we have evidence about whether its fields are visited in
        // this TU. Used to gate the "used-as-field" diagnostic so it only
        // fires in TUs that can authoritatively answer.
        if (const auto *parent = llvm::dyn_cast<clang::CXXRecordDecl>(
                fieldDecl->getParent())) {
          holderVisitForGcVisible_.insert(parent->getCanonicalDecl());
        }
      }
    }

    for (const auto *child : stmt->children()) {
      collectVisitedFields(child, visitedFields);
    }
  }
};

class JsgLintModule : public clang::tidy::ClangTidyModule {
 public:
  void addCheckFactories(clang::tidy::ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<VisitForGcCheck>("jsg-visit-for-gc");
  }
};

static clang::tidy::ClangTidyModuleRegistry::Add<JsgLintModule>
    X("jsg-lint", "Workerd JSG static checks.");

}  // namespace jsglint
}  // namespace workerd
