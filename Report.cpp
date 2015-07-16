#include "Report.h"

#include <llvm/Support/raw_ostream.h>
#include <clang/Basic/Specifiers.h>

#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

#include "DB.h"

enum ReportType { dot, plantuml };

template <ReportType> void reportBegin(const DB &, const ReportKind& kind) {}
template <ReportType> void reportEnd(const DB &, const ReportKind& kind) {}
template <ReportType> void reportClasses(const DB &, const ReportKind& kind) {}
template <ReportType> void reportInheritance(const DB &, const ReportKind& kind) {}

template <> void reportBegin<plantuml>(const DB &, const ReportKind& kind) {
  llvm::outs() << "@startuml\n\n"
                  "skinparam class {\n"
                  "  BackgroundColor White\n"
                  "  ArrowColor Black\n"
                  "  BorderColor DimGrey\n"
                  "}\n"
                  "hide circle\n"
                  "hide empty attributes\n\n";
}
template <> void reportEnd<plantuml>(const DB &, const ReportKind& kind) {
  llvm::outs() << "\n@enduml\n";
}
template <> void reportClasses<plantuml>(const DB &db, const ReportKind& kind) {
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
        case clang::AS_public: {
          access = "+";
        } break;
        case clang::AS_private: {
          access = "-";
        } break;
        case clang::AS_protected: {
          access = "#";
        } break;
        case clang::AS_none:
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
      if (kind.documentOwns) {
        db.execute("SELECT object, name FROM owns WHERE owner ='" + class_ +
                   "'");
        for (const auto &row : db.rows)
          llvm::outs() << "\"" + class_ + "\" *-- \"" + row[0] + "\" : \"" +
                              row[1] + "\"\n";
      }

      // show "uses" relationships
      if (kind.documentUses) {
        db.execute("SELECT object FROM uses WHERE user ='" + class_ + "'");
        for (const auto &row : db.rows) {
          llvm::outs() << "\"" + class_ + "\" --> \"" + row[0] + "\"\n";
        }
      }
    }

    // show "binds" relationships
    if (kind.documentBinds) {
      db.execute("SELECT DISTINCT template, template_args FROM template_inst");
      const auto template_rows = db.rows;

      for (const auto& template_ : template_rows) {
        llvm::outs() << "class \"" + template_[0] + "\"<" + template_[1] +
                            "> {\n}\n";

        db.execute("SELECT instance FROM template_inst WHERE template = '" +
                   template_[0] + "'");
        for (const auto& row: db.rows) {
          llvm::outs() << "\"" + row[0] + "\" ..|> \"" + template_[0] +
                              "\" : <<bind>>\n";
        }
      }
    }
  }
}

template <> void reportInheritance<plantuml>(const DB &db, const ReportKind& kind) {
  if (not db.execute("SELECT derived, base FROM inheritance"))
    return;
  for (const auto& row: db.rows) {
    assert(row.size() == 2);
    llvm::outs() << "\"" + row[0] << "\" --|> \"" << row[1] << "\"\n";
  }
}

template <> void reportBegin<dot>(const DB&, const ReportKind& kind) {
  llvm::outs() << "digraph G {\n";
}

template <> void reportEnd<dot>(const DB&, const ReportKind& kind) {
  llvm::outs() << "}\n";
}

template <> void reportInheritance<dot>(const DB &db, const ReportKind& kind) {
  if (not db.execute("SELECT derived, base FROM inheritance"))
    return;

  for (const auto& row : db.rows ) {
    assert(row.size() == 2);

    llvm::outs() << row[0] << " -> " << row[1] << "\n";
  }
}

template <> void reportClasses<dot>(const DB &db, const ReportKind& kind) {
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

template <ReportType T> void report(const DB &db, const ReportKind &kind) {
  reportBegin<T>(db, kind);
  reportClasses<T>(db, kind);
  reportInheritance<T>(db, kind);
  reportEnd<T>(db, kind);
}

void report(const DB &db, const ReportKind &kind) {
  return report<plantuml>(db, kind);
}
