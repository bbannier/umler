#ifndef DB_H
#define DB_H

#include <string>
#include <vector>

struct sqlite3_stmt;
struct sqlite3;

class DB {
public:
  explicit DB(const std::string &dbpath);

  ~DB();

  bool execute(const std::string &statement) const;

  mutable std::vector<std::vector<std::string>> rows;

  sqlite3 *connection;

private:
  mutable sqlite3_stmt *stmt;

  bool step() const;
};

#endif // DB_H
