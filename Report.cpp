#include "Report.h"

#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

#include "clang/Basic/Specifiers.h"
#include "llvm/Support/raw_ostream.h"

#include "DB.h"

enum ReportType { dot, plantuml };

template <ReportType> void reportBegin(const DB &, const ReportKind &Kind) {}
template <ReportType> void reportEnd(const DB &, const ReportKind &Kind) {}
template <ReportType> void reportClasses(const DB &, const ReportKind &Kind) {}
template <ReportType>
void reportInheritance(const DB &, const ReportKind &Kind) {}

template <> void reportBegin<plantuml>(const DB &, const ReportKind &Kind) {
  llvm::outs() << "@startuml\n\n"
                  "skinparam class {\n"
                  "  BackgroundColor White\n"
                  "  ArrowColor Black\n"
                  "  BorderColor DimGrey\n"
                  "}\n"
                  "hide circle\n"
                  "hide empty attributes\n\n";
}
template <> void reportEnd<plantuml>(const DB &, const ReportKind &Kind) {
  llvm::outs() << "\n@enduml\n";
}
template <> void reportClasses<plantuml>(const DB &Db, const ReportKind &Kind) {
  Db.execute("SELECT DISTINCT namespace FROM classes");
  const auto NamespaceRows = Db.rows;

  for (size_t I = 0; I < NamespaceRows.size(); ++I) {
    const auto &Ns = NamespaceRows[I][0];
    if (not Db.execute("SELECT name FROM classes WHERE namespace = '" + Ns +
                       "'"))
      return;
    const auto ClassRows = Db.rows;
    for (const auto &Row : ClassRows) {
      const auto &Class = Row[0];
      llvm::outs() << "class \"" + Class + "\" {\n";

      if (Kind.DocumentMethods) {
        Db.execute("SELECT name, parameters, returns, access, static, abstract "
                   "FROM methods WHERE class='" +
                   Class + "'");
        for (const auto &Method : Db.rows) {
          std::string Access = "";
          switch (std::stoi(Method[3])) {
          case clang::AS_public: {
            Access = "+";
          } break;
          case clang::AS_private: {
            Access = "-";
          } break;
          case clang::AS_protected: {
            Access = "#";
          } break;
          case clang::AS_none:
            break;
          }

          const std::string IsStatic = std::stoi(Method[4]) ? "{static}" : "";
          const std::string IsAbstract =
              std::stoi(Method[5]) ? "{abstract}" : "";
          const auto Modifiers = IsStatic + IsAbstract;

          const auto Returns = Method[2] == "void" ? "" : Method[2];
          llvm::outs() << "  " + Access + Returns + " " + Method[0] + "(" +
                              Method[1] + ")" + " " + Modifiers + "\n";
        }
      }
      llvm::outs() << "}\n";

      // show "owns" relationships
      if (Kind.DocumentOwns) {
        Db.execute("SELECT object, name FROM owns WHERE owner ='" + Class +
                   "'");
        for (const auto &Row : Db.rows)
          llvm::outs() << "\"" + Class + "\" *-- \"" + Row[0] + "\" : \"" +
                              Row[1] + "\"\n";
      }

      // show "uses" relationships
      if (Kind.DocumentUses) {
        Db.execute("SELECT object FROM uses WHERE user ='" + Class + "'");
        for (const auto &Row : Db.rows) {
          llvm::outs() << "\"" + Class + "\" --> \"" + Row[0] + "\"\n";
        }
      }
    }

    // show "binds" relationships
    if (Kind.DocumentBinds) {
      Db.execute("SELECT DISTINCT template, template_args FROM template_inst");
      const auto TemplateRows = Db.rows;

      for (const auto &Template : TemplateRows) {
        llvm::outs() << "class \"" + Template[0] + "\"<" + Template[1] +
                            "> {\n}\n";

        Db.execute("SELECT instance FROM template_inst WHERE template = '" +
                   Template[0] + "'");
        for (const auto &Row : Db.rows) {
          llvm::outs() << "\"" + Row[0] + "\" ..|> \"" + Template[0] +
                              "\" : <<bind>>\n";
        }
      }
    }
  }
}

template <>
void reportInheritance<plantuml>(const DB &Db, const ReportKind &Kind) {
  if (not Db.execute("SELECT derived, base FROM inheritance"))
    return;
  for (const auto &Row : Db.rows) {
    assert(Row.size() == 2);
    llvm::outs() << "\"" + Row[0] << "\" --|> \"" << Row[1] << "\"\n";
  }
}

template <> void reportBegin<dot>(const DB &, const ReportKind &Kind) {
  llvm::outs() << "digraph G {\n";
}

template <> void reportEnd<dot>(const DB &, const ReportKind &Kind) {
  llvm::outs() << "}\n";
}

template <> void reportInheritance<dot>(const DB &Db, const ReportKind &Kind) {
  if (not Db.execute("SELECT derived, base FROM inheritance"))
    return;

  for (const auto &Row : Db.rows) {
    assert(Row.size() == 2);

    llvm::outs() << Row[0] << " -> " << Row[1] << "\n";
  }
}

template <> void reportClasses<dot>(const DB &Db, const ReportKind &Kind) {
  if (not Db.execute("SELECT DISTINCT namespace FROM classes"))
    return;

  const auto NamespaceRows = Db.rows;

  for (size_t I = 0; I < NamespaceRows.size(); ++I) {
    const auto &Ns = NamespaceRows[I][0];
    llvm::outs() << "subgraph cluster_" << std::to_string(I) << "{\n";
    llvm::outs() << "label = \"" << Ns << "\"\n";
    if (not Db.execute("SELECT name FROM classes WHERE namespace = '" + Ns +
                       "'"))
      return;
    for (const auto &Row : Db.rows) {
      const auto &Class = Row[0];
      llvm::outs() << Class << ";\n";
    }

    llvm::outs() << "}\n";
  }
}

template <ReportType T> void report(const DB &Db, const ReportKind &Kind) {
  reportBegin<T>(Db, Kind);
  reportClasses<T>(Db, Kind);
  reportInheritance<T>(Db, Kind);
  reportEnd<T>(Db, Kind);
}

void report(const DB &Db, const ReportKind &Kind) {
  return report<plantuml>(Db, Kind);
}
