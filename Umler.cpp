#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/VariadicFunction.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"

#include <algorithm>
#include <memory>
#include <string>

#include <cassert>

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
  const CXXRecordDecl* derived;
  const DB* db;
};

template <typename T> std::string className(const T &);
template <> std::string className<CXXRecordDecl>(const CXXRecordDecl&);
template <> std::string className<QualType>(const QualType&);
template <> std::string className<TemplateArgument>(const TemplateArgument&);

template <> std::string className<CXXRecordDecl>(const CXXRecordDecl &cl) {
  if (const auto t = dyn_cast_or_null<ClassTemplateSpecializationDecl>(&cl)) {
    std::string name = cl.getNameAsString();
    name += "<";
    const auto &args = t->getTemplateArgs();

    for (unsigned i = 0; i < args.size(); ++i) {
      if (i > 0)
        name += ", ";
      name += className(args.get(i));
    }
    name += ">";

    return name;
  }

  return cl.getNameAsString();
}
template <> std::string className<QualType>(const QualType &t) {
  const auto &tp = t.getTypePtrOrNull();
  if (tp->isReferenceType() or tp->isPointerType()) {
    return className(tp->getPointeeType());
  } else if (const auto cl = tp->getAsCXXRecordDecl()) {
    return className(*cl);
  }
  return t.getAsString();
}
template <> std::string className<TemplateArgument>(const TemplateArgument &a) {
  switch (a.getKind()) {
  case clang::TemplateArgument::Null:
    return "NULL";
  case clang::TemplateArgument::Type:
    return className(a.getAsType());
  case clang::TemplateArgument::Integral:
    return std::to_string(*a.getAsIntegral().getRawData());
  default: // FIXME
    llvm::errs() << "No idea how to print TemplateArgument kind " +
                        std::to_string(a.getKind()) + "\n";
    return "";
  }
}

bool recordClass(const CXXRecordDecl *cl, const DB &db) {
  const auto class_name = className(*cl);
  if (class_name.empty())
    return false;

  std::string ns_name = "";
  {
    auto context = cl->getEnclosingNamespaceContext();
    while (true) {
      const auto name = dyn_cast<NamespaceDecl>(context)->getNameAsString();
      ns_name = name + "::";
      if (context == cl->getEnclosingNamespaceContext())
        break;
      context = context->getEnclosingNamespaceContext();
    }
    ns_name = ns_name.substr(0, ns_name.size() - 2);
  }

  db.execute("INSERT OR IGNORE INTO classes (name, namespace) VALUES ('" +
             class_name + "','" + ns_name + "');");

  if (const auto inst = dyn_cast_or_null<ClassTemplateSpecializationDecl>(cl)) {
    // extract a string for the template parameters
    std::string tmpl_args = "";
    if (const auto tmpl = inst->getSpecializedTemplate()) {
      if (const auto tmpl_para = tmpl->getTemplateParameters()) {
        for (unsigned i = 0; i < tmpl_para->size(); ++i) {
          if (i > 0) {
            tmpl_args += ", ";
          }
          tmpl_args += tmpl_para->getParam(i)->getNameAsString();
        }
      }
    }
    db.execute("INSERT OR IGNORE INTO template_inst (instance, template, "
               "template_args)"
               "VALUES ('" +
               class_name + "','" + inst->getNameAsString() + "','" +
               tmpl_args + "')");
  }

  for (const auto &method : cl->methods()) {
    if (method->isImplicit())
      continue;
    const auto &method_name = method->getNameAsString();
    const auto& return_type = method->getReturnType();
    const auto & returns = className(return_type);
    // do never document boring void types
    if (not return_type->isVoidType() and
        not return_type->isVoidPointerType()) {
      db.execute("INSERT OR IGNORE INTO uses(user, object) VALUES ('" +
                 class_name + "','" + returns + "')");
    }

    const auto access = std::to_string(method->getAccess());

    std::string parameters;
    for (unsigned i = 0; i < method->getNumParams(); ++i) {
      const auto &param = method->getParamDecl(0);
      if (i > 0)
        parameters += ", ";
      parameters +=
          className(param->getType()) + " " + param->getNameAsString();
      db.execute("INSERT OR IGNORE INTO uses(user, object) VALUES ('" +
                 class_name + "','" + className(param->getType()) + "')");
    }

    const std::string is_static = std::to_string(method->isStatic());
    const std::string is_abstract = std::to_string(method->isPure());

    if (not db.execute("INSERT OR IGNORE INTO methods (class, name, returns, "
                       "parameters, access, static, abstract) VALUES ('" +
                       class_name + "','" + method_name + "','" + returns +
                       "','" + parameters + "'," + access + "," + is_static +
                       "," + is_abstract + ");"))
      return false;
  }

  // record member variables
  for (const auto &field : cl->fields()) {
    if (field->isImplicit())
      continue;
    if (const auto d = field->getType()->getAsCXXRecordDecl()) {
      const auto& field_type = className(*d);
      const auto& field_name = field->getNameAsString();

      if (not db.execute(
              "INSERT OR IGNORE INTO owns (owner, object, name) VALUES ('" +
              className(*cl) + "','" + field_type + "','" + field_name + "')"))
        return false;
    }
  }

  return true;
}

bool recordBases(const CXXRecordDecl *base, void *p) {
  const auto data = reinterpret_cast<const BaseCallbackData*>(p);
  const auto derived = data->derived;
  assert(derived);

  recordClass(base, *data->db);

  const auto is_direct_base = [](const CXXRecordDecl *base,
                                 const CXXRecordDecl *derived) {
    return std::find_if(
               derived->bases_begin(), derived->bases_end(),
               [&base, &derived](const CXXBaseSpecifier &base_sp) {
                 const auto &name1 =
                     base_sp.getType()->getAsCXXRecordDecl()->getName();
                 const auto &name2 = base->getName();
                 return name1 == name2;
               }) != derived->bases_end();
  };

  if (is_direct_base(base, derived)) {
    data->db->execute(
        "INSERT OR IGNORE INTO inheritance (derived, base) VALUES ('" +
        className(*derived) + "','" + className(*base) + "')");
    const auto new_data = BaseCallbackData{base, data->db};
    base->forallBases(recordBases, const_cast<BaseCallbackData *>(&new_data));
  }

  return true;
}

void walkHierarchy(const CXXRecordDecl *derived, const DB &db) {
  const auto data = BaseCallbackData{derived, &db};
  derived->forallBases(recordBases, const_cast<BaseCallbackData *>(&data));

  recordClass(derived, db);
}

class UmlerCallback : public MatchFinder::MatchCallback {
public:
  explicit UmlerCallback(const DB &db) : db(db) {}

  void run(const MatchFinder::MatchResult &Result) override {
    const auto node = Result.Nodes.getNodeAs<CXXRecordDecl>("node");

    walkHierarchy(node, db);
  }

// private:
  const DB& db;
};

} // end anonymous namespace

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal();
  CommonOptionsParser OptionsParser(argc, argv, UmlerCategory);
  RefactoringTool Tool(OptionsParser.getCompilations(),
                       OptionsParser.getSourcePathList());
  ast_matchers::MatchFinder Finder;

  const auto db = DB{DBPath.getValue()};
  auto Callback = UmlerCallback{db};

  if (ClassName.empty()) {
    Finder.addMatcher(
        recordDecl(anything(), isDefinition(), unless(isImplicit()))
            .bind("node"),
        &Callback);
  } else {
    for (const auto &name : ClassName) {
      Finder.addMatcher(
          recordDecl(hasName(name), isDefinition(), unless(isImplicit()))
          .bind("node"),
          &Callback);
    }
  }

  const auto frontend_result =
      Tool.run(newFrontendActionFactory(&Finder).get());

  report(db, {.documentOwns = DocumentOwns.getValue(),
              .documentUses = DocumentUses.getValue(),
              .documentBinds = DocumentBinds.getValue(),
              .documentMethods = DocumentMethods.getValue()});

  return frontend_result;
}
