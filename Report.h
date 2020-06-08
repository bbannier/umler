#ifndef REPORT_H
#define REPORT_H

class DB;

struct ReportKind {
  bool DocumentOwns;
  bool DocumentUses;
  bool DocumentBinds;
  bool DocumentMethods;
};

void report(const DB &Db, const ReportKind &Kind);

#endif // REPORT_H
