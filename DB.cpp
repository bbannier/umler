#include "DB.h"

#include <algorithm>

#include <llvm/Support/raw_ostream.h>
#include <sqlite3.h>

DB::DB(const std::string &Dbpath) {
  if (sqlite3_open(Dbpath.c_str(), &connection) != SQLITE_OK) {
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

DB::~DB() { sqlite3_close(connection); }

bool DB::execute(const std::string &Statement) const {
  if (sqlite3_prepare_v2(connection, Statement.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    llvm::errs() << "COULD NOT PREPARE " << Statement << "\n";
    llvm::errs() << sqlite3_errmsg(connection) << "\n";
    return false;
  }

  rows.clear();
  step();

  return true;
}

bool DB::step() const {
  const auto Rc = sqlite3_step(stmt);
  if (Rc == SQLITE_DONE) {
    return true;
  }

  if (Rc == SQLITE_ROW) {
    std::vector<std::string> Result;
    for (int Column = 0; Column < sqlite3_column_count(stmt); ++Column) {
      Result.emplace_back(
          reinterpret_cast<const char *>(sqlite3_column_text(stmt, Column)));
    }
    rows.emplace_back(std::move(Result));
    return step();
  }

  return false;
}
