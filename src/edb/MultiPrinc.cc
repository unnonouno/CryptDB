/*
 * MultiPrincipal.cpp
 *
 */

#include "MultiPrinc.h"
#include "cryptdb_log.h"

MultiPrinc::MultiPrinc(Connect * connarg)
{
    conn = connarg;
    accMan = new KeyAccess(conn);
}

MultiPrinc::~MultiPrinc()
{
    mkm.cleanup();
    accMan->finish();
}

string
fullName(string field, string name)
{
    if (isTableField(field)) {
        return field;
    } else {
        return name + "." + field;
    }
}

bool
isTableField(string token)
{
    size_t pos = token.find(".");

    if (pos == string::npos) {
        return false;
    } else {
        return true;
    }
}

const bool VERBOSE = true;

void
MultiPrinc::processAnnotation(list<string>::iterator & wordsIt,
                              list<string> & words, string tablename,
                              string currentField,
                              bool & encryptfield, map<string,
                                                       TableMetadata *> & tm)
{

    LOG(mp) << "processing annotation";
    if (equalsIgnoreCase(*wordsIt, "encfor")) {

        if (VERBOSE_G) { LOG(mp) << "encfor"; }
        wordsIt++;
        string field2 = *wordsIt;
        wordsIt++;
        mkm.encForMap[fullName(currentField, tablename)] = fullName(field2,
                                                                    tablename);
        if (VERBOSE_G) { LOG(mp) << "==> " <<
                         fullName(currentField, tablename) << " " << fullName(
                             field2,
                             tablename); }
        mkm.reverseEncFor[fullName(field2, tablename)] = true;
        encryptfield = true;

        //check if there is any annotation for security level
        string secAnns[] =
            { levelnames[(int) SECLEVEL::DET],
              levelnames[(int) SECLEVEL::DETJOIN],
              levelnames[(int) SECLEVEL::OPE],
              levelnames[(int) SECLEVEL::SEMANTIC_AGG]
            };
        unsigned int noSecAnns = 4;
        while ((wordsIt != words.end()) &&
               contains(*wordsIt, secAnns, noSecAnns)) {
            FieldMetadata * fm = tm[tablename]->fieldMetaMap[currentField];

            if (equalsIgnoreCase(levelnames[(int) SECLEVEL::DET], *wordsIt)) {
                if (VERBOSE_G) { LOG(mp) << "at det"; }
                fm->secLevelDET = SECLEVEL::DET;
                wordsIt++;
                continue;
            }

            if (equalsIgnoreCase(levelnames[(int) SECLEVEL::DETJOIN], *wordsIt)) {
                fm->secLevelDET = SECLEVEL::DETJOIN;
                wordsIt++;
                continue;
            }

            if (equalsIgnoreCase(levelnames[(int) SECLEVEL::OPE], *wordsIt)) {
                if (VERBOSE_G) { LOG(mp) << "at det and opeself"; }
                fm->secLevelOPE = SECLEVEL::OPE;
                fm->secLevelDET = SECLEVEL::DET;
                fm->ope_used = true;
                wordsIt++;
                continue;
            }

            if (equalsIgnoreCase(levelnames[(int) SECLEVEL::SEMANTIC_AGG], *wordsIt)) {
                fm->agg_used = true;
                wordsIt++;
                continue;
            }
            assert_s(false, "invalid control path");
        }

        return;
    }

    if (equalsIgnoreCase(*wordsIt, "givespsswd")) {
        wordsIt++;
        string field2 = *wordsIt;
        wordsIt++;
        int resacc = accMan->addGives(fullName(currentField, tablename));
        assert_s(resacc >=0, "access manager gives psswd failed");
        resacc = accMan->addAccess( fullName(currentField,
                                             tablename),
                                    fullName(field2, tablename));
        assert_s(resacc >=0, "access manager addaccessto failed");
        encryptfield = false;
        return;
    }

    int countaccessto = 0;
    while (true) {
        if (equalsIgnoreCase(*wordsIt,"hasaccessto")) {
            wordsIt++;
            if (countaccessto > 0) {
                assert_s(
                    false,
                    "multiple hasaccessto annotations on same field, need to add this in insert relations");
            }
            countaccessto++;
            string field2 = *wordsIt;
            wordsIt++;
            string hasAccess = fullName(currentField, tablename);
            string accessto = fullName(field2, tablename);
            int resacc = accMan->addAccess(hasAccess, accessto);
            assert_s(resacc >=0, "access manager addAccessto failed");
            encryptfield = false;
            if (equalsIgnoreCase(*wordsIt, "if")) {             //predicate
                wordsIt++;                 // go over "if"
                Predicate * pred = new Predicate();
                pred->name = *wordsIt;
                roll<string>(wordsIt, 2);                 //go over name and (
                while (wordsIt->compare(")")) {
                    pred->fields.push_back(*wordsIt);
                    wordsIt++;
                    checkStr(wordsIt, words, ",", ")");
                }
                wordsIt++;
                mkm.condAccess[hasAccess] = pred;
            }
            continue;
        }

        if (equalsIgnoreCase(*wordsIt, "equals")) {
            wordsIt++;
            string field2 = *wordsIt;
            wordsIt++;
            int resacc = accMan->addEquals(fullName(currentField,
                                                    tablename),
                                           fullName(field2, tablename));
            assert_s(resacc >=0, "access manager addEquals failed");
            encryptfield = false;
            continue;
        }
        return;
    }

    encryptfield = false;

}

int
MultiPrinc::commitAnnotations()
{
    return accMan->CreateTables();
}

static bool
validate(string a, string op, string b)
{
    if (op.compare("=")) {
        return false;
    }

    if (!isField(a)) {
        return false;
    }

    if (isField(b)) {
        return false;
    }

    return true;
}

typedef struct equalOp {
    string a, op, b;
} equalOp;

//  Returns true if there is at least one value at "it"
// Effects: moves "it" after the  value or at the end if there are no such
// values
static bool
getItem(list<string>::iterator & it, const list<string>::iterator & itend,
        string & a)
{
    if (it == itend) {
        return false;
    }

    a = *it;
    it++;
    return true;
}

// Returns true if there are at least three values at "it"
// Effects: moves it after the three values or at the end if there are no such
// values
static bool
getTriple(list<string>::iterator & it, const list<string>::iterator & itend,
          string & a, string & b,
          string & c)
{
    return getItem(it, itend,
                   a) && getItem(it, itend, b) && getItem(it, itend,
                                                          c);
}

// Requires: it points after "WHERE" clause
//           query is flat, does not have nested AND and OR and it is not a
// nested query
// returns a list of all equality operations of the form "field = value"
static list<equalOp> *
getEqualityExpr(list<string>::iterator & it, list<string> & query,
                QueryMeta & qm, map<string, TableMetadata *> & tableMetaMap)
{

    //LOG(mp) << "eq expr \n";
    string lst[] = {"and", "or"};
    unsigned int lstno = 2;

    list<equalOp> * res = new list<equalOp>();
    string a, op, b;

    while ((it!=query.end()) && !isQuerySeparator(*it)) {
        LOG(mp) << "before get triple \n";
        //get the first pair of expressions and point after it
        if (!getTriple(it, query.end(), a, op, b)) {
            return res;
        }
        LOG(mp) << "working with " << a << " " << op << " " << b << "\n";
        //we have a triple, let's check it is a good one and that it is not
        // part of an expression (i.e. a keyword or the end comes after)
        if (validate(a, op,
                     b) &&
            ((it == query.end()) || (isQuerySeparator(*it)) ||
             contains(*it, lst, lstno))) {
            //it's a good one, add it to the list
            string table, field;
            getTableField(a, table, field, qm, tableMetaMap);
            a = fullName(field, table);
            equalOp eo;
            eo.a = a;
            eo.op = op;
            eo.b = b;
            res->push_back(eo);
            LOG(mp) << "adding " << a << " " << op << " " << b << "\n";

        } else {
            // we might be inside a longer expression, e.g., a = 5+b, proceed
            // outside of it
            while (it != query.end() && !isQuerySeparator(*it) &&
                   !contains(*it, lst, lstno)) {
                it++;
            }

        }

        // skip any and/or
        if ((it !=  query.end()) && contains(*it, lst, lstno)) {
            it++;
        }

    }

    return res;

}

void
MultiPrinc::getEncForFromFilter(command comm, list<string> query, TMKM & tmkm,
                                QueryMeta & qm, map<string,
                                                    TableMetadata *> &
                                tableMetaMap)
{
    if (!PARSING) {
        tmkm.encForVal = map<string, string>();

        assert_s((comm == cmd::SELECT) || (comm == cmd::UPDATE) || (comm == cmd::DELETE),
                 "query does not have filter");

        string table = qm.tables.front();
        list<string>::iterator kwit = itAtKeyword(query, "where");
        if (kwit == query.end()) {
            return;
        }
        kwit++;

        list<equalOp> * eos = getEqualityExpr(kwit, query, qm, tableMetaMap);
        string a, op, b;

        for (auto it = eos->begin(); it != eos->end(); it++) {

            a = it->a;
            op = it->op;
            b = it->b;

            if (VERBOSE) { LOG(mp) << "EXPR " << a << op << b << "\n"; }
            if (mkm.reverseEncFor.find(a) != mkm.reverseEncFor.end()) {
                if (VERBOSE) { LOG(mp) << "RECORDING " << a << op << b << "\n"; }
                tmkm.encForVal[a] =  b;
            } else {
                if (VERBOSE) {
                    LOG(mp) << "do not record \n";
                    LOG(mp) << "here is what that map contains: ";
                    for (auto xit = mkm.reverseEncFor.begin();
                         xit != mkm.reverseEncFor.end(); xit++) {
                        LOG(mp) << xit->first << " ";
                    }
                }
            }
        }

        if (VERBOSE) {
            LOG(mp) << "here is what that encforval contains: ";
            for (auto it = tmkm.encForVal.begin();
                 it != tmkm.encForVal.end(); it++) {
                LOG(mp) << it->first << " " << it->second << "\n";
            }
        }

        if (VERBOSE) { LOG(mp) << "done with all expressions \n"; }

    }
}

// extracts enc-for principals from queries
void
MultiPrinc::prepareSelect(list<string> & words, TMKM & tmkm, QueryMeta & qm,
                          map<string,
                              TableMetadata *> & tm)
{
    // records for which principals some values are encrypted by looking in
    // the where clause as well
    getEncForFromFilter(cmd::SELECT, words, tmkm, qm, tm);

    //add all fields requested in principalsSeen

    list<string>::iterator it = words.begin();
    getFieldsItSelect(words, it);

    while ((it != words.end()) && (!isQuerySeparator(*it))) {
        // gettimeofday(&starttime, NULL);
        if (isAgg(*it)) {
            string field, table;
            onion o;
            processAgg(it, words, field, table, o, qm, tm, 1);
            continue;
        }

        string field, table;
        getTableField(*it, table, field, qm, tm);

        string fn = fullName(field, table);

        it++;

        processAlias(it, words);

    }

}

string
MultiPrinc::selectEncFor(string table, string field, QueryMeta & qm,
                         TMKM & tmkm, TableMetadata * tm,
                         FieldMetadata * fm)
{
    string princ = mkm.encForMap[fullName(field, table)];
    if (tmkm.principalsSeen.find(princ) == tmkm.principalsSeen.end()) {
        //need to add principal for which this field is encrypted
        tmkm.principalsSeen[princ] = true;
        return ", " +
               fieldNameForQuery(tm->anonTableName, table,
                                 getField(mkm.encForMap[fullName(field,
                                                                 table)]),
                                 fm->type, qm) + " ";
    }

    return "";
}

// fills tmkm.encForReturned and decides if the next field was added by us and
// should not be returned to the user
void
MultiPrinc::processReturnedField(unsigned int index, string fullname, onion o,
                                 TMKM & tmkm,
                                 bool & ignore)
{

    ignore = false;

    if (o != oNONE) {
        //figure out where is the principal we want
        string princ = mkm.encForMap[fullname];
        if (tmkm.principalsSeen.find(princ) == tmkm.principalsSeen.end()) {
            //it must be value after because we inserted it
            tmkm.encForReturned[princ] = index+1;
            tmkm.principalsSeen[princ] = true;
            ignore = true;

        }
    }

    tmkm.returnBitMap[index] = true;
    tmkm.returnBitMap[index+1] = !ignore;
}

//returns the name of the table if given an expression of the form
// $(PSSWD_TABLE_PREFIX)__TABLENAME,
// else returns ""
static string
getPsswdTable(string table)
{
    size_t prefix_len = PWD_TABLE_PREFIX.length();

    if (table.substr(0, prefix_len) == PWD_TABLE_PREFIX) {
        return table.substr(prefix_len, table.length()-prefix_len);
    }

    return "";
}

bool
MultiPrinc::checkPsswd(command comm, list<string> & words)
{

    /*
     * checks for
     * INSERT INTO cryptdbpsswd__TABLENAME (fieldname, psswd) VALUES (...)"
     */
    list<string>::iterator wordsIt = words.begin();
    string table;

    if (comm == cmd::INSERT) {
        roll<string>(wordsIt, 2);
        table = *wordsIt;
        string pw_table;
        if ((pw_table = getPsswdTable(table)).length() > 0) {
            wordsIt++;
            assert_s(wordsIt->compare(
                         "(") == 0,
                     "expected ( fields names list ) before VALUES ");
            roll<string>(wordsIt, 1);
            string type = fullName(*wordsIt, pw_table);
            roll<string>(wordsIt, 6);
            string uname = removeApostrophe(*wordsIt);
            wordsIt++;
            string p = removeApostrophe(*wordsIt);

            /*
             * XXX
             * we should hash this password!
             */
            string passwd = p;
            passwd.resize(AES_KEY_BYTES);

            int resacc = accMan->insertPsswd(Prin(type, uname), passwd);
            assert_s(resacc >=0, "access manager insert psswd failed");
            return true;
        }

        return false;
    }

    if (comm == cmd::DELETE) {
        roll<string>(wordsIt, 2);         //now points to table name
        table = *wordsIt;
        string pw_table;
        if (VERBOSE_G) { LOG(mp) << "table in DELETE " << table <<"\n"; }
        if ((pw_table = getPsswdTable(table)).length() > 0) {
            roll<string>(wordsIt, 2);             // now points to givespsswd
                                                  // fieldname
            string type = fullName(*wordsIt, pw_table);
            roll<string>(wordsIt,2);             //now points to value
            string uname = *wordsIt;
            accMan->removePsswd(Prin(type, removeApostrophe(uname)));
            return true;
        }

        return false;
    }

    assert_s(false, "checkpasswd should be called only for insert and delete");
    return false;
}

bool
MultiPrinc::checkPredicate(string hasaccess, map<string, string> & vals)
{
    if (mkm.condAccess.find(hasaccess) != mkm.condAccess.end()) {

        Predicate * pred = mkm.condAccess[hasaccess];

        //need to check correctness of this predicate
        string query = "SELECT " +  pred->name + "( ";
        for (list<string>::iterator it = pred->fields.begin();
             it != pred->fields.end(); it++) {
            query += " " + vals[*it] + ",";
        }
        query[query.length()-1]=' ';
        query += ");";
        DBResult * dbres;
        if (VERBOSE_G) { LOG(mp) << "check pred: " << query << "\n"; }
        assert_s(conn->execute(
                     query.c_str(),
                     dbres), "failure while executing query " + query);
        ResType * result = dbres->unpack();
        delete dbres;
        if (result->at(1).at(0).compare("1") == 0) {
            if (VERBOSE_G) { LOG(mp) << "pred OK\n"; }
            delete result;
            return true;
        } else {
            if (VERBOSE_G) { LOG(mp) << "pred NO\n"; }
            delete result;
            return false;
        }
    }

    //no predicate
    return true;
}

//wordsIt points to the first value
void
MultiPrinc::insertRelations(const list<string> & values, string table,
                            list<string> fields,
                            TMKM & tmkm)
{

    //first collect all values in a list
    map<string, string> vals;
    list<string>::iterator fieldIt = fields.begin();
    list<string>::const_iterator valIt = values.begin();

    if (VERBOSE_G) { LOG(mp) << "fields are "; myPrint(fields); }
    if (VERBOSE_G) { LOG(mp) << "values are "; myPrint(values); }

    while (fieldIt != fields.end()) {
        vals[*fieldIt] = removeApostrophe(*valIt);
        fieldIt++; valIt++;
    }

    assert_s(
        valIt == values.end(),
        "values and fields should have the same length");

    // We have a mapping, vals, of field name to values in this insert
    // we need to figure out which has access to values to insert
    // TODO: this is restricted to values in the same table only

    for (fieldIt = fields.begin(); fieldIt != fields.end(); fieldIt++) {
        string fullField = fullName(*fieldIt, table);
        if (mkm.encForMap.find(fullField) != mkm.encForMap.end()) {
            string encForField = mkm.encForMap[fullField];
            tmkm.encForVal[encForField] = vals[getField(encForField)];
        }
        string hasaccess = fullField;
        if (VERBOSE_G) { LOG(mp) << "hasaccess " << hasaccess; }

        if (accMan->isType(hasaccess)) {
            LOG(mp) << hasaccess << " is type \n";
            std::set<string> accessto_lst = accMan->getTypesHasAccessTo(
                hasaccess);
            if (VERBOSE_G) { LOG(mp) << hasaccess << " has access to  " <<
                             " <" << toString(accessto_lst) << ">\n"; }

            for (std::set<string>::iterator accIt = accessto_lst.begin();
                 accIt != accessto_lst.end(); accIt++) {
                string accessto = *accIt;
                if (vals.find(accessto) == vals.end()) {
                    //this access to is not in this table and hence in this
                    // insert
                    continue;
                }
                LOG(mp) << "before predicate \n";
                //TODO: checkPredicate should also take accessto
                if (checkPredicate(hasaccess, vals)) {
                    //need to insert
                    LOG(mp) << "inserting \n";
                    int resacc =
                        accMan->insert(Prin(hasaccess,
                                            vals[*fieldIt]),
                                       Prin(*accIt, vals[getField(accessto)]));
                    assert_s(resacc >=0, "access manager insert failed");
                    if (VERBOSE_G) { LOG(mp) << "after insert\n"; }
                }
            }
        }

    }

    vals.clear();
}

bool
MultiPrinc::isActiveUsers(const string &query)
{
    list<string> words = getSQLWords(query);
    list<string>::iterator wordsIt = words.begin();
    string firstw = *wordsIt;
    string thirdw;
    if (words.size() > 2) {
        roll<string>(wordsIt, 2);
        thirdw = *wordsIt;
    } else {
        thirdw = "";
    }
    if ((equalsIgnoreCase(firstw,
                          "INSERT") ||
         equalsIgnoreCase(firstw,
                          "DELETE")) &&
        equalsIgnoreCase(psswdtable, thirdw)) {
        return true;
    }
    return false;

}

bool
MultiPrinc::isPrincipal(string fullname)
{
    if ((accMan->getTypesAccessibleFrom(fullname).size() > 0)) {

        return true;
    }
    return false;
}

string
MultiPrinc::get_key(string fieldName, TempMKM & tmkm)
{

    assert_s(mkm.encForMap.find(
                 fieldName) != mkm.encForMap.end(),
             "cryptappgetkey gets unencrypted feild <"+fieldName+">");
    string encForField = mkm.encForMap[fieldName];

    if (tmkm.encForVal.find(encForField) != tmkm.encForVal.end()) {
        if (VERBOSE_G) {LOG(mp) << "asking get key for " << encForField <<
                        " <" << tmkm.encForVal[encForField] << "> \n"; }
        string key =
            accMan->getKey(Prin(encForField,
                                removeApostrophe(tmkm.encForVal[encForField])));
        LOG(mp) << "-- key from accman is " <<
        CryptoManager::marshallKey(key) << "\n";
        assert_s(
            key.length() > 0, "access manager does not have needed key!!");
        return key;
    }

    assert_s(
        false,
        "cryptdb does not have keys to encrypt query for requested users \n");
    return NULL;
}

string
MultiPrinc::get_key(string fieldName, TMKM & tmkm,
                    const vector<string>  & res)
{
    assert_s(mkm.encForMap.find(
                 fieldName) != mkm.encForMap.end(),
             "cryptappgetkey gets unencrypted field <"+fieldName+">");
    string encForField = mkm.encForMap[fieldName];

    if (tmkm.encForVal.find(encForField) != tmkm.encForVal.end()) {
        string key =
            accMan->getKey(Prin(encForField,
                                removeApostrophe(tmkm.encForVal[encForField])));
        if (VERBOSE_G) {LOG(mp) << "using encforval; encForField " <<
                        encForField << " val " <<
                        tmkm.encForVal[encForField] <<
                        " encforreturned index " <<
                        tmkm.encForReturned[encForField] <<
                        "\n"; }
        LOG(mp) << "-- key from accman is " <<
        CryptoManager::marshallKey(key) << "\n";
        assert_s(key.length() > 0, "access manager does not have key\n");
        return key;
    }

    if (tmkm.encForReturned.find(encForField) != tmkm.encForReturned.end()) {
        string val = res[tmkm.encForReturned[encForField]];
        string key = accMan->getKey(Prin(encForField, removeApostrophe(val)));
        LOG(mp) << "-- key from accman is " <<
        CryptoManager::marshallKey(key) << "\n";
        if (VERBOSE_G) {LOG(mp) << "using encforreturned: get key " <<
                        encForField << " val " << val <<
                        " encforreturned index " <<
                        tmkm.encForReturned[encForField] << "\n"; }
        assert_s(
            key.length() > 0, "access manager does not have needed key\n");
        return key;
    }

    assert_s(false, "cryptdb does not have keys to decrypt query result \n");
    return NULL;
}

void
MKM::cleanup()
{
    encForMap.clear();
    reverseEncFor.clear();
    condAccess.clear();
}

void
TempMKM::cleanup()
{
    encForVal.clear();
    encForReturned.clear();
    principalsSeen.clear();
    returnBitMap.clear();
}

