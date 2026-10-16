// JsonTabularize.h — flatten "tabular" JSON into arrays of flat objects and
// render CSV. Port of JsonTabularize.cs; the schema argument is the internal
// form produced by JsonSchemaMaker::BuildSchema.

#pragma once

#include "JNode.h"

namespace jsontools {

enum class JsonTabularizerStrategy { DEFAULT, FULL_RECURSIVE, NO_RECURSION, STRINGIFY_ITERABLES };

enum JsonFormat : uint8_t {
    JF_BAD = 1,
    JF_REC = 2,   // array of flat objects
    JF_ROW = 4,   // object/array with only scalar values
    JF_SCAL = 8,
    JF_SQR = 16,  // array of arrays of scalars
    JF_TAB = 32,  // object mapping keys to scalars and arrays of scalars
    JF_ANY_TABLE = JF_SQR | JF_REC | JF_TAB,
};

class JsonTabularizer {
public:
    JsonTabularizerStrategy strategy;

    explicit JsonTabularizer(JsonTabularizerStrategy strategy_ = JsonTabularizerStrategy::DEFAULT)
        : strategy(strategy_) {}

    JsonFormat ClassifySchema(const JNodePtr& schema);
    // list of (path array as JArray, format) for all tables found in the schema
    std::vector<std::pair<JNodePtr, JsonFormat>> FindTabsInSchema(const JNodePtr& schema);
    std::shared_ptr<JArray> BuildTable(const JNodePtr& obj, const JNodePtr& schema,
                                       const std::string& keySep = ".");
    std::string TableToCsv(JArray* table, char delim = ',', char quote = '"',
                           const std::string& newline = "\n",
                           const std::vector<std::string>* header = nullptr,
                           bool boolsAsInts = false);

    static void ApplyQuotesIfNeeded(std::string& sb, const std::string& s, char delim, char quote);
    static void CsvStringToSb(std::string& sb, const JNodePtr& jnode, char delim, char quote,
                              bool boolsAsInts);

private:
    void FindTabsInSchemaHelper(const JNodePtr& schema, std::vector<JNodePtr>& path,
                                std::vector<std::pair<JNodePtr, JsonFormat>>& tabPaths);
    OrderedMap ResolveHang(const OrderedMap& hangingRow, const std::string& keySep);
    void AnyTableToRecord(const JNodePtr& obj, JsonFormat cls, const OrderedMap& restOfRow,
                          std::vector<JNodePtr>& result, std::vector<std::string>& superKeylist,
                          const std::string& keySep);
    void BuildTableHelper_DEFAULT(const JNodePtr& json, JsonFormat cls, size_t depth,
                                  JArray* tabPath, std::vector<JNodePtr>& result,
                                  const OrderedMap& restOfRow,
                                  std::vector<std::string>& superKeylist,
                                  const std::string& keySep);
    void FlattenSingleRow_FULL_RECURSIVE(const JNodePtr& row, const std::string& keySep,
                                         OrderedMap& restOfRow,
                                         std::vector<std::string>& superKeylist);
    void BuildTableHelper_FULL_RECURSIVE(const JNodePtr& obj, std::vector<JNodePtr>& result,
                                         const std::string& keySep);
    void BuildTableHelper_STRINGIFY_ITERABLES(const JNodePtr& obj, std::vector<JNodePtr>& result,
                                              const std::string& keySep);
};

} // namespace jsontools
