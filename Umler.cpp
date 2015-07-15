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
#include "clang/Basic/Specifiers.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"

#include <sqlite3.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <cassert>
#include <cstddef>
#include <ext/alloc_traits.h>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

namespace {
// Set up the command line options
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::OptionCategory UmlerCategory("tool-template options");
static cl::opt<std::string> ClassName("c", cl::desc("Class Name"), cl::init(""),
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

class DB {
public:
  explicit DB(const std::string &dbpath) {
    if (sqlite3_open(dbpath.c_str(), &connection) != SQLITE_OK) {
      llvm::errs() << "COULD NOT OPEN DB\n";
      connection = nullptr;
      return;
    }

    if (not execute("CREATE TABLE IF NOT EXISTS classes ("
                    "id INTEGER PRIMARY KEY,"
                    "name TEXT NOT NULL,"
                    "namespace TEXT);") or
        not execute("CREATE UNIQUE INDEX IF NOT EXISTS classes_idx ON "
                    "classes(name, namespace)") or
        not execute("CREATE TABLE IF NOT EXISTS inheritance ("
                    "derived INTEGER REFERENCES classes(id),"
                    "base INTEGER REFERENCES classes(id));") or
        not execute("CREATE UNIQUE INDEX IF NOT EXISTS inheritance_idx ON "
                    "inheritance(derived, base)") or
        not execute("CREATE TABLE IF NOT EXISTS methods ("
                    "class INTEGER REFERENCES classes(id),"
                    "name TEXT NOT NULL,"
                    "returns TEXT,"
                    "parameters TEXT,"
                    "access INTEGER,"
                    "static INTEGER,"
                    "abstract INTEGER);") or
        not execute("CREATE UNIQUE INDEX IF NOT EXISTS methods_idx ON "
                    "methods(class, name, returns, parameters)") or
        not execute("CREATE TABLE IF NOT EXISTS owns ("
                    "owner INTEGER REFERENCES classes(id),"
                    "object INTEGER REFERENCES classes(id),"
                    "name TEXT NOT NULL);") or
        not execute("CREATE UNIQUE INDEX IF NOT EXISTS owns_idx ON "
                    "owns(owner, object, name)") or
        not execute("CREATE TABLE IF NOT EXISTS uses ("
                    "user INTEGER REFERENCES classes(id),"
                    "object INTEGER REFERENCES classes(id))") or
        not execute("CREATE UNIQUE INDEX IF NOT EXISTS uses_idx ON "
                    "uses(user, object)") or
        not execute("CREATE TABLE IF NOT EXISTS template_inst ("
                    "instance INTEGER REFERENCES classes(id),"
                    "template TEXT NOT NULL,"
                    "template_args TEXT);") or
        not execute("CREATE UNIQUE INDEX IF NOT EXISTS template_inst_idx ON "
                    "template_inst(instance, template, template_args)"))
      connection = nullptr;
  }

  ~DB() {
    sqlite3_close(connection);
  }

  bool execute(const std::string &statement) const {
    if (sqlite3_prepare_v2(connection, statement.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
      llvm::errs() << "COULD NOT PREPARE " << statement << "\n";
      llvm::errs() << sqlite3_errmsg(connection) << "\n";
      return false;
    }

    rows.clear();
    step();

    return true;
  }

  mutable std::vector<std::vector<std::string>> rows;

  sqlite3 *connection;

private:
  mutable sqlite3_stmt *stmt;

  bool step() const {
    const auto rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      return true;
    }

    if (rc == SQLITE_ROW) {
      std::vector<std::string> result;
      for (int column = 0; column < sqlite3_column_count(stmt); ++column) {
        result.emplace_back(
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, column)));
      }
      rows.emplace_back(std::move(result));
      return step();
    }

    return false;
  }
};

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
    llvm::errs() << "No idea how to print TeampleArgument kind " +
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
    const auto& returns = className(method->getReturnType());
    db.execute("INSERT OR IGNORE INTO uses(user, object) VALUES ('" +
               class_name + "','" + returns + "')");

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

enum ReportType { dot, sqlite, plantuml };

template <ReportType> void reportBegin(const DB &) {}
template <ReportType> void reportEnd(const DB &) {}
template <ReportType> void reportClasses(const DB &) {}
template <ReportType> void reportInheritance(const DB &) {}

template <> void reportBegin<plantuml>(const DB &) {
  llvm::outs() << "@startuml\n\n"
                  "skinparam class {\n"
                  "  BackgroundColor White\n"
                  "  ArrowColor Black\n"
                  "  BorderColor DimGrey\n"
                  "}\n"
                  "hide circle\n"
                  "hide empty attributes\n\n";
}
template <> void reportEnd<plantuml>(const DB &) {
  llvm::outs() << "\n@enduml\n";
}
template <> void reportClasses<plantuml>(const DB &db) {
  db.execute("SELECT DISTINCT namespace FROM classes");
  const auto namespace_rows = db.rows;

  for (size_t i = 0; i < namespace_rows.size(); ++i) {
    const auto &ns = namespace_rows[i][0];
    if (not db.execute("SELECT name FROM classes WHERE namespace = '" + ns +
                       "'"))
      return;
    const auto class_rows = db.rows;
    for (const auto& row: class_rows) {
      const auto& class_ = row[0];
      llvm::outs() << "class \"" + class_ + "\" {\n";

      db.execute("SELECT name, parameters, returns, access, static, abstract FROM methods WHERE class='" +
                 class_ + "'");
      for (const auto& method : db.rows) {
        std::string access = "";
        switch (std::stoi(method[3])) {
        case AS_public: {
          access = "+";
        } break;
        case AS_private: {
          access = "-";
        } break;
        case AS_protected: {
          access = "#";
        } break;
        case AS_none:
          break;
        }

        const std::string is_static = std::stoi(method[4]) ? "{static}" : "";
        const std::string is_abstract = std::stoi(method[5]) ? "{abstract}" : "";
        const auto modifiers = is_static + is_abstract;

        const auto returns = method[2] == "void" ? "" : method[2];
        llvm::outs() << "  " + access + returns + " " + method[0] + "(" +
                            method[1] + ")" + " " + modifiers + "\n";
      }
      llvm::outs() << "}\n";

      // show "owns" relationships
      if (DocumentOwns.getValue()) {
        db.execute("SELECT object, name FROM owns WHERE owner ='" + class_ +
                   "'");
        for (const auto &row : db.rows)
          llvm::outs() << "\"" + class_ + "\" o-- \"" + row[0] + "\" : \"" +
                              row[1] + "\"\n";
      }

      // show "uses" relationships
      if (DocumentUses.getValue()) {
        db.execute("SELECT object FROM uses WHERE user ='" + class_ + "'");
        for (const auto &row : db.rows) {
          llvm::outs() << "\"" + class_ + "\" --> \"" + row[0] + "\"\n";
        }
      }
    }

    // show "binds" relationships
    if (DocumentBinds.getValue()) {
      db.execute("SELECT DISTINCT template, template_args FROM template_inst");
      const auto template_rows = db.rows;

      for (const auto& template_ : template_rows) {
        llvm::outs() << "class \"" + template_[0] + "\"<" + template_[1] +
                            "> {\n}\n";

        db.execute("SELECT instance FROM template_inst WHERE instance = '" +
                   template_[0] + "'");
        for (const auto& row: db.rows) {
          llvm::outs() << "\"" + row[0] + "\" ..|> \"" + template_[0] +
                              "\" : <<bind>>\n";
        }
      }
    }
  }
}

template <> void reportInheritance<plantuml>(const DB &db) {
  if (not db.execute("SELECT derived, base FROM inheritance"))
    return;
  for (const auto& row: db.rows) {
    assert(row.size() == 2);
    llvm::outs() << "\"" + row[0] << "\" --|> \"" << row[1] << "\"\n";
  }
}

template <> void reportBegin<dot>(const DB&) {
  llvm::outs() << "digraph G {\n";
}

template <> void reportEnd<dot>(const DB&) {
  llvm::outs() << "}\n";
}

template <> void reportInheritance<dot>(const DB &db) {
  if (not db.execute("SELECT derived, base FROM inheritance"))
    return;

  for (const auto& row : db.rows ) {
    assert(row.size() == 2);

    llvm::outs() << row[0] << " -> " << row[1] << "\n";
  }
}

template <> void reportClasses<dot>(const DB &db) {
  if (not db.execute("SELECT DISTINCT namespace FROM classes"))
    return;

  const auto namespace_rows = db.rows;

  for (size_t i = 0; i < namespace_rows.size(); ++i) {
    const auto &ns = namespace_rows[i][0];
    llvm::outs() << "subgraph cluster_" << std::to_string(i) << "{\n";
    llvm::outs() << "label = \"" << ns << "\"\n";
    if (not db.execute("SELECT name FROM classes WHERE namespace = '" + ns +
                       "'"))
      return;
    for (const auto& row: db.rows) {
      const auto& class_ = row[0];
      llvm::outs() << class_ << ";\n";
    }

    llvm::outs() << "}\n";
  }
}

template <ReportType T> void report(const DB &db) {
  reportBegin<T>(db);
  reportClasses<T>(db);
  reportInheritance<T>(db);
  reportEnd<T>(db);
}
} // end anonymous namespace

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal();
  CommonOptionsParser OptionsParser(argc, argv, UmlerCategory);
  RefactoringTool Tool(OptionsParser.getCompilations(),
                       OptionsParser.getSourcePathList());
  ast_matchers::MatchFinder Finder;

  const auto &name = ClassName.getValue();
  const auto &match_name = name.empty() ? anything() : hasName(name);

  const auto db = DB{DBPath.getValue()};
  auto Callback = UmlerCallback{db};

  Finder.addMatcher(
      recordDecl(match_name, isDefinition(), unless(isImplicit())).bind("node"),
      &Callback);

  const auto frontend_result =
      Tool.run(newFrontendActionFactory(&Finder).get());

  const auto format = plantuml;
  switch (format) {
  case sqlite: {
    report<sqlite>(db);
  } break;
  case dot: {
    report<dot>(db);
  } break;
  case plantuml: {
    report<plantuml>(db);
  } break;
  }

  return frontend_result;
}
