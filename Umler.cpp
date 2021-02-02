#include <algorithm>
#include <cassert>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/Basic/LLVM.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include "DB.h"
#include "Report.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

namespace {
// Set up the command line options
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::OptionCategory UmlerCategory("tool-template options");
static cl::list<std::string> ClassName("c", cl::desc("Class Name"),
                                       cl::cat(UmlerCategory));
static cl::opt<std::string> DBPath("d", cl::desc("path to result database"),
                                   cl::init(":memory:"),
                                   cl::cat(UmlerCategory));
static cl::opt<bool> DocumentUses("document-uses",
                                  cl::desc("show uses relationships"),
                                  cl::init(false), cl::cat(UmlerCategory));
static cl::opt<bool> DocumentOwns("document-owns",
                                  cl::desc("show owns relationships"),
                                  cl::init(false), cl::cat(UmlerCategory));
static cl::opt<bool> DocumentBinds("document-binds",
                                   cl::desc("show binds relationships"),
                                   cl::init(false), cl::cat(UmlerCategory));
static cl::opt<bool> DocumentMethods("document-methods",
                                     cl::desc("show class methods"),
                                     cl::init(true), cl::cat(UmlerCategory));

struct BaseCallbackData {
  const CXXRecordDecl *Derived;
  const DB *Db;
};

/// obtain name from some class-like entity
///
/// declared as a template to inhibit conversion
/// @param v the class-like entity to work on
template <typename T> std::string className(const T &V);
template <> std::string className<CXXRecordDecl>(const CXXRecordDecl &);
template <> std::string className<QualType>(const QualType &);
template <> std::string className<TemplateArgument>(const TemplateArgument &);

template <> std::string className<CXXRecordDecl>(const CXXRecordDecl &Cl) {
  if (const auto *const T =
          dyn_cast_or_null<ClassTemplateSpecializationDecl>(&Cl)) {
    std::string Name = Cl.getNameAsString();
    Name += "<";
    const auto &Args = T->getTemplateArgs();

    for (unsigned I = 0; I < Args.size(); ++I) {
      if (I > 0)
        Name += ", ";
      Name += className(Args.get(I));
    }
    Name += ">";

    return Name;
  }

  return Cl.getNameAsString();
}
template <> std::string className<QualType>(const QualType &T) {
  const auto &Tp = T.getTypePtrOrNull();
  if (Tp->isReferenceType() or Tp->isPointerType()) {
    return className(Tp->getPointeeType());
  } else if (auto *const Cl = Tp->getAsCXXRecordDecl()) {
    return className(*Cl);
  }
  return T.getAsString();
}
template <> std::string className<TemplateArgument>(const TemplateArgument &A) {
  switch (A.getKind()) {
  case clang::TemplateArgument::Null:
    return "NULL";
  case clang::TemplateArgument::Type:
    return className(A.getAsType());
  case clang::TemplateArgument::Integral:
    return std::to_string(*A.getAsIntegral().getRawData());
  default: // FIXME
    llvm::errs() << "No idea how to print TemplateArgument kind " +
                        std::to_string(A.getKind()) + "\n";
    return "";
  }
}

bool recordClass(const CXXRecordDecl *Cl, const DB &Db) {
  const auto ClassName = className(*Cl);
  if (ClassName.empty())
    return false;

  std::string NsName = "";
  {
    const auto *Context = Cl->getEnclosingNamespaceContext();
    while (not Context->isFileContext()) {
      if (const auto *const Ns = dyn_cast<NamespaceDecl>(Context)) {
        const auto Name = Ns->getNameAsString();
        NsName = Name + "::";
        Context = Context->getEnclosingNamespaceContext();
      } else {
        break;
      }
    }
    NsName = NsName.substr(0, NsName.size() - 2);
  }

  Db.execute("INSERT OR IGNORE INTO classes (name, namespace) VALUES ('" +
             ClassName + "','" + NsName + "');");

  if (const auto *const Inst =
          dyn_cast_or_null<ClassTemplateSpecializationDecl>(Cl)) {
    // extract a string for the template parameters
    std::string TmplArgs = "";
    if (auto *const Tmpl = Inst->getSpecializedTemplate()) {
      if (auto *const TmplPara = Tmpl->getTemplateParameters()) {
        for (unsigned I = 0; I < TmplPara->size(); ++I) {
          if (I > 0) {
            TmplArgs += ", ";
          }
          TmplArgs += TmplPara->getParam(I)->getNameAsString();
        }
      }
    }
    Db.execute("INSERT OR IGNORE INTO template_inst (instance, template, "
               "template_args)"
               "VALUES ('" +
               ClassName + "','" + Inst->getNameAsString() + "','" + TmplArgs +
               "')");
  }

  for (auto &&Method : Cl->methods()) {
    if (Method->isImplicit())
      continue;
    const auto &MethodName = Method->getNameAsString();
    const auto &ReturnType = Method->getReturnType();
    const auto &Returns = className(ReturnType);
    // do never document boring void types
    if (not ReturnType->isVoidType() and not ReturnType->isVoidPointerType()) {
      Db.execute("INSERT OR IGNORE INTO uses(user, object) VALUES ('" +
                 ClassName + "','" + Returns + "')");
    }

    const auto Access = std::to_string(Method->getAccess());

    std::string Parameters;
    for (unsigned I = 0; I < Method->getNumParams(); ++I) {
      const auto &Param = Method->getParamDecl(I);
      if (I > 0)
        Parameters += ", ";
      Parameters +=
          className(Param->getType()) + " " + Param->getNameAsString();
      Db.execute("INSERT OR IGNORE INTO uses(user, object) VALUES ('" +
                 ClassName + "','" + className(Param->getType()) + "')");
    }

    const std::string IsStatic = std::to_string(Method->isStatic());
    const std::string IsAbstract = std::to_string(Method->isPure());

    if (not Db.execute("INSERT OR IGNORE INTO methods (class, name, returns, "
                       "parameters, access, static, abstract) VALUES ('" +
                       ClassName + "','" + MethodName + "','" + Returns +
                       "','" + Parameters + "'," + Access + "," + IsStatic +
                       "," + IsAbstract + ");"))
      return false;
  }

  // record member variables
  for (auto &&Field : Cl->fields()) {
    if (Field->isImplicit())
      continue;
    if (auto *const D = Field->getType()->getAsCXXRecordDecl()) {
      const auto &FieldType = className(*D);
      const auto &FieldName = Field->getNameAsString();

      if (not Db.execute(
              "INSERT OR IGNORE INTO owns (owner, object, name) VALUES ('" +
              className(*Cl) + "','" + FieldType + "','" + FieldName + "')"))
        return false;
    }
  }

  return true;
}

bool recordBases(const CXXRecordDecl *Base, const BaseCallbackData &Data) {
  const auto *const Derived = Data.Derived;
  assert(Derived);

  recordClass(Base, *Data.Db);

  const auto IsDirectBase = [](const CXXRecordDecl *Base,
                               const CXXRecordDecl *Derived) {
    return std::find_if(Derived->bases_begin(), Derived->bases_end(),
                        [&Base](const CXXBaseSpecifier &BaseSp) {
                          const auto &Name1 =
                              BaseSp.getType()->getAsCXXRecordDecl()->getName();
                          const auto &Name2 = Base->getName();
                          return Name1 == Name2;
                        }) != Derived->bases_end();
  };

  if (IsDirectBase(Base, Derived)) {
    Data.Db->execute(
        "INSERT OR IGNORE INTO inheritance (derived, base) VALUES ('" +
        className(*Derived) + "','" + className(*Base) + "')");
    const auto NewData = BaseCallbackData{Base, Data.Db};
    Base->forallBases([&NewData](const CXXRecordDecl *Base) {
      return recordBases(Base, NewData);
    });
  }

  return true;
}

void walkHierarchy(const CXXRecordDecl *Derived, const DB &Db) {
  const auto Data = BaseCallbackData{Derived, &Db};
  Derived->forallBases(
      [&Data](const CXXRecordDecl *Base) { return recordBases(Base, Data); });

  recordClass(Derived, Db);
}

class UmlerCallback : public MatchFinder::MatchCallback {
public:
  explicit UmlerCallback(const DB &Db) : Db(Db) {}

  void run(const MatchFinder::MatchResult &Result) override {
    const auto *const Node = Result.Nodes.getNodeAs<CXXRecordDecl>("node");

    walkHierarchy(Node, Db);
  }

  // private:
  const DB &Db;
};

/// helper for build_nested_namespace_matchers
template <typename Iterable>
auto helperBuildNestedNamespaceMatchers(const StringRef &Head,
                                        const Iterable &Tail)
    -> decltype(namespaceDecl()) {
  if (not Tail.size()) { // bottom condition
    return namespaceDecl(hasName(Head.str()));
  }

  const auto &NewHead = *Tail.begin();
  const auto NewTail = Iterable(std::next(Tail.begin()), Tail.end());

  return namespaceDecl(
      hasName(Head.str()),
      hasAncestor(helperBuildNestedNamespaceMatchers(NewHead, NewTail)));
}

/// build a matcher from a list of namespaces
//
/// e.g. given a list {"n1", "n2", "n3"} this would match
/// namespace n1 { namespace n2 { namespace n3 /* MATCH */ { } } }
///
/// @param namespaces a non-empty iterable of namespace name strings
/// @pre namespaces is not empty
/// @returns a matcher for the most nested name
template <typename Iterable>
auto buildNestedNamespaceMatchers(const Iterable &Namespaces)
    -> decltype(namespaceDecl()) {
  // we iterate over the names reversed to build the matcher from the bottom up
  const auto &Head = *Namespaces.rbegin();
  const auto Tail = Iterable(std::next(Namespaces.rbegin()), Namespaces.rend());

  return helperBuildNestedNamespaceMatchers(Head, Tail);
}

/// extract the namespaces in some class name
/// @param full_name the class name, possibly including namespaces
SmallVector<StringRef, 1000>
extractNamespaceComponents(const StringRef &FullName) {
  decltype(extractNamespaceComponents("")) Namespaces;

  // we split at `::`
  FullName.split(Namespaces, "::");

  // last element is always a class name
  Namespaces.pop_back();

  // remove empty ns names, e.g. from a ::ns::ClassName
  Namespaces.erase(std::remove_if(Namespaces.begin(), Namespaces.end(),
                                  [](const StringRef &S) { return S.empty(); }),
                   Namespaces.end());

  return Namespaces;
}

} // end anonymous namespace

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  auto OptionsParser = CommonOptionsParser::create(argc, argv, UmlerCategory);

  if (auto Error = OptionsParser.takeError()) {
    llvm::errs() << "Could not parse options";
  }

  RefactoringTool Tool(OptionsParser->getCompilations(),
                       OptionsParser->getSourcePathList());
  ast_matchers::MatchFinder Finder;

  const auto Db = DB{DBPath.getValue()};
  auto Callback = UmlerCallback{Db};

  if (ClassName.empty()) {
    Finder.addMatcher(
        recordDecl(anything(), isDefinition(), unless(isImplicit()))
            .bind("node"),
        &Callback);
  } else {
    for (const auto &Name : ClassName) {
      const auto Namespaces = extractNamespaceComponents(Name);
      if (not Namespaces.size()) {
        Finder.addMatcher(
            recordDecl(hasName(Name), isDefinition(), unless(isImplicit()))
                .bind("node"),
            &Callback);
      } else {
        auto NamespaceMatcher = buildNestedNamespaceMatchers(Namespaces);
        Finder.addMatcher(recordDecl(hasName(Name), isDefinition(),
                                     unless(isImplicit()),
                                     hasAncestor(NamespaceMatcher))
                              .bind("node"),
                          &Callback);
      }
    }
  }

  const auto FrontendResult = Tool.run(newFrontendActionFactory(&Finder).get());

  report(Db, {.DocumentOwns = DocumentOwns.getValue(),
              .DocumentUses = DocumentUses.getValue(),
              .DocumentBinds = DocumentBinds.getValue(),
              .DocumentMethods = DocumentMethods.getValue()});

  return FrontendResult;
}
