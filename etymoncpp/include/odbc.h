#ifndef ETYMON_ODBC_H
#define ETYMON_ODBC_H

#include <string>
#include <sql.h>
#include <sqlext.h>

using namespace std;

namespace etymon {

const char* odbcStrError(SQLRETURN rc);

class OdbcEnv {
public:
    SQLHENV env;
    OdbcEnv();
    ~OdbcEnv();
};

class OdbcDbc {
public:
    SQLHDBC dbc;
    string dataSourceName;
    OdbcDbc(const OdbcEnv& odbcEnv, const string& dataSourceName);
    void getDbmsName(string* dbmsName);
    void execDirect(const string& sql);
    void commit();
    void rollback();
    ~OdbcDbc();
};

class OdbcStmt {
public:
    SQLHSTMT stmt;
    OdbcStmt(const OdbcDbc& odbcDbc);
    ~OdbcStmt();
};

}

#endif