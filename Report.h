#ifndef REPORT_H
#define REPORT_H

class DB;

struct ReportKind {
  bool documentOwns;
  bool documentUses;
  bool documentBinds;
  bool documentMethods;
};

void report(const DB &db, const ReportKind& kind);

#endif  // REPORT_H
