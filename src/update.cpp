#include <cstdint>
#include <curl/curl.h>
#include <experimental/filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>

#include "../etymoncpp/include/curl.h"
#include "extract.h"
#include "init.h"
#include "log.h"
#include "merge.h"
#include "stage_json.h"
#include "timer.h"
#include "update.h"

using namespace etymon;
namespace fs = std::experimental::filesystem;

void makeUpdateTmpDir(const Options& opt, string* loaddir)
{
    fs::path datadir = opt.datadir;
    fs::path tmp = datadir / "tmp";
    //fs::path tmppath = tmp / ("update_" + to_string(time(nullptr)));
    fs::path tmppath = tmp / "update";
    fs::remove_all(tmppath);
    fs::create_directories(tmppath);
    *loaddir = tmppath;

    //*loaddir = opt.datadir;
    //etymon::join(loaddir, "tmp");
    //mkdir(loaddir->c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP |
    //        S_IROTH | S_IXOTH);
    //string filename = "tmp_ldp_" + to_string(time(nullptr));
    //etymon::join(loaddir, filename);
    //mkdir(loaddir->c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP |
    //        S_IROTH | S_IXOTH);
}

bool isForeignKey(etymon::OdbcDbc* dbc, Log* log, const TableSchema& table2,
        const ColumnSchema& column2, const TableSchema& table1)
{
    string sql =
        "SELECT 1\n"
        "    FROM " + table2.tableName + " AS r2\n"
        "        JOIN " + table1.tableName + " AS r1\n"
        "            ON r2." + column2.columnName + "_sk = r1.sk\n"
        "    LIMIT 1;";
    log->logDetail(sql);
    etymon::OdbcStmt stmt(dbc);
    try {
        dbc->execDirect(&stmt, sql);
    } catch (runtime_error& e) {
        return false;
    }
    return dbc->fetch(&stmt);
}

void analyzeReferentialPaths(etymon::OdbcDbc* dbc, Log* log,
        const TableSchema& table2, const ColumnSchema& column2,
        const TableSchema& table1, bool logAnalysis, bool forceConstraints)
{
    string sql =
        "SELECT " + column2.columnName + "_sk AS fkey_sk,\n"
        "       " + column2.columnName + " AS fkey_id\n"
        "    FROM " + table2.tableName + "\n"
        "    WHERE " + column2.columnName + "_sk NOT IN (\n"
        "        SELECT sk FROM " + table1.tableName + "\n"
        "    );";
    log->logDetail(sql);
    // Assume the tables exist.
    //etymon::OdbcDbc deleteDBC(odbc, dbName);
    vector<string> fkeys;
    {
        etymon::OdbcStmt stmt(dbc);
        dbc->execDirect(&stmt, sql);
        while (dbc->fetch(&stmt)) {
            string fkeySK, fkeyID;
            dbc->getData(&stmt, 1, &fkeySK);
            dbc->getData(&stmt, 2, &fkeyID);
            if (forceConstraints)
                fkeys.push_back(fkeySK);
            if (logAnalysis)
                log->log(Level::debug, "", table2.tableName,
                        "Nonexistent key in referential path:\n"
                        "    Referencing table: " + table2.tableName + "\n"
                        "    Referencing column: " + column2.columnName + "\n"
                        "    Referencing column (sk): " + fkeySK + "\n"
                        "    Referencing column (id): " + fkeyID + "\n"
                        "    Referenced table: " + table1.tableName + "\n"
                        "    Action: " +
                        (forceConstraints ? "Deleted (cascading)" : "Ignored"),
                        -1);
        }
    }
    if (forceConstraints) {
        for (auto& fkey : fkeys) {
            sql =
                "DELETE FROM\n"
                "    " + table2.tableName + "\n"
                "    WHERE " + column2.columnName + "_sk = '" + fkey + "';";
            log->logDetail(sql);
            dbc->execDirect(nullptr, sql);
        }
        sql =
            "INSERT INTO ldpsystem.referential_constraints\n"
            "    (referencing_table, referencing_column,\n"
            "        referenced_table, referenced_column)\n"
            "    VALUES\n"
            "    ('" + table2.tableName + "',\n"
            "        '" + column2.columnName + "',\n"
            "        '" + table1.tableName + "',\n"
            "        'sk');";
        log->logDetail(sql);
        dbc->execDirect(nullptr, sql);
        sql =
            "ALTER TABLE\n"
            "    " + table2.tableName + "\n"
            "    ADD CONSTRAINT\n"
            "        " + table2.tableName + "_" + column2.columnName +
            "_sk_fkey\n"
            "        FOREIGN KEY (" + column2.columnName + "_sk)\n"
            "        REFERENCES\n"
            "        " + table1.tableName + "\n"
            "        (sk);";
        log->logDetail(sql);
        dbc->execDirect(nullptr, sql);
    }
}

class Reference {
public:
    string referencingTable;
    string referencingColumn;
    string referencedTable;
    string referencedColumn;
};

void processReferentialPaths(etymon::OdbcEnv* odbc, const string& dbName,
        etymon::OdbcDbc* dbc, Log* log, const Schema& schema,
        const TableSchema& table, bool detectForeignKeys,
        map<string, vector<Reference>>* refs)
{
    etymon::OdbcDbc queryDBC(odbc, dbName);
    log->logDetail("Searching for foreign keys in table: " + table.tableName);
    //printf("Table: %s\n", table.tableName.c_str());
    for (auto& column : table.columns) {
        if (column.columnType != ColumnType::id)
            continue;
        if (column.columnName == "id")
            continue;
        //printf("    Column: %s\n", column.columnName.c_str());
        for (auto& table1 : schema.tables) {
            if (isForeignKey(&queryDBC, log, table, column, table1)) {

                string key = table.tableName + "." + column.columnName + "_sk";
                Reference ref = {
                    table.tableName,
                    column.columnName + "_sk",
                    table1.tableName,
                    "sk"
                };

                //fprintf(stderr, "%s(%s) -> %s(%s)\n",
                //        ref.referencingTable.c_str(),
                //        ref.referencingColumn.c_str(),
                //        ref.referencedTable.c_str(),
                //        ref.referencedColumn.c_str());

                (*refs)[key].push_back(ref);

                //printf("        -> %s\n", table1.tableName.c_str());
                //analyzeReferentialPaths([>odbc, dbName,<] dbc, log, table,
                //        column, table1, logAnalysis, forceConstraints);
            }
        }
    }
}

void selectConfigGeneral(etymon::OdbcDbc* dbc, Log* log,
        bool* detectForeignKeys, bool* forceForeignKeyConstraints,
        bool* enableForeignKeyWarnings)
{
    string sql =
        "SELECT detect_foreign_keys,\n"
        "       force_foreign_key_constraints,\n"
        "       enable_foreign_key_warnings\n"
        "    FROM ldpconfig.general;";
    log->logDetail(sql);
    etymon::OdbcStmt stmt(dbc);
    dbc->execDirect(&stmt, sql);
    dbc->fetch(&stmt);
    string s1, s2, s3;
    dbc->getData(&stmt, 1, &s1);
    dbc->getData(&stmt, 2, &s2);
    dbc->getData(&stmt, 3, &s3);
    dbc->fetch(&stmt);
    *detectForeignKeys = (s1 == "1");
    *forceForeignKeyConstraints = (s2 == "1");
    *enableForeignKeyWarnings = (s3 == "1");
}

void runUpdate(const Options& opt)
{
    CURLcode cc;
    curl_global curl_env(CURL_GLOBAL_ALL, &cc);
    if (cc) {
        throw runtime_error(string("Error initializing curl: ") +
                curl_easy_strerror(cc));
    }

    etymon::OdbcEnv odbc;

    etymon::OdbcDbc logDbc(&odbc, opt.db);
    Log log(&logDbc, opt.logLevel, opt.console, opt.prog);

    log.log(Level::debug, "server", "", "Starting full update", -1);
    Timer fullUpdateTimer(opt);

    Schema schema;
    Schema::MakeDefaultSchema(&schema);

    init_upgrade(&odbc, opt.db, opt.ldpUser, opt.ldpconfigUser, opt.datadir,
            &log);

    ExtractionFiles extractionDir(opt);

    string loadDir;

    Curl c;
    //if (!c.curl) {
    //    // throw?
    //}
    string token, tenantHeader, tokenHeader;

    if (opt.loadFromDir != "") {
        //if (opt.logLevel == Level::trace)
        //    fprintf(opt.err, "%s: Reading data from directory: %s\n",
        //            opt.prog, opt.loadFromDir.c_str());
        loadDir = opt.loadFromDir;
    } else {
        log.log(Level::trace, "", "", "Logging in to Okapi service", -1);

        okapiLogin(opt, &log, &token);

        makeUpdateTmpDir(opt, &loadDir);
        extractionDir.dir = loadDir;

        tenantHeader = "X-Okapi-Tenant: ";
        tenantHeader + opt.okapiTenant;
        tokenHeader = "X-Okapi-Token: ";
        tokenHeader += token;
        c.headers = curl_slist_append(c.headers, tenantHeader.c_str());
        c.headers = curl_slist_append(c.headers, tokenHeader.c_str());
        c.headers = curl_slist_append(c.headers,
                "Accept: application/json,text/plain");
        curl_easy_setopt(c.curl, CURLOPT_HTTPHEADER, c.headers);
    }

    Timer idmapTimer1(opt);
    IDMap idmap(&odbc, opt.db, &log, loadDir, opt.datadir);
    log.log(Level::debug, "update", "", "Synchronized cache",
            idmapTimer1.elapsedTime());

    string ldpconfigDisableAnonymization;
    {
        etymon::OdbcDbc dbc(&odbc, opt.db);
        string sql = "SELECT disable_anonymization FROM ldpconfig.general;";
        log.logDetail(sql);
        {
            etymon::OdbcStmt stmt(&dbc);
            dbc.execDirect(&stmt, sql);
            dbc.fetch(&stmt);
            dbc.getData(&stmt, 1, &ldpconfigDisableAnonymization);
        }
    }

    for (auto& table : schema.tables) {

        if (opt.table != "" && opt.table != table.tableName)
            continue;

        bool anonymizeTable = ( table.anonymize &&
                (!opt.disableAnonymization ||
                 ldpconfigDisableAnonymization != "1") );

        //printf("anonymize=%d\tfile_disable=%d\tdb_disable=%s\tA=%d\n",
        //        table.anonymize, opt.disableAnonymization,
        //        ldpconfigDisableAnonymization.c_str(), anonymizeTable);

        if (anonymizeTable)
            continue;

        log.log(Level::trace, "", "",
                "Updating table: " + table.tableName, -1);

        Timer updateTimer(opt);

        ExtractionFiles extractionFiles(opt);

        if (opt.loadFromDir == "") {
            log.log(Level::trace, "", "",
                    "Extracting: " + table.sourcePath, -1);
            bool foundData = directOverride(opt, table.tableName) ?
                retrieveDirect(opt, &log, table, loadDir, &extractionFiles) :
                retrievePages(c, opt, &log, token, table, loadDir,
                        &extractionFiles);
            if (!foundData)
                table.skip = true;
        }

        if (table.skip || opt.extractOnly)
            continue;

        etymon::OdbcDbc dbc(&odbc, opt.db);
        //PQsetNoticeProcessor(db.conn, debugNoticeProcessor, (void*) &opt);
        DBType dbt(&dbc);

        {
            etymon::OdbcTx tx(&dbc);

            log.log(Level::trace, "", "",
                    "Staging table: " + table.tableName, -1);
            stageTable(opt, &log, &table, &odbc, &dbc, &dbt, loadDir, &idmap);

            log.log(Level::trace, "", "",
                    "Merging table: " + table.tableName, -1);
            mergeTable(opt, &log, table, &odbc, &dbc, dbt);

            log.log(Level::trace, "", "",
                    "Replacing table: " + table.tableName, -1);

            dropTable(opt, &log, table.tableName, &dbc);

            placeTable(opt, &log, table, &dbc);
            //updateStatus(opt, table, &dbc);

            //updateDBPermissions(opt, &log, &dbc);

            tx.commit();
        }

        //vacuumAnalyzeTable(opt, table, &dbc);

        string sql = 
            "SELECT COUNT(*) FROM\n"
            "    " + table.tableName + ";";
        log.logDetail(sql);
        string rowCount;
        {
            etymon::OdbcStmt stmt(&dbc);
            dbc.execDirect(&stmt, sql);
            dbc.fetch(&stmt);
            dbc.getData(&stmt, 1, &rowCount);
        }
        sql = 
            "SELECT COUNT(*) FROM\n"
            "    history." + table.tableName + ";";
        log.logDetail(sql);
        string historyRowCount;
        {
            etymon::OdbcStmt stmt(&dbc);
            dbc.execDirect(&stmt, sql);
            dbc.fetch(&stmt);
            dbc.getData(&stmt, 1, &historyRowCount);
        }
        sql =
            "UPDATE ldpsystem.tables\n"
            "    SET updated = " + string(dbt.currentTimestamp()) + ",\n"
            "        row_count = " + rowCount + ",\n"
            "        history_row_count = " + historyRowCount + ",\n"
            "        documentation = '" + table.sourcePath + " in "
            + table.moduleName + "',\n"
            "        documentation_url = 'https://dev.folio.org/reference/api/#"
            + table.moduleName + "'\n"
            "    WHERE table_name = '" + table.tableName + "';";
        log.logDetail(sql);
        dbc.execDirect(nullptr, sql);

        log.log(Level::debug, "update", table.tableName,
                "Updated table: " + table.tableName,
                updateTimer.elapsedTime());

        //if (opt.logLevel == Level::trace)
        //    loadTimer.print("load time");
    } // for

    Timer idmapTimer2(opt);
    idmap.syncCommit();
    log.log(Level::debug, "update", "", "Synchronized cache",
            idmapTimer2.elapsedTime());

    //{
    //    etymon::OdbcDbc dbc(&odbc, opt.db);
    //    {
    //        etymon::OdbcTx tx(&dbc);
    //        dropOldTables(opt, &log, &dbc);
    //        tx.commit();
    //    }
    //}

    log.log(Level::debug, "server", "", "Completed full update",
            fullUpdateTimer.elapsedTime());

    // TODO Move analysis and constraints out of update process.
    {
        etymon::OdbcDbc dbc(&odbc, opt.db);

        bool detectForeignKeys = false;
        bool forceForeignKeyConstraints = false;
        bool enableForeignKeyWarnings = false;
        selectConfigGeneral(&dbc, &log, &detectForeignKeys,
                &forceForeignKeyConstraints, &enableForeignKeyWarnings);

        if (detectForeignKeys) {

            log.log(Level::debug, "server", "",
                    "Starting referential analysis", -1);

            Timer refTimer(opt);

            etymon::OdbcTx tx(&dbc);

            map<string, vector<Reference>> refs;
            for (auto& table : schema.tables)
                processReferentialPaths(&odbc, opt.db, &dbc, &log, schema,
                        table, detectForeignKeys, &refs);

            string sql = "DELETE FROM ldpconfig.foreign_keys;";
            log.detail(sql);
            dbc.exec(sql);

            for (pair<string, vector<Reference>> p : refs) {
                bool enable = (p.second.size() == 1);
                for (auto& r : p.second) {
                    sql =
                        "INSERT INTO ldpconfig.foreign_keys\n"
                        "    (enable_constraint,\n"
                        "        referencing_table, referencing_column,\n"
                        "        referenced_table, referenced_column)\n"
                        "VALUES\n"
                        "    (" + string(enable ? "TRUE" : "FALSE") + ",\n"
                        "        '" + r.referencingTable + "',\n"
                        "        '" + r.referencingColumn + "',\n"
                        "        '" + r.referencedTable + "',\n"
                        "        '" + r.referencedColumn + "');";
                    log.detail(sql);
                    dbc.exec(sql);
                }
            }

            tx.commit();

            log.log(Level::debug, "server", "",
                    "Completed referential analysis",
                    refTimer.elapsedTime());
        }

        //if (forceForeignKeyConstraints) {
        //}

    }

    Timer idmapTimer3(opt);
    idmap.vacuum();
    log.log(Level::debug, "update", "", "Optimized cache",
            idmapTimer3.elapsedTime());
}

void runUpdateProcess(const Options& opt)
{
#ifdef GPROF
    string updateDir = "./update-gprof";
    fs::create_directories(updateDir);
    chdir(updateDir.c_str());
#endif
    try {
        runUpdate(opt);
        exit(0);
    } catch (runtime_error& e) {
        string s = e.what();
        if ( !(s.empty()) && s.back() == '\n' )
            s.pop_back();
        etymon::OdbcEnv odbc;
        etymon::OdbcDbc logDbc(&odbc, opt.db);
        Log log(&logDbc, opt.logLevel, opt.console, opt.prog);
        log.log(Level::error, "server", "", s, -1);
        exit(1);
    }
}

